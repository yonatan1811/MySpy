#pragma once
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// .sshot v2  —  inter-frame compressed screenshot archive
//
// FILE LAYOUT
// ───────────
//   [FileHeader  16 B]          always at offset 0
//   [FrameHeader 32 B][payload] repeated N times, appended sequentially
//
// FRAME TYPES
// ────────────
//   KEYFRAME (0x01)
//     payload = raw JPEG of the dirty crop.
//     The decoder decodes this JPEG and paints it onto the canvas at
//     (crop_x, crop_y).  No dependency on any previous frame.
//     Written every KEYFRAME_INTERVAL frames, and always for the first frame.
//
//   DELTA (0x02)
//     payload = zlib-compressed XOR buffer.
//     The XOR buffer has exactly crop_w * crop_h * 3 bytes (24-bit RGB,
//     top-down, no row padding).
//     Each byte = current_pixel_byte XOR previous_canvas_pixel_byte.
//     Static pixels produce 0x00 — zlib compresses long runs of zeros to
//     almost nothing.  Only truly changed pixels carry meaningful entropy.
//
//     Decoder:
//       1. zlib-decompress payload → xor_buf (crop_w * crop_h * 3 bytes)
//       2. For each pixel at (crop_x+x, crop_y+y):
//            canvas[y][x] ^= xor_buf[y * crop_w + x]   (all 3 channels)
//
// FILEHEADER (16 bytes)
// ──────────────────────
//   magic[4]   "SSv2"
//   version    uint16 = 2
//   reserved   uint16
//   count      uint32   total frames written so far
//   reserved   uint32
//
// FRAMEHEADER (32 bytes)
// ───────────────────────
//   timestamp  int64    unix seconds UTC
//   crop_x     int32
//   crop_y     int32
//   crop_w     int32
//   crop_h     int32
//   payload_sz uint32   compressed bytes that follow
//   frame_type uint8    FRAME_TYPE_KEYFRAME or FRAME_TYPE_DELTA
//   reserved   uint8[3]
// ─────────────────────────────────────────────────────────────────────────────

static constexpr uint8_t FRAME_TYPE_KEYFRAME = 0x01;
static constexpr uint8_t FRAME_TYPE_DELTA = 0x02;

static constexpr uint32_t KEYFRAME_INTERVAL = 30; // one keyframe every N frames

#pragma pack(push, 1)

struct SshotFileHeader
{
    char     magic[4] = { 'S','S','v','2' };
    uint16_t version = 2;
    uint16_t reserved0 = 0;
    uint32_t count = 0;
    uint32_t reserved1 = 0;
};
static_assert(sizeof(SshotFileHeader) == 16, "");

struct SshotFrameHeader
{
    int64_t  timestamp = 0;
    int32_t  crop_x = 0;
    int32_t  crop_y = 0;
    int32_t  crop_w = 0;
    int32_t  crop_h = 0;
    uint32_t payload_size = 0;
    uint8_t  frame_type = 0;
    uint8_t  reserved[3] = {};
};
static_assert(sizeof(SshotFrameHeader) == 32, "");

#pragma pack(pop)

static constexpr DWORD SSHOT_COUNT_OFFSET =
static_cast<DWORD>(offsetof(SshotFileHeader, count)); // = 8