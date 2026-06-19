#pragma once

#define NOMINMAX

#include "SandboxLoader/Modules/IModule.h"
#include "SandboxLoader/Containers/SshotContainer.h"

// miniz: single-header zlib-compatible compression, no install required.
// We include the .c directly here so there is no separate compilation unit.
// In a real project you would add miniz.c to the project's source list instead.
#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#include "SandboxLoader/Dependencies/miniz.h"

#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstring>

#include <wincodec.h>
#include <shlwapi.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shlwapi.lib")

#undef min
#undef max

// ─────────────────────────────────────────────────────────────────────────────
// ScreenshotModule  —  GDI capture + XOR delta + zlib + .sshot v2
//
// PER-FRAME PIPELINE
// ───────────────────
//
//  Every frame:
//   1. GDI BitBlt → raw 24-bit BGR screen pixels (same as always)
//   2. Delta detect → dirty bounding rect  (unchanged)
//   3. Extract crop as top-down RGB buffer  (unchanged, flip Y + swap BGR→RGB)
//
//  Keyframe (every KEYFRAME_INTERVAL frames, or first frame):
//   4K. WIC JPEG-encode the crop → payload
//   5K. Write FrameHeader(KEYFRAME) + payload to .sshot
//   6K. Paint crop onto m_canvas at (crop_x, crop_y)
//
//  Delta frame:
//   4D. XOR crop pixels against m_canvas at (crop_x, crop_y)
//          xor_buf[i] = crop_rgb[i] ^ canvas_rgb[(crop_y+y)*W + (crop_x+x)]
//       Result: zeros where nothing changed, signal only where pixels differ.
//   5D. zlib-compress xor_buf → payload  (MZ_BEST_COMPRESSION level 9)
//   6D. Write FrameHeader(DELTA) + payload to .sshot
//   7D. Apply XOR to m_canvas to advance it to current state
//          canvas_rgb[...] ^= xor_buf[...]
//
// WHY XOR INSTEAD OF SUBTRACT?
// ─────────────────────────────
// XOR is its own inverse: decode is identical to encode (just XOR again).
// No clamping, no sign handling, no overflow — a single operation for both
// encode and decode.  The tradeoff vs subtract is that XOR doesn't produce
// smooth gradients in the diff, but for screen content (sharp pixel changes)
// it compresses just as well under zlib because unchanged pixels are
// exactly 0x00 regardless of the mathematical operation used.
//
// CANVAS MODEL
// ─────────────
// m_canvas is a full-screen top-down RGB buffer (W * H * 3 bytes).
// It always holds the reconstructed state of the last committed frame.
// The Python parser maintains an identical canvas during reconstruction.
// They stay in sync because both apply the same XOR operations in the
// same order.
//
// SIZE EXPECTATIONS
// ──────────────────
// Keyframe:  JPEG quality 55 of dirty crop → 20–80 KB
// Delta:     zlib of XOR buffer where most bytes are 0x00
//            → user typing in one window: 0.5–5 KB
//            → window drag/animation:     5–30 KB
//            → full screen change:        ~same as keyframe (worst case)
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T* () { return p; }
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

class ScreenshotModule : public IModule
{
public:
    explicit ScreenshotModule(UINT intervalSeconds = 30, UINT jpegQuality = 55)
        : m_intervalMs(intervalSeconds * 1000)
        , m_jpegQuality(jpegQuality)
    {}

    std::wstring Name() const override { return L"ScreenshotModule"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void OnCreate(HWND hwnd) override
    {
        s_instance = this;

        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInit = SUCCEEDED(hr) || hr == S_FALSE;

        hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
            reinterpret_cast<void**>(&m_factory));

        if (FAILED(hr))
        {
            std::cerr << "[ScreenshotModule] WIC init failed hr="
                << std::hex << hr << "\n";
            return;
        }

        ::CreateDirectoryW(LR"(C:\tmp)", nullptr);
        InitArchive();

        ::SetTimer(hwnd, TIMER_ID, m_intervalMs, nullptr);

        m_hWinEvent = ::SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        std::wcout << L"[ScreenshotModule] Ready. Archive=" << m_archivePath
            << L" quality=" << m_jpegQuality
            << L" keyframe-every=" << KEYFRAME_INTERVAL << L"\n";

        CaptureAndAppend();
    }

    bool OnMessage(HWND, UINT uMsg, WPARAM wParam, LPARAM) override
    {
        if (uMsg == WM_TIMER && wParam == TIMER_ID)
        {
            CaptureAndAppend();
            return true;
        }
        return false;
    }

    void OnDestroy(HWND hwnd) override
    {
        ::KillTimer(hwnd, TIMER_ID);
        if (m_hWinEvent) { ::UnhookWinEvent(m_hWinEvent); m_hWinEvent = nullptr; }
        delete[] m_prevPixels; m_prevPixels = nullptr;
        delete[] m_canvas;     m_canvas = nullptr;
        if (m_factory.p) { m_factory.p->Release(); m_factory.p = nullptr; }
        if (m_comInit) ::CoUninitialize();
        s_instance = nullptr;
    }

private:
    static constexpr UINT_PTR TIMER_ID = 1001;

    UINT      m_intervalMs;
    UINT      m_jpegQuality;
    bool      m_comInit = false;
    ComPtr<IWICImagingFactory> m_factory;
    HWINEVENTHOOK m_hWinEvent = nullptr;

    std::wstring m_archivePath = LR"(C:\tmp\monitor.sshot)";

    // GDI capture buffer (bottom-up BGR from GetDIBits)
    BYTE* m_prevPixels = nullptr;
    int   m_prevW = 0, m_prevH = 0;

    // Reconstructed canvas — top-down RGB, full screen, mirrors parser state
    BYTE* m_canvas = nullptr;
    int   m_canvasW = 0;
    int   m_canvasH = 0;

    uint32_t m_frameCount = 0; // total frames written (drives keyframe schedule)

    static ScreenshotModule* s_instance;

    // ── Foreground hook ───────────────────────────────────────────────────────
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD)
    {
        if (!s_instance) return;
        wchar_t t[256] = {};
        ::GetWindowTextW(hwnd, t, 256);
        std::wcout << L"[ScreenshotModule] Foreground → \"" << t << L"\"\n";
        s_instance->CaptureAndAppend();
    }

    // ── Archive init ──────────────────────────────────────────────────────────
    void InitArchive()
    {
        DWORD attr = ::GetFileAttributesW(m_archivePath.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES)
        {
            // Count existing frames so keyframe schedule continues correctly
            m_frameCount = ReadExistingCount();
            std::wcout << L"[ScreenshotModule] Appending to existing archive ("
                << m_frameCount << L" frames)\n";
            return;
        }

        HANDLE hf = ::CreateFileW(m_archivePath.c_str(), GENERIC_WRITE, 0,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return;

        SshotFileHeader fh{};
        DWORD w{};
        ::WriteFile(hf, &fh, sizeof(fh), &w, nullptr);
        ::CloseHandle(hf);
        std::wcout << L"[ScreenshotModule] Created archive: " << m_archivePath << L"\n";
    }

    uint32_t ReadExistingCount()
    {
        HANDLE hf = ::CreateFileW(m_archivePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE) return 0;
        uint32_t count = 0;
        ::SetFilePointer(hf, SSHOT_COUNT_OFFSET, nullptr, FILE_BEGIN);
        DWORD r{};
        ::ReadFile(hf, &count, sizeof(count), &r, nullptr);
        ::CloseHandle(hf);
        return count;
    }

    // ── Master capture pipeline ───────────────────────────────────────────────
    void CaptureAndAppend()
    {
        if (!m_factory.p) return;

        // ── STEP 1: GDI full-screen capture ───────────────────────────────────
        const int W = ::GetSystemMetrics(SM_CXSCREEN);
        const int H = ::GetSystemMetrics(SM_CYSCREEN);

        HDC hdcScreen = ::GetDC(nullptr);
        HDC hdcMem = ::CreateCompatibleDC(hdcScreen);
        HBITMAP hBmp = ::CreateCompatibleBitmap(hdcScreen, W, H);
        HBITMAP hOld = static_cast<HBITMAP>(::SelectObject(hdcMem, hBmp));
        ::BitBlt(hdcMem, 0, 0, W, H, hdcScreen, 0, 0, SRCCOPY);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi); bi.biWidth = W; bi.biHeight = H;
        bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;

        const DWORD stride = ((W * 3 + 3) & ~3);
        std::vector<BYTE> cur(stride * H);
        ::GetDIBits(hdcMem, hBmp, 0, H, cur.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

        ::SelectObject(hdcMem, hOld);
        ::DeleteObject(hBmp);
        ::DeleteDC(hdcMem);
        ::ReleaseDC(nullptr, hdcScreen);

        // ── STEP 2: Delta detection → dirty rect ──────────────────────────────
        RECT dirty = { W, H, 0, 0 };
        bool anyChange = false;

        if (m_prevPixels && m_prevW == W && m_prevH == H)
        {
            for (int y = 0; y < H; ++y)
            {
                const BYTE* c = cur.data() + y * stride;
                const BYTE* p = m_prevPixels + y * stride;
                for (int x = 0; x < W; ++x)
                {
                    if (c[x * 3] != p[x * 3] || c[x * 3 + 1] != p[x * 3 + 1] || c[x * 3 + 2] != p[x * 3 + 2])
                    {
                        dirty.left = std::min(dirty.left, (LONG)x);
                        dirty.top = std::min(dirty.top, (LONG)y);
                        dirty.right = std::max(dirty.right, (LONG)(x + 1));
                        dirty.bottom = std::max(dirty.bottom, (LONG)(y + 1));
                        anyChange = true;
                    }
                }
            }
        }
        else
        {
            dirty = { 0, 0, (LONG)W, (LONG)H };
            anyChange = true;
        }

        // Update GDI prev-frame buffer
        if (!m_prevPixels || m_prevW != W || m_prevH != H)
        {
            delete[] m_prevPixels;
            m_prevPixels = new BYTE[stride * H];
            m_prevW = W; m_prevH = H;
        }
        std::memcpy(m_prevPixels, cur.data(), stride * H);

        if (!anyChange)
        {
            std::wcout << L"[ScreenshotModule] No change, skipping\n";
            return;
        }

        // Ensure canvas is allocated (full-screen top-down RGB)
        if (!m_canvas || m_canvasW != W || m_canvasH != H)
        {
            delete[] m_canvas;
            m_canvas = new BYTE[W * H * 3]();  // zero-initialised
            m_canvasW = W; m_canvasH = H;
            // Force a keyframe whenever canvas is (re-)allocated
            m_frameCount = 0;
        }

        const int cropW = dirty.right - dirty.left;
        const int cropH = dirty.bottom - dirty.top;

        // ── STEP 3: Build top-down RGB crop (flip Y, swap BGR→RGB) ────────────
        // Layout: tightly packed, cropW*3 bytes per row, no padding.
        const int rgbStride = cropW * 3;
        std::vector<BYTE> cropRgb(rgbStride * cropH);

        for (int y = 0; y < cropH; ++y)
        {
            // DIB is bottom-up: screen row (dirty.top+y) is at DIB row (H-1-(dirty.top+y))
            int dibY = (H - 1) - (dirty.top + y);
            const BYTE* src = cur.data() + dibY * stride + dirty.left * 3;
            BYTE* dst = cropRgb.data() + y * rgbStride;
            for (int x = 0; x < cropW; ++x)
            {
                dst[x * 3 + 0] = src[x * 3 + 2]; // R ← B
                dst[x * 3 + 1] = src[x * 3 + 1]; // G
                dst[x * 3 + 2] = src[x * 3 + 0]; // B ← R
            }
        }

        // ── Decide frame type ─────────────────────────────────────────────────
        bool isKeyframe = (m_frameCount % KEYFRAME_INTERVAL == 0);

        if (isKeyframe)
            WriteKeyframe(cropRgb, dirty, cropW, cropH, W);
        else
            WriteDelta(cropRgb, dirty, cropW, cropH, W);

        ++m_frameCount;
    }

    // ── KEYFRAME: JPEG-encode crop, paint onto canvas ─────────────────────────
    void WriteKeyframe(const std::vector<BYTE>& cropRgb,
        const RECT& dirty, int cropW, int cropH, int W)
    {
        // Encode crop to JPEG in memory
        std::vector<BYTE> jpeg = EncodeJpeg(cropRgb, cropW, cropH);

        // Paint crop onto canvas so delta frames can diff against it
        PaintOntoCanvas(cropRgb, dirty, cropW, cropH, W);

        // Append to archive
        AppendFrame(jpeg.data(), (uint32_t)jpeg.size(),
            dirty.left, dirty.top, cropW, cropH,
            FRAME_TYPE_KEYFRAME);

        std::wcout << L"[ScreenshotModule] KEYFRAME #" << m_frameCount
            << L" " << cropW << L"x" << cropH
            << L" → " << jpeg.size() / 1024 << L" KB\n";
    }

    // ── DELTA: XOR crop against canvas, zlib-compress ─────────────────────────
    void WriteDelta(const std::vector<BYTE>& cropRgb,
        const RECT& dirty, int cropW, int cropH, int W)
    {
        const int rgbStride = cropW * 3;
        const int xorBufBytes = rgbStride * cropH;

        // ── STEP 4D: XOR crop pixels against canvas ───────────────────────────
        //
        // Canvas is full-screen top-down RGB, stride = W*3 (no padding).
        // For canvas pixel at screen position (dirty.left+x, dirty.top+y):
        //   canvas_offset = (dirty.top + y) * W * 3 + (dirty.left + x) * 3
        //
        // xor_buf[y * cropW * 3 + x * 3 + ch]
        //   = cropRgb[y * cropW * 3 + x * 3 + ch]
        //   ^ canvas[(dirty.top+y)*W*3 + (dirty.left+x)*3 + ch]
        //
        // Pixels that didn't change → 0x00 in xor_buf.
        // zlib sees long runs of zeros → extremely high compression ratio.

        std::vector<BYTE> xorBuf(xorBufBytes);

        for (int y = 0; y < cropH; ++y)
        {
            const BYTE* crop = cropRgb.data() + y * rgbStride;
            const BYTE* canvas = m_canvas
                + (dirty.top + y) * m_canvasW * 3
                + dirty.left * 3;
            BYTE* xor_ = xorBuf.data() + y * rgbStride;

            for (int b = 0; b < rgbStride; ++b)
                xor_[b] = crop[b] ^ canvas[b];
        }

        // ── STEP 5D: zlib-compress the XOR buffer ─────────────────────────────
        //
        // mz_compress2 is miniz's one-shot zlib compression.
        // MZ_BEST_COMPRESSION (level 9) maximises ratio at the cost of CPU.
        // For a monitoring tool that runs in the background, this is fine.
        //
        // The compressed output is at most mz_compressBound(xorBufBytes) bytes.
        // In practice for XOR-delta data it will be much smaller.

        mz_ulong compBound = mz_compressBound((mz_ulong)xorBufBytes);
        std::vector<BYTE> compressed(compBound);
        mz_ulong compSize = compBound;

        int mzResult = mz_compress2(
            compressed.data(), &compSize,
            xorBuf.data(), (mz_ulong)xorBufBytes,
            MZ_BEST_COMPRESSION);

        if (mzResult != MZ_OK)
        {
            std::cerr << "[ScreenshotModule] zlib compress failed: " << mzResult << "\n";
            // Fall back to a keyframe on compress failure
            WriteKeyframe(cropRgb, dirty, cropW, cropH, W);
            return;
        }
        compressed.resize(compSize);

        // ── STEP 6D: Update canvas ─────────────────────────────────────────────
        // Apply the same XOR to advance the canvas to the current frame.
        // After this, canvas matches the current screen state in the crop region.
        PaintOntoCanvas(cropRgb, dirty, cropW, cropH, W);

        // ── STEP 7D: Append to archive ─────────────────────────────────────────
        AppendFrame(compressed.data(), (uint32_t)compressed.size(),
            dirty.left, dirty.top, cropW, cropH,
            FRAME_TYPE_DELTA);

        std::wcout << L"[ScreenshotModule] DELTA    #" << m_frameCount
            << L" " << cropW << L"x" << cropH
            << L" raw=" << xorBufBytes / 1024 << L" KB"
            << L" → " << compSize / 1024 << L" KB"
            << L" (" << (100 * compSize / xorBufBytes) << L"% of raw)\n";
    }

    // ── Paint a crop into the canvas (used for both keyframe and delta) ────────
    // After this call, canvas[dirty.top..dirty.bottom][dirty.left..dirty.right]
    // reflects the current screen state in that region.
    void PaintOntoCanvas(const std::vector<BYTE>& cropRgb,
        const RECT& dirty, int cropW, int cropH, int /*W*/)
    {
        const int rgbStride = cropW * 3;
        for (int y = 0; y < cropH; ++y)
        {
            const BYTE* src = cropRgb.data() + y * rgbStride;
            BYTE* dst = m_canvas
                + (dirty.top + y) * m_canvasW * 3
                + dirty.left * 3;
            std::memcpy(dst, src, rgbStride);
        }
    }

    // ── JPEG encode via WIC (in-memory) ───────────────────────────────────────
    std::vector<BYTE> EncodeJpeg(const std::vector<BYTE>& rgb, int w, int h)
    {
        ComPtr<IStream> mem;
        ::CreateStreamOnHGlobal(nullptr, TRUE, &mem);

        ComPtr<IWICBitmapEncoder>      enc;
        ComPtr<IWICBitmapFrameEncode>  frame;
        ComPtr<IPropertyBag2>          props;

        m_factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc);
        enc->Initialize(mem, WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&frame, &props);

        // Set quality
        PROPBAG2 pb{}; pb.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT  vq{};  vq.vt = VT_R4;
        vq.fltVal = static_cast<float>(m_jpegQuality) / 100.0f;
        props->Write(1, &pb, &vq);

        frame->Initialize(props);
        frame->SetSize(w, h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppRGB;
        frame->SetPixelFormat(&fmt);

        ComPtr<IWICBitmap> bmp;
        m_factory->CreateBitmapFromMemory(
            w, h, GUID_WICPixelFormat24bppRGB,
            w * 3, w * 3 * h,
            const_cast<BYTE*>(rgb.data()), &bmp);

        frame->WriteSource(bmp, nullptr);
        frame->Commit();
        enc->Commit();

        HGLOBAL hg{};
        ::GetHGlobalFromStream(mem, &hg);
        SIZE_T  sz = ::GlobalSize(hg);
        void* ptr = ::GlobalLock(hg);

        std::vector<BYTE> out(static_cast<BYTE*>(ptr),
            static_cast<BYTE*>(ptr) + sz);
        ::GlobalUnlock(hg);
        return out;
    }

    // ── Append one frame record to the .sshot file ────────────────────────────
    void AppendFrame(const void* payload, uint32_t payloadSize,
        int32_t x, int32_t y, int32_t w, int32_t h,
        uint8_t frameType)
    {
        HANDLE hf = ::CreateFileW(m_archivePath.c_str(),
            GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf == INVALID_HANDLE_VALUE)
        {
            std::cerr << "[ScreenshotModule] Cannot open archive, GLE="
                << ::GetLastError() << "\n";
            return;
        }

        // Seek to end, write FrameHeader + payload
        ::SetFilePointer(hf, 0, nullptr, FILE_END);

        SshotFrameHeader fh{};
        fh.timestamp = static_cast<int64_t>(std::time(nullptr));
        fh.crop_x = x;  fh.crop_y = y;
        fh.crop_w = w;  fh.crop_h = h;
        fh.payload_size = payloadSize;
        fh.frame_type = frameType;

        DWORD written{};
        ::WriteFile(hf, &fh, sizeof(fh), &written, nullptr);
        ::WriteFile(hf, payload, payloadSize, &written, nullptr);

        // Update count in file header
        ::SetFilePointer(hf, SSHOT_COUNT_OFFSET, nullptr, FILE_BEGIN);
        uint32_t count = 0; DWORD rd{};
        ::ReadFile(hf, &count, sizeof(count), &rd, nullptr);
        ++count;
        ::SetFilePointer(hf, SSHOT_COUNT_OFFSET, nullptr, FILE_BEGIN);
        ::WriteFile(hf, &count, sizeof(count), &written, nullptr);

        ::CloseHandle(hf);
    }
};

inline ScreenshotModule* ScreenshotModule::s_instance = nullptr;