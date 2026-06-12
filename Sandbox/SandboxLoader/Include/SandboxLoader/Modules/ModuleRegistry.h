#pragma once
#include "IModule.h"
#include <vector>
#include <memory>
#include <iostream>

// ─────────────────────────────────────────────────────────────────────────────
// ModuleRegistry
//
// Owns all IModule instances and fans out window messages to each one in
// registration order.  The WndProc delegates entirely to this class.
// ─────────────────────────────────────────────────────────────────────────────

class ModuleRegistry
{
public:
    // Transfer ownership of a module into the registry.
    void Register(std::unique_ptr<IModule> module)
    {
        std::wcout << L"[Registry] Registered module: " << module->Name() << L"\n";
        m_modules.push_back(std::move(module));
    }

    // Forward WM_CREATE to every module so they can set up subscriptions.
    void OnCreate(HWND hwnd)
    {
        for (auto& mod : m_modules)
        {
            try { mod->OnCreate(hwnd); }
            catch (const std::exception& ex)
            {
                std::wcerr << "[Registry] " << mod->Name().c_str()
                          << "::OnCreate threw: " << ex.what() << "\n";
            }
        }
    }

    // Fan out a message.  If a module returns true the message is consumed
    // and remaining modules are skipped for this message.
    bool Dispatch(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
    {
        for (auto& mod : m_modules)
        {
            try
            {
                if (mod->OnMessage(hwnd, uMsg, wParam, lParam))
                    return true;
            }
            catch (const std::exception& ex)
            {
                std::wcerr << "[Registry] " << mod->Name().c_str()
                          << "::OnMessage threw: " << ex.what() << "\n";
            }
        }
        return false;
    }

    // Forward WM_DESTROY to every module so they can clean up.
    void OnDestroy(HWND hwnd)
    {
        for (auto& mod : m_modules)
        {
            try { mod->OnDestroy(hwnd); }
            catch (const std::exception& ex)
            {
                std::wcerr << "[Registry] " << mod->Name().c_str()
                          << "::OnDestroy threw: " << ex.what() << "\n";
            }
        }
    }

private:
    std::vector<std::unique_ptr<IModule>> m_modules;
};
