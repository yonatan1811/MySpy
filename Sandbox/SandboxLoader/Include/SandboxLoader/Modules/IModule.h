#pragma once
#include <Windows.h>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// IModule – every monitoring module implements this interface.
//
// HOW TO ADD A NEW MODULE
// ───────────────────────
// 1. Create  modules/YourModule.h  (and optionally  modules/YourModule.cpp)
// 2. Inherit from IModule and override the methods you care about.
// 3. In main.cpp, #include your header and call:
//        registry.Register(std::make_unique<YourModule>());
//    That's it. The message loop and window management stay untouched.
//
// LIFECYCLE
// ─────────
//   OnCreate()   – called when the hidden message window is created.
//                  Subscribe to clipboard notifications, install hooks, etc.
//   OnMessage()  – called for every Windows message.
//                  Return true to mark the message as handled (stops further
//                  dispatch to other modules); false to keep forwarding.
//   OnDestroy()  – called just before the window is destroyed.
//                  Uninstall hooks, release resources.
//   Name()       – human-readable label used in logs / diagnostics.
// ─────────────────────────────────────────────────────────────────────────────

class IModule
{
public:
    virtual ~IModule() = default;

    // Called once after the window is successfully created.
    virtual void OnCreate(HWND hwnd) { (void)hwnd; }

    // Called for every message dispatched to the window procedure.
    // Return true if the message was fully handled by this module.
    virtual bool OnMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        (void)hwnd; (void)uMsg; (void)wParam; (void)lParam;
        return false;
    }

    // Called once before the window is destroyed.
    virtual void OnDestroy(HWND hwnd) { (void)hwnd; }

    // Diagnostic label.
    virtual std::wstring Name() const = 0;
};
