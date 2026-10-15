// Starts the companion (companion/, a separate .NET program) if it isn't running and asks it for
// a short pairing code over loopback. Runs on a worker thread that only does process and socket
// work; the result is posted to the Unity window, so the dialog is shown on the main thread.
#pragma once
#include <windows.h>
#include <string>

static const unsigned short kCompanionPort = 47322;

// lParam of the result message; the receiver deletes it.
struct CompanionResult
{
    bool ok = false;
    bool started = false; // Encore started the companion (it wasn't running)
    std::string url;      // companion address on the LAN, e.g. http://192.168.1.20:47322/
    std::string code;     // 6-digit pairing code
    int expiresMinutes = 0;
    std::string error;    // when !ok: what went wrong, for the player
};

// Returns false (and does nothing) while a previous request is still running.
bool RequestCompanionPairing(HWND wnd, UINT resultMsg, unsigned short encorePort);
