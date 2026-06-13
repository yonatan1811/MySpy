#pragma once
#define NOMINMAX

#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <vector>
#include <algorithm>

#include "SandboxLoader/Modules/IModule.h"

#include <shlwapi.h>
#include <wincodec.h> 
#include <wincodecsdk.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib , "Shlwapi.lib")

#undef min
#undef max



// ─────────────────────────────────────────────────────────────────────────────
// ScreenshotModule  —  GDI capture + WIC encode
//
// WHY WIC INSTEAD OF GDI+?
// ─────────────────────────
// GDI+ is frozen at version 1.1 (released ~2006). WIC is the API Microsoft
// replaced it with, and is actively maintained as part of Windows.
//
// Concrete advantages WIC gives us here:
//   • Better PNG compression — WIC's PNG encoder uses a higher default
//     DEFLATE effort than GDI+'s, typically 10–20% smaller files.
//   • Proper palette/quantization pipeline — WIC has first-class support
//     for palette generation and ordered/error-diffusion dithering via
//     IWICPalette, without needing the undocumented GdipInitializePalette.
//   • COM-based, composable — each step (source → transform → encoder)
//     is a distinct COM interface. Easy to swap in a JPEG or TIFF encoder
//     by changing one GUID.
//   • Actively updated — new formats (HEIF, AVIF via codec packs) are
//     added to WIC, never to GDI+.
//
// WIC COM OBJECT MODEL (what we use)
// ────────────────────────────────────
//   IWICImagingFactory          — the root factory, created once
//     └─ CreateBitmapFromMemory → IWICBitmap          (wraps our raw pixels)
//     └─ CreateEncoder          → IWICBitmapEncoder   (PNG encoder)
//          └─ CreateNewFrame    → IWICBitmapFrameEncode (one image frame)
//     └─ CreatePalette          → IWICPalette          (256-color table)
//
// PIPELINE (same three layers as before, WIC replaces GDI+ in layers 3+4)
// ─────────────────────────────────────────────────────────────────────────
//   GDI BitBlt          → raw 24-bit BGR pixels in memory
//   Delta detection     → dirty bounding rect (or skip if no change)
//   WIC IWICBitmap      → wrap the cropped pixels, convert BGR→RGB
//   IWICPalette         → derive optimal 256-color palette from the bitmap
//   IWICBitmapFrameEncode→ write palette + dithered pixels into PNG frame
//   IStream (file)      → output .png file
// ─────────────────────────────────────────────────────────────────────────────

// ── Smart pointer helpers for COM objects ────────────────────────────────────
// We use a minimal RAII wrapper so we don't need to #include <wrl/client.h>
// (which pulls in a lot of headers). For a real project, prefer ComPtr<T>.
template<typename T>
struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    operator T* () { return p; }
    // Disable copy — these are move-only resources
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
};

class ScreenshotModule : public IModule
{
public:
    explicit ScreenshotModule(UINT intervalSeconds = 30)
        : m_intervalMs(intervalSeconds * 1000)
    {}

    std::wstring Name() const override { return L"ScreenshotModule"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void OnCreate(HWND hwnd) override
    {
        s_instance = this;

        // WIC lives in the COM ecosystem — CoInitialize must be called on this
        // thread before any COM object can be created.
        // COINIT_APARTMENTTHREADED matches a single-threaded message-loop thread.
        HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInitialised = SUCCEEDED(hr) || hr == S_FALSE; // S_FALSE = already init

        // Create the WIC factory — this is the entry point for everything WIC.
        // CLSID_WICImagingFactory is the concrete class; IID_IWICImagingFactory
        // is the interface we want back.
        hr = ::CoCreateInstance(
            CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IWICImagingFactory,
            reinterpret_cast<void**>(&m_wicFactory));

        if (FAILED(hr))
        {
            std::cerr << "[ScreenshotModule] CoCreateInstance(WICFactory) failed, hr="
                << std::hex << hr << "\n";
            return;
        }
        std::wcout << L"[ScreenshotModule] WIC factory ready\n";

        // Periodic timer
        ::SetTimer(hwnd, TIMER_ID, m_intervalMs, nullptr);
        std::wcout << L"[ScreenshotModule] Timer=" << m_intervalMs / 1000 << L"s\n";

        // Foreground-change hook — WINEVENT_OUTOFCONTEXT means Windows delivers
        // events via our message queue, no DLL injection required.
        m_hWinEvent = ::SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        CaptureAndSave(L"startup");
    }

    bool OnMessage(HWND /*hwnd*/, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/) override
    {
        if (uMsg == WM_TIMER && wParam == TIMER_ID)
        {
            CaptureAndSave(L"timer");
            return true;
        }
        return false;
    }

    void OnDestroy(HWND hwnd) override
    {
        ::KillTimer(hwnd, TIMER_ID);

        if (m_hWinEvent) { ::UnhookWinEvent(m_hWinEvent); m_hWinEvent = nullptr; }

        delete[] m_prevPixels;
        m_prevPixels = nullptr;

        // Release COM objects before CoUninitialize
        if (m_wicFactory.p) { m_wicFactory.p->Release(); m_wicFactory.p = nullptr; }

        if (m_comInitialised)
            ::CoUninitialize();

        s_instance = nullptr;
    }

private:
    static constexpr UINT_PTR TIMER_ID = 1001;

    UINT              m_intervalMs;
    bool              m_comInitialised = false;
    ComPtr<IWICImagingFactory> m_wicFactory;
    HWINEVENTHOOK     m_hWinEvent = nullptr;

    // Previous frame for delta detection
    BYTE* m_prevPixels = nullptr;
    int   m_prevW = 0, m_prevH = 0;

    static ScreenshotModule* s_instance;

    // ── Foreground hook callback ───────────────────────────────────────────────
    static void CALLBACK WinEventProc(
        HWINEVENTHOOK, DWORD, HWND hwnd,
        LONG, LONG, DWORD, DWORD)
    {
        if (s_instance)
        {
            wchar_t title[256] = {};
            ::GetWindowTextW(hwnd, title, 256);
            std::wcout << L"[ScreenshotModule] Foreground → \"" << title << L"\"\n";
            s_instance->CaptureAndSave(L"fg");
        }
    }

    // ── Timestamp ─────────────────────────────────────────────────────────────
    static std::wstring Timestamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::wostringstream ss;
        ss << std::put_time(&tm, L"%Y%m%d_%H%M%S");
        return ss.str();
    }

    // ── Master pipeline ───────────────────────────────────────────────────────
    void CaptureAndSave(const std::wstring& reason)
    {
        if (!m_wicFactory.p) return;

        // ── STEP 1: GDI full-screen capture → raw pixel buffer ────────────────
        const int W = ::GetSystemMetrics(SM_CXSCREEN);
        const int H = ::GetSystemMetrics(SM_CYSCREEN);

        HDC hdcScreen = ::GetDC(nullptr);
        HDC hdcMem = ::CreateCompatibleDC(hdcScreen);
        HBITMAP hBmp = ::CreateCompatibleBitmap(hdcScreen, W, H);
        HBITMAP hOld = static_cast<HBITMAP>(::SelectObject(hdcMem, hBmp));
        ::BitBlt(hdcMem, 0, 0, W, H, hdcScreen, 0, 0, SRCCOPY);

        // GetDIBits reads pixels in BGR order, bottom-up (standard DIB format).
        // stride is padded to 4-byte alignment per BMP spec.
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = W;
        bi.biHeight = H;       // positive = bottom-up
        bi.biPlanes = 1;
        bi.biBitCount = 24;      // BGR, no alpha
        bi.biCompression = BI_RGB;

        const DWORD stride = ((W * 3 + 3) & ~3);
        const DWORD totalBytes = stride * H;

        std::vector<BYTE> cur(totalBytes);
        ::GetDIBits(hdcMem, hBmp, 0, H, cur.data(),
            reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

        ::SelectObject(hdcMem, hOld);
        ::DeleteObject(hBmp);
        ::DeleteDC(hdcMem);
        ::ReleaseDC(nullptr, hdcScreen);

        // ── STEP 2: Delta detection → dirty bounding rect ─────────────────────
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

        // Store current frame as next "previous"
        if (!m_prevPixels || m_prevW != W || m_prevH != H)
        {
            delete[] m_prevPixels;
            m_prevPixels = new BYTE[totalBytes];
            m_prevW = W; m_prevH = H;
        }
        std::memcpy(m_prevPixels, cur.data(), totalBytes);

        if (!anyChange)
        {
            std::wcout << L"[ScreenshotModule] No change, skipping\n";
            return;
        }

        const int cropW = dirty.right - dirty.left;
        const int cropH = dirty.bottom - dirty.top;
        std::wcout << L"[ScreenshotModule] Dirty " << cropW << L"x" << cropH
            << L" [" << reason << L"]\n";

        // ── STEP 3: Build a top-down RGB buffer of the dirty crop ─────────────
        // WIC expects:
        //   • Top-down row order  (DIB is bottom-up → we flip Y)
        //   • RGB byte order      (DIB is BGR      → we swap R and B)
        //
        // We produce a tightly-packed buffer (no row padding) because WIC's
        // CreateBitmapFromMemory accepts an explicit stride parameter.

        const UINT wicStride = cropW * 3; // 3 bytes/pixel, no padding needed
        std::vector<BYTE> rgb(wicStride * cropH);

        for (int y = 0; y < cropH; ++y)
        {
            // Flip Y: DIB row 0 = screen bottom, so screen row (dirty.top + y)
            // is stored at DIB row (H - 1 - (dirty.top + y))
            int dibY = (H - 1) - (dirty.top + y);
            const BYTE* src = cur.data() + dibY * stride + dirty.left * 3;
            BYTE* dst = rgb.data() + y * wicStride;

            for (int x = 0; x < cropW; ++x)
            {
                // Swap BGR → RGB
                dst[x * 3 + 0] = src[x * 3 + 2]; // R ← B slot
                dst[x * 3 + 1] = src[x * 3 + 1]; // G ← G slot
                dst[x * 3 + 2] = src[x * 3 + 0]; // B ← R slot
            }
        }

        // ── STEP 4: Wrap pixels in a WIC bitmap ───────────────────────────────
        // IWICBitmap is WIC's in-memory bitmap type.
        // GUID_WICPixelFormat24bppRGB matches our 3-bytes-per-pixel RGB buffer.
        ComPtr<IWICBitmap> wicBmp;
        HRESULT hr = m_wicFactory->CreateBitmapFromMemory(
            cropW, cropH,
            GUID_WICPixelFormat24bppRGB,
            wicStride,              // bytes per row
            wicStride * cropH,      // total buffer size
            rgb.data(),
            &wicBmp);

        if (FAILED(hr))
        {
            std::cerr << "[ScreenshotModule] CreateBitmapFromMemory failed, hr="
                << std::hex << hr << "\n";
            return;
        }

        // ── STEP 5: 8-bit palette quantization via WIC ────────────────────────
        //
        // WIC PALETTE QUANTIZATION WALKTHROUGH
        // ──────────────────────────────────────
        // A) CreatePalette() — allocates an empty IWICPalette object.
        //
        // B) InitializeFromBitmap() — WIC scans the source bitmap's pixels and
        //    builds an optimal N-color palette.  WICBitmapPaletteTypeFixedHalftone256
        //    gives us 256 colors chosen to best represent this specific image
        //    (as opposed to a fixed web-safe palette).
        //    The last parameter (fAddTransparentColor=FALSE) skips alpha.
        //
        // C) CreateBitmapFromSourceRect() — not needed here; we already have a crop.
        //
        // D) The palette is passed to the frame encoder (step 6) which uses it
        //    when converting 24bpp → 8bpp during the write.

        ComPtr<IWICPalette> palette;
        hr = m_wicFactory->CreatePalette(&palette);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] CreatePalette failed\n"; return; }

        // InitializeFromBitmap scans the pixels and fills the palette with the
        // best 256 colors for this specific image (octree quantization internally).
        hr = palette->InitializeFromBitmap(
            wicBmp,   // source to sample colors from
            256,      // max colors
            FALSE);   // no transparent color slot
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] Palette init failed\n"; return; }

        // ── STEP 6: Create the PNG encoder and write the file ─────────────────
        //
        // WIC ENCODER PIPELINE
        // ──────────────────────
        // IWICBitmapEncoder  — represents the file format (PNG in our case).
        //   └─ IWICBitmapFrameEncode — represents one image frame inside the file.
        //        PNG is single-frame; formats like TIFF/GIF can have multiple.
        //
        // The encoder writes directly to an IStream.
        // SHCreateStreamOnFileEx opens a file-backed IStream — WIC handles
        // all the buffering and flushing internally.

        ::CreateDirectoryW(LR"(C:\tmp\screenshots)", nullptr);
        std::wstring path = LR"(C:\tmp\screenshots\shot_)"
            + Timestamp() + L"_" + reason + L".png";

        // Open a file stream for writing
        ComPtr<IStream> stream;
        hr = ::SHCreateStreamOnFileEx(
            path.c_str(),
            STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
            FILE_ATTRIBUTE_NORMAL,
            TRUE,       // create if not exists
            nullptr,
            &stream);
        if (FAILED(hr))
        {
            std::cerr << "[ScreenshotModule] SHCreateStreamOnFileEx failed, hr="
                << std::hex << hr << "\n";
            return;
        }

        // Create a PNG encoder bound to the stream
        // GUID_ContainerFormatPng identifies PNG; swap for GUID_ContainerFormatJpeg
        // or GUID_ContainerFormatBmp to switch formats with no other code changes.
        ComPtr<IWICBitmapEncoder> encoder;
        hr = m_wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] CreateEncoder failed\n"; return; }

        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] Encoder init failed\n"; return; }

        // Create a frame (PNG has exactly one)
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2>         props;   // encoder properties (compression level etc.)
        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] CreateNewFrame failed\n"; return; }

        // ── Optional: set PNG compression level via property bag ──────────────
        // The PNG encoder exposes "InterlaceOption" and "FilterOption".
        // Compression level is controlled via the underlying zlib and is not
        // directly exposed; WIC uses a balanced default (~level 6).
        // To get maximum compression you'd use a custom IWICBitmapEncoder
        // backed by libpng — WIC's default is already better than GDI+'s.

        hr = frame->Initialize(props);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] Frame init failed\n"; return; }

        // Tell the frame our pixel dimensions
        hr = frame->SetSize(cropW, cropH);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] SetSize failed\n"; return; }

        // Set the output pixel format.
        // WICPixelFormat8bppIndexed → 8-bit palettised (one byte per pixel).
        // The frame will dither the 24bpp source down to 8bpp using the palette
        // we provide below.
        WICPixelFormatGUID fmt = GUID_WICPixelFormat8bppIndexed;
        hr = frame->SetPixelFormat(&fmt);
        // fmt is updated to what the encoder actually accepted — verify:
        if (fmt != GUID_WICPixelFormat8bppIndexed)
        {
            // Encoder didn't accept 8bpp — fall back to 24bpp (still PNG, just larger)
            std::wcout << L"[ScreenshotModule] 8bpp not supported by encoder, using 24bpp\n";
        }

        // Hand the palette to the frame encoder.
        // The encoder will use this palette when converting 24bpp pixels → 8bpp indices.
        hr = frame->SetPalette(palette);
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] SetPalette failed\n"; return; }

        // Write pixels — WIC reads from the IWICBitmap and converts on the fly.
        // WriteSource handles the format conversion (24bpp RGB → 8bpp indexed)
        // and dithering internally using Floyd-Steinberg error diffusion.
        hr = frame->WriteSource(wicBmp, nullptr); // nullptr = entire bitmap
        if (FAILED(hr))
        {
            std::cerr << "[ScreenshotModule] WriteSource failed, hr="
                << std::hex << hr << "\n";
            return;
        }

        // Commit the frame, then the encoder (flushes to the IStream)
        hr = frame->Commit();
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] Frame Commit failed\n"; return; }

        hr = encoder->Commit();
        if (FAILED(hr)) { std::cerr << "[ScreenshotModule] Encoder Commit failed\n"; return; }

        std::wcout << L"[ScreenshotModule] Saved " << path << L"\n";
    }
};

inline ScreenshotModule* ScreenshotModule::s_instance = nullptr;