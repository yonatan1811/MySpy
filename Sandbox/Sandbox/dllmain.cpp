// dllmain.cpp : Defines the entry point for the DLL application.
#include <winsock2.h>
#include <ws2tcpip.h>
// Link against ws2_32.lib and ensure named imports
#pragma comment(lib, "ws2_32.lib")

extern "C" {
    __declspec(dllexport) void install();
}



#include <winsock2.h>
#include <ws2tcpip.h>
// Link against ws2_32.lib and ensure named imports
#pragma comment(lib, "ws2_32.lib")
#include <windows.h>
#include <iostream>
#include <string>






void install()
{
    // Create parser with base address
    // Get handle to the module
    WORD wVersionRequested = MAKEWORD(2, 2);
    WSADATA wsaData;

    int result = WSAStartup(wVersionRequested, &wsaData);
    inet_pton(1, nullptr, nullptr);

}



BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        MessageBoxA(nullptr, "Lol", "What", MB_OK);
        break;

    case DLL_PROCESS_DETACH:
        break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    }
    return TRUE;
}