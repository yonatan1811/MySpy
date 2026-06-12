#pragma once


#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <iostream>
#include <vector>


#include "SandboxLoader/Modules/IModule.h"



// ─────────────────────────────────────────────────────────────────────────────
// ScreenshotModule
//
// Takes a full-screen BMP screenshot every N seconds using pure WinAPI / GDI.
// Screenshots are saved to C:\tmp\screenshots\ as
//     screenshot_YYYYMMDD_HHMMSS.bmp
//
// HOW IT WORKS (WinAPI / GDI walkthrough)
// ────────────────────────────────────────
// 1. GetDC(nullptr)              – grab a DC for the whole screen
// 2. CreateCompatibleDC()        – create an in-memory DC
// 3. CreateCompatibleBitmap()    – allocate a bitmap the same size as the screen
// 4. SelectObject()              – attach the bitmap to the memory DC
// 5. BitBlt()                    – copy pixels from screen DC → memory DC
// 6. GetDIBits()                 – read raw pixel data into a DIB buffer
// 7. Write BITMAPFILEHEADER +
//    BITMAPINFOHEADER + pixels   – assemble a valid .bmp file manually
// 8. Clean up all GDI objects
//
// The module uses WM_TIMER (SetTimer / KillTimer) so it works entirely inside
// the existing message loop without any extra threads.
// ─────────────────────────────────────────────────────────────────────────────


class ScreenshotModule : public IModule
{
public:
    // intervalSeconds – how often to take a screenshot (default: 30 s)
    explicit ScreenshotModule(UINT intervalSeconds = 30)
        : m_intervalMs(intervalSeconds * 1000)
    {}

    std::wstring Name() const override { return L"ScreenshotModule"; }

    void OnCreate(HWND hwnd) override
    {
        // Timer ID 1 is owned by this module.
        ::SetTimer(hwnd, TIMER_ID, m_intervalMs, nullptr);
        std::wcout << L"[ScreenshotModule] Timer set every "
                   << m_intervalMs / 1000 << L" s\n";

        m_hWinEvent = ::SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,   // last  event in range
            NULL,                      // hModWndProcDll — NULL = in-process delivery
            WinHookScreenShot,              // our callback
            0,                         // any process
            0,                         // any thread
            WINEVENT_OUTOFCONTEXT      // deliver via message queue, no DLL required
            | WINEVENT_SKIPOWNPROCESS);

        /*SetWindowsHookExA(WH_CBT , HookScreenShot, nullptr , 0);*/

        // Take an immediate screenshot so we don't wait for the first tick.
        TakeScreenshot();
    }

    bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override
    {
        (void)hwnd; (void)lParam;

        if (uMsg == WM_TIMER && wParam == TIMER_ID)
        {
            TakeScreenshot();
            return true;
        }
        return false;
    }

    void OnDestroy(HWND hwnd) override
    {
        ::KillTimer(hwnd, TIMER_ID);
        if (m_hWinEvent)
        {
            ::UnhookWinEvent(m_hWinEvent);
            m_hWinEvent = nullptr;
        }
    }

private:
    static constexpr UINT_PTR TIMER_ID = 1001; // unique ID for this module's timer
    UINT m_intervalMs;
    HWINEVENTHOOK m_hWinEvent = nullptr;

    
    static std::wstring Timestamp()
    {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);

        std::wostringstream ss;
        ss << std::put_time(&tm, L"%Y%m%d_%H%M%S");
        return ss.str();
    }

    // ── Core screenshot logic ─────────────────────────────────────────────────
    static void TakeScreenshot()
    {
        // ── Step 1: get screen dimensions ────────────────────────────────────
        const int screenW = ::GetSystemMetrics(SM_CXSCREEN);
        const int screenH = ::GetSystemMetrics(SM_CYSCREEN);

        // ── Step 2: acquire DCs ───────────────────────────────────────────────
        HDC hdcScreen = ::GetDC(nullptr);           // screen device context
        if (!hdcScreen)
        {
            std::cerr << "[ScreenshotModule] GetDC failed\n";
            return;
        }

        HDC hdcMem = ::CreateCompatibleDC(hdcScreen); // memory (off-screen) DC
        if (!hdcMem)
        {
            ::ReleaseDC(nullptr, hdcScreen);
            std::cerr << "[ScreenshotModule] CreateCompatibleDC failed\n";
            return;
        }

        // ── Step 3: create a bitmap backed by the screen DC ──────────────────
        HBITMAP hBitmap = ::CreateCompatibleBitmap(hdcScreen, screenW, screenH);
        if (!hBitmap)
        {
            ::DeleteDC(hdcMem);
            ::ReleaseDC(nullptr, hdcScreen);
            std::cerr << "[ScreenshotModule] CreateCompatibleBitmap failed\n";
            return;
        }

        // ── Step 4: select bitmap into memory DC ─────────────────────────────
        HBITMAP hOldBitmap = static_cast<HBITMAP>(::SelectObject(hdcMem, hBitmap));

        // ── Step 5: blit screen pixels into memory DC ─────────────────────────
        ::BitBlt(hdcMem, 0, 0, screenW, screenH, hdcScreen, 0, 0, SRCCOPY);

        // ── Step 6: read raw pixel data (DIB = Device-Independent Bitmap) ─────
        BITMAPINFOHEADER biHeader{};
        biHeader.biSize        = sizeof(BITMAPINFOHEADER);
        biHeader.biWidth       = screenW;
        biHeader.biHeight      = screenH; // positive → bottom-up (standard BMP)
        biHeader.biPlanes      = 1;
        biHeader.biBitCount    = 24;      // 24-bit RGB, no alpha, no palette
        biHeader.biCompression = BI_RGB;

        // Calculate row stride: each row is padded to a 4-byte boundary.
        const DWORD rowStride   = ((screenW * 3 + 3) & ~3);
        const DWORD pixelBytes  = rowStride * screenH;

        std::vector<BYTE> pixels(pixelBytes);
        ::GetDIBits(hdcMem, hBitmap, 0, screenH, pixels.data(),
                    reinterpret_cast<BITMAPINFO*>(&biHeader), DIB_RGB_COLORS);

        // ── Step 7: assemble BMP file in memory and write to disk ─────────────
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType      = 0x4D42; // 'BM'
        fileHeader.bfOffBits   = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize      = fileHeader.bfOffBits + pixelBytes;

        std::wstring path = LR"(C:\tmp\screenshots\screenshot_)" + Timestamp() + L".bmp";

        // Ensure output directory exists
        ::CreateDirectoryW(LR"(C:\tmp\screenshots)", nullptr);

        HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            std::cerr << "[ScreenshotModule] CreateFile failed, GLE="
                      << ::GetLastError() << "\n";
        }
        else
        {
            DWORD written{};
            ::WriteFile(hFile, &fileHeader, sizeof(fileHeader), &written, nullptr);
            ::WriteFile(hFile, &biHeader,   sizeof(biHeader),   &written, nullptr);
            ::WriteFile(hFile, pixels.data(), pixelBytes,        &written, nullptr);
            ::CloseHandle(hFile);
            std::wcout << L"[ScreenshotModule] Saved " << path << L"\n";
        }

        // ── Step 8: clean up GDI resources ────────────────────────────────────
        ::SelectObject(hdcMem, hOldBitmap);
        ::DeleteObject(hBitmap);
        ::DeleteDC(hdcMem);
        ::ReleaseDC(nullptr, hdcScreen);
    }

    static void CALLBACK WinHookScreenShot(
        HWINEVENTHOOK /*hWinEventHook*/,
        DWORD         /*event*/,
        HWND          hwnd,
        LONG          /*idObject*/,
        LONG          /*idChild*/,
        DWORD         /*dwEventThread*/,
        DWORD         /*dwmsEventTime*/)
    {
           TakeScreenshot();
    }


    /*static LRESULT CALLBACK HookScreenShot(int ncode, WPARAM wParam, LPARAM lparam)
    {
        if (ncode >= 0)
        {
            switch (ncode)
            {
            case HCBT_ACTIVATE:
                TakeScreenshot();
                break;
            }
        }
        return CallNextHookEx(nullptr, ncode, wParam, lparam);

    }*/
    
};
