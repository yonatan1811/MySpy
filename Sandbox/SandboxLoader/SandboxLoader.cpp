// Source - https://stackoverflow.com/a/59473501
// Posted by Remy Lebeau
// Retrieved 2026-06-05, License - CC BY-SA 4.0
#include <Windows.h>
#include <string_view>
#include <fstream>
#include <iostream>

HINSTANCE g_hThisInst = NULL;
HWND g_hwndCurrent = NULL;
//HWND g_hwndNext = NULL;
bool g_AddedListener = false;   
bool g_UpdatingClipboard = false;

constexpr std::wstring_view CLIP_TEXT = L"AW";


void take_screenshot()
{

}


void write_clipboard()
{
    const auto clipboard_handle = GetClipboardData(CF_UNICODETEXT);
    if (!clipboard_handle)
    {
        throw std::exception("LOL");
    }

    // Lock the handle to get a pointer
    wchar_t* text = static_cast<wchar_t*>(GlobalLock(clipboard_handle));
    if (text == nullptr) {
        throw std::exception("LOL1");
    }

    std::wofstream file{ LR"(C:\tmp\clip.log)" , std::ios_base::app };
    file << text << L"\n";

    GlobalUnlock(clipboard_handle);
}


//void change{
//
//    EmptyClipboard();
//    size_t bytes = (CLIP_TEXT.size() + 1) * sizeof(wchar_t);
//    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
//    if (!hMem) {
//        CloseClipboard();
//        return false;
//    }
//    void* ptr = GlobalLock(hMem);
//    std::memcpy(ptr, CLIP_TEXT.data(), bytes);
//
//    GlobalUnlock(hMem);
//    if (!SetClipboardData(CF_UNICODETEXT, hMem))
//    {
//        GlobalFree(hMem);
//        CloseClipboard();
//        return false;
//    }
//}

uint32_t set_me() {
    if (!OpenClipboard(nullptr)) return false;
    try
    {
        write_clipboard();
    }
    catch (const std::exception&)
    {
        std::cerr << "Error" << "\n";
    } 
    
    CloseClipboard();
    return true;
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        //g_hwndNext = ::SetClipboardViewer(hwnd);
        g_AddedListener = ::AddClipboardFormatListener(hwnd);
        return g_AddedListener ? 0 : -1;

    case WM_DESTROY:
        /*
        ChangeClipboardChain(hwnd, g_hwndNext); 
        g_hwndNext = NULL;
        */
        if (g_AddedListener)
        {
            RemoveClipboardFormatListener(hwnd);
            g_AddedListener = false;
        }
        return 0;


    case WM_CLIPBOARDUPDATE:
        if (g_UpdatingClipboard)
        {
            return 0;
        }
        g_UpdatingClipboard = true;
        set_me();
        g_UpdatingClipboard = false;
        return 0;

    case WM_DESTROYCLIPBOARD:
        // Handle clipboard cleared event and forward message
        break;
    }

    return ::DefWindowProc(hwnd, uMsg, wParam, lParam);
}


HRESULT SetOrRefreshWindowsHook()
{
    try
    {
        if (!g_hwndCurrent)
        {
            WNDCLASS wndClass = {};
            wndClass.lpfnWndProc = &WndProc;
            wndClass.hInstance = g_hThisInst;
            wndClass.lpszClassName = TEXT("Nice");

            if (!::RegisterClass(&wndClass))
            {
                DWORD dwLastError = ::GetLastError();
                if (dwLastError != ERROR_CLASS_ALREADY_EXISTS)
                    return HRESULT_FROM_WIN32(dwLastError);
            }

            g_hwndCurrent = ::CreateWindowEx(0, wndClass.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hThisInst, NULL);
            if (!g_hwndCurrent)
            {
                DWORD dwLastError = ::GetLastError();
                return HRESULT_FROM_WIN32(dwLastError);
            }
        }
    }
    catch (...)
    {
        return E_UNEXPECTED;
    }

    return S_OK;
}



int WINAPI main(
    HINSTANCE hInstance,
    HINSTANCE,
    PWSTR,
    int)
{
    g_hThisInst = hInstance;
    SetOrRefreshWindowsHook();
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        DispatchMessage(&msg);
    }
}

