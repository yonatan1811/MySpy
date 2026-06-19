#!/usr/bin/env python3
"""
parser.py  —  .sshot v2 inter-frame screenshot archive extractor

USAGE
─────
  python parser.py monitor.sshot                  # list all frames
  python parser.py monitor.sshot --extract        # extract all as JPEG
  python parser.py monitor.sshot --extract --frame 42
  python parser.py monitor.sshot --extract --from "2026-06-13 10:00"
  python parser.py monitor.sshot --watch          # live tail mode

RECONSTRUCTION MODEL
────────────────────
The parser maintains a "canvas" — a full-screen PIL Image in RGB mode.
This mirrors exactly what m_canvas does in ScreenshotModule.

  KEYFRAME:
    1. Decode the JPEG payload → PIL image (the crop)
    2. Paste the crop onto canvas at (crop_x, crop_y)
    3. Canvas now shows the screen as of this frame

  DELTA:
    1. zlib.decompress(payload) → xor_buf  (crop_w * crop_h * 3 bytes)
    2. Crop the current canvas region at (crop_x, crop_y, crop_w, crop_h)
       → prev_crop as raw bytes
    3. XOR xor_buf against prev_crop byte-by-byte → new_crop_bytes
    4. Build a PIL image from new_crop_bytes
    5. Paste onto canvas at (crop_x, crop_y)
    6. Canvas now shows the screen as of this frame

The canvas starts as a black (zero) image.
The C++ writer initialises m_canvas with `new BYTE[W*H*3]()` (zero),
so both sides start from the same state.

IMPORTANT: frames MUST be processed in order. You cannot seek to a delta
frame directly — you must replay all previous frames to reconstruct the
canvas state at that point. Keyframes (every 30 frames by default) reset
the dependency chain.
"""

import struct
import sys
import os
import zlib
import time
import argparse
from io import BytesIO
from datetime import datetime, timezone
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("ERROR: Pillow is required.  Run:  pip install Pillow", file=sys.stderr)
    sys.exit(1)

# ── Format constants (must match SshotContainer.h exactly) ───────────────────
FILE_HEADER_FMT   = "<4sHHII"           # 16 bytes
FILE_HEADER_SIZE  = struct.calcsize(FILE_HEADER_FMT)

FRAME_HEADER_FMT  = "<qiiiIIB3s"        # 32 bytes
FRAME_HEADER_SIZE = struct.calcsize(FRAME_HEADER_FMT)

MAGIC             = b"SSv2"
FRAME_KEYFRAME    = 0x01
FRAME_DELTA       = 0x02

assert FILE_HEADER_SIZE  == 16, f"FileHeader size mismatch: {FILE_HEADER_SIZE}"
assert FRAME_HEADER_SIZE == 32, f"FrameHeader size mismatch: {FRAME_HEADER_SIZE}"


# ── Data classes ──────────────────────────────────────────────────────────────
class FileHeader:
    def __init__(self, raw: bytes):
        magic, ver, _r0, count, _r1 = struct.unpack(FILE_HEADER_FMT, raw)
        if magic != MAGIC:
            raise ValueError(f"Bad magic {magic!r}, expected {MAGIC!r}. "
                             f"Is this a v2 .sshot file?")
        self.version = ver
        self.count   = count


class FrameHeader:
    def __init__(self, raw: bytes):
        ts, x, y, w, h, psz, ftype, _r = struct.unpack(FRAME_HEADER_FMT, raw)
        self.timestamp    = ts
        self.crop_x       = x
        self.crop_y       = y
        self.crop_w       = w
        self.crop_h       = h
        self.payload_size = psz
        self.frame_type   = ftype

    @property
    def dt(self) -> datetime:
        return datetime.fromtimestamp(self.timestamp, tz=timezone.utc).astimezone()

    @property
    def type_str(self) -> str:
        return "KEY  " if self.frame_type == FRAME_KEYFRAME else "DELTA"

    def __repr__(self):
        return (f"<{self.type_str} {self.dt:%Y-%m-%d %H:%M:%S} "
                f"({self.crop_x},{self.crop_y}) {self.crop_w}×{self.crop_h} "
                f"{self.payload_size/1024:.1f} KB>")


# ── Canvas — the stateful reconstructor ──────────────────────────────────────
class Canvas:
    """
    Mirrors m_canvas in ScreenshotModule.
    Starts as a black image; grows lazily on the first keyframe.
    """
    def __init__(self):
        self.img: Image.Image | None = None  # full-screen RGB PIL image

    def _ensure(self, w: int, h: int):
        """Allocate or resize canvas to fit (w, h)."""
        if self.img is None or self.img.size != (w, h):
            self.img = Image.new("RGB", (w, h), (0, 0, 0))

    def apply_keyframe(self, header: FrameHeader, jpeg_bytes: bytes):
        """
        Decode JPEG and paste onto canvas.
        We infer full-screen size from crop_x + crop_w / crop_y + crop_h
        (worst case: first frame is always full-screen).
        """
        crop = Image.open(BytesIO(jpeg_bytes)).convert("RGB")

        # Grow canvas if needed (first frame gives us true screen dimensions)
        screen_w = max(self.img.size[0] if self.img else 0,
                       header.crop_x + header.crop_w)
        screen_h = max(self.img.size[1] if self.img else 0,
                       header.crop_y + header.crop_h)
        self._ensure(screen_w, screen_h)

        self.img.paste(crop, (header.crop_x, header.crop_y))

    def apply_delta(self, header: FrameHeader, compressed: bytes):
        """
        Decompress the XOR buffer, apply it to the canvas crop region.

        XOR decode:
          new_pixel_byte = xor_buf_byte ^ old_canvas_pixel_byte

        This is the inverse of what the C++ encoder does:
          xor_buf_byte = new_pixel_byte ^ old_canvas_pixel_byte
        And since XOR is its own inverse:
          new = xor ^ old  ↔  xor = new ^ old
        """
        if self.img is None:
            raise ValueError(
                f"DELTA frame at index with no prior keyframe — "
                f"archive may be corrupt or first frame was not a keyframe.")

        # 1. Decompress
        xor_buf = zlib.decompress(compressed)
        expected = header.crop_w * header.crop_h * 3
        if len(xor_buf) != expected:
            raise ValueError(
                f"XOR buffer size mismatch: got {len(xor_buf)}, "
                f"expected {expected} ({header.crop_w}×{header.crop_h}×3)")

        # 2. Extract the current canvas region as raw bytes
        crop_region = self.img.crop((
            header.crop_x,
            header.crop_y,
            header.crop_x + header.crop_w,
            header.crop_y + header.crop_h,
        ))
        old_bytes = crop_region.tobytes()  # packed RGB, top-down, no padding

        # 3. XOR old bytes with xor_buf → new pixel bytes
        new_bytes = bytes(a ^ b for a, b in zip(old_bytes, xor_buf))

        # 4. Build new crop image and paste back onto canvas
        new_crop = Image.frombytes(
            "RGB", (header.crop_w, header.crop_h), new_bytes)
        self.img.paste(new_crop, (header.crop_x, header.crop_y))

    def snapshot(self) -> Image.Image:
        """Return a copy of the current canvas state."""
        if self.img is None:
            raise ValueError("Canvas is empty — no frames processed yet")
        return self.img.copy()


# ── Low-level reader ──────────────────────────────────────────────────────────
def read_frames(path: str):
    """
    Generator: yields (FileHeader,) first, then (index, FrameHeader, payload_bytes).
    Reads sequentially — safe to use while the monitor is writing.
    """
    with open(path, "rb") as f:
        raw_fh = f.read(FILE_HEADER_SIZE)
        if len(raw_fh) < FILE_HEADER_SIZE:
            raise ValueError("File too small")
        yield FileHeader(raw_fh)

        idx = 0
        while True:
            raw_frame = f.read(FRAME_HEADER_SIZE)
            if not raw_frame:
                break
            if len(raw_frame) < FRAME_HEADER_SIZE:
                print(f"[warn] Truncated frame header at #{idx}", file=sys.stderr)
                break
            fh = FrameHeader(raw_frame)
            payload = f.read(fh.payload_size)
            if len(payload) < fh.payload_size:
                print(f"[warn] Truncated payload at #{idx}", file=sys.stderr)
                break
            yield idx, fh, payload
            idx += 1


# ── Reconstruct all frames up to a target index ───────────────────────────────
def reconstruct_up_to(path: str, target_idx: int | None = None):
    """
    Replay the archive through the canvas model.
    Yields (index, FrameHeader, Canvas snapshot) for every frame
    (or only at target_idx if specified).
    """
    canvas = Canvas()
    gen = read_frames(path)
    next(gen)  # consume FileHeader

    for idx, fh, payload in gen:
        if fh.frame_type == FRAME_KEYFRAME:
            canvas.apply_keyframe(fh, payload)
        elif fh.frame_type == FRAME_DELTA:
            canvas.apply_delta(fh, payload)
        else:
            print(f"[warn] Unknown frame type 0x{fh.frame_type:02x} at #{idx}",
                  file=sys.stderr)
            continue

        if target_idx is None or idx == target_idx:
            yield idx, fh, canvas.snapshot()

        if target_idx is not None and idx >= target_idx:
            break


# ── Commands ──────────────────────────────────────────────────────────────────
def cmd_list(path: str):
    gen  = read_frames(path)
    fh   = next(gen)
    print(f"Archive : {path}")
    print(f"Version : {fh.version}  |  Frames in header: {fh.count}")
    print()
    print(f"{'#':>5}  {'Type':5}  {'Timestamp':>22}  "
          f"{'X':>5} {'Y':>5}  {'W':>5} {'H':>5}  {'Payload KB':>10}")
    print("─" * 75)

    total_raw = total_compressed = 0
    for idx, frame, _ in gen:
        raw_kb  = frame.crop_w * frame.crop_h * 3 / 1024
        comp_kb = frame.payload_size / 1024
        ratio   = f"{comp_kb/raw_kb*100:.0f}%" if raw_kb > 0 else "—"
        print(f"{idx:>5}  {frame.type_str}  {frame.dt:%Y-%m-%d %H:%M:%S}  "
              f"{frame.crop_x:>5} {frame.crop_y:>5}  "
              f"{frame.crop_w:>5} {frame.crop_h:>5}  "
              f"{comp_kb:>7.1f} KB ({ratio})")
        total_compressed += frame.payload_size
        total_raw        += int(raw_kb * 1024)

    print("─" * 75)
    if total_raw > 0:
        print(f"Total payload : {total_compressed/1024:.1f} KB  "
              f"({total_compressed/1024/1024:.2f} MB)")
        print(f"Uncompressed  : {total_raw/1024:.1f} KB  "
              f"({total_raw/1024/1024:.2f} MB)")
        print(f"Overall ratio : {total_compressed/total_raw*100:.1f}%")


def cmd_extract(path: str, outdir: str,
                from_dt: datetime | None, to_dt: datetime | None,
                only_frame: int | None, quality: int = 85):
    Path(outdir).mkdir(parents=True, exist_ok=True)
    extracted = 0

    for idx, fh, snapshot in reconstruct_up_to(path, only_frame):
        if from_dt and fh.dt < from_dt:
            continue
        if to_dt   and fh.dt > to_dt:
            continue

        fname = (f"{idx:04d}_{fh.dt:%Y%m%d_%H%M%S}"
                 f"_{fh.type_str.strip()}"
                 f"_x{fh.crop_x}_y{fh.crop_y}"
                 f"_{fh.crop_w}x{fh.crop_h}.jpg")
        out_path = os.path.join(outdir, fname)

        # Save canvas snapshot (full reconstructed screen) as JPEG
        snapshot.save(out_path, "JPEG", quality=quality)
        size_kb = os.path.getsize(out_path) / 1024
        print(f"[{fh.type_str}] #{idx:04d} → {fname}  ({size_kb:.1f} KB on disk)")
        extracted += 1

        if only_frame is not None:
            break

    print(f"\nExtracted {extracted} frame(s) → {outdir}")


def cmd_watch(path: str, outdir: str, quality: int = 85):
    Path(outdir).mkdir(parents=True, exist_ok=True)
    print(f"[watch] Tailing {path} → {outdir}  (Ctrl+C to stop)")

    seen_idx = -1

    while True:
        try:
            # Rebuild canvas from scratch each poll so we stay consistent.
            # In a production tool you'd cache canvas state — but for a learning
            # project clarity beats cleverness.
            for idx, fh, snapshot in reconstruct_up_to(path):
                if idx > seen_idx:
                    fname = (f"{idx:04d}_{fh.dt:%Y%m%d_%H%M%S}"
                             f"_{fh.type_str.strip()}.jpg")
                    out_path = os.path.join(outdir, fname)
                    snapshot.save(out_path, "JPEG", quality=quality)
                    print(f"[watch] #{idx:04d} {fh.type_str} → {fname} "
                          f"({os.path.getsize(out_path)/1024:.1f} KB)")
                    seen_idx = idx
        except (FileNotFoundError, ValueError):
            pass

        time.sleep(1.0)


# ── CLI ───────────────────────────────────────────────────────────────────────
def parse_dt(s: str) -> datetime:
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
        try:
            return datetime.strptime(s, fmt).astimezone()
        except ValueError:
            continue
    raise argparse.ArgumentTypeError(f"Cannot parse '{s}'")


def main():
    ap = argparse.ArgumentParser(description=".sshot v2 archive parser")
    ap.add_argument("archive")
    ap.add_argument("--extract", action="store_true")
    ap.add_argument("--watch",   action="store_true")
    ap.add_argument("--outdir",  default="./extracted")
    ap.add_argument("--from",    dest="from_dt", type=parse_dt, metavar="DATETIME")
    ap.add_argument("--to",      dest="to_dt",   type=parse_dt, metavar="DATETIME")
    ap.add_argument("--frame",   dest="frame",   type=int, metavar="N")
    ap.add_argument("--quality", dest="quality", type=int, default=85,
                    help="JPEG quality for extracted snapshots (default 85)")
    args = ap.parse_args()

    if args.watch:
        cmd_watch(args.archive, args.outdir, args.quality)
    elif args.extract:
        cmd_extract(args.archive, args.outdir,
                    args.from_dt, args.to_dt, args.frame, args.quality)
    else:
        cmd_list(args.archive)


if __name__ == "__main__":
    main()