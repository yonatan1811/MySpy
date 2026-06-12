#pragma once


#include <fstream>
#include <iostream>

#include "SandboxLoader/Modules/IModule.h"


// ─────────────────────────────────────────────────────────────────────────────
// ClipboardModule
//
// Listens for WM_CLIPBOARDUPDATE messages (registered via
// AddClipboardFormatListener) and appends every clipboard text change to
// C:\tmp\clip.log.
//
// This is the original clipboard logic, unchanged, just wrapped in a module.
// ─────────────────────────────────────────────────────────────────────────────

class ClipboardModule : public IModule
{
public:
    std::wstring Name() const override { return L"ClipboardModule"; }

    // Subscribe to clipboard notifications on behalf of our hidden window.
    void OnCreate(HWND hwnd) override
    {
        m_registered = ::AddClipboardFormatListener(hwnd);
        if (!m_registered)
            std::cerr << "[ClipboardModule] AddClipboardFormatListener failed\n";
        else
            std::wcout << L"[ClipboardModule] Listening for clipboard changes\n";
    }

    bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) override
    {
        (void)wParam; (void)lParam;

        if (uMsg == WM_CLIPBOARDUPDATE)
        {
            if (!m_busy)
            {
                m_busy = true;
                CaptureClipboard(hwnd);
                m_busy = false;
            }
            return true; // message handled
        }

        return false;
    }

    void OnDestroy(HWND hwnd) override
    {
        if (m_registered)
        {
            ::RemoveClipboardFormatListener(hwnd);
            m_registered = false;
        }
    }

private:
    bool m_registered = false;
    bool m_busy       = false;  // re-entrancy guard (matches original g_UpdatingClipboard)

    void CaptureClipboard(HWND /*hwnd*/)
    {
        if (!::OpenClipboard(nullptr))
        {
            std::cerr << "[ClipboardModule] OpenClipboard failed\n";
            return;
        }

        HANDLE hData = ::GetClipboardData(CF_UNICODETEXT);
        if (hData)
        {
            wchar_t* text = static_cast<wchar_t*>(::GlobalLock(hData));
            if (text)
            {
                std::wofstream file{ LR"(C:\tmp\clip.log)", std::ios_base::app };
                file << text << L"\n";
                ::GlobalUnlock(hData);
                std::wcout << L"[ClipboardModule] Captured clipboard text\n";
            }
        }

        ::CloseClipboard();
    }
};
