// Encore trigger interface: a loopback-only HTTP listener that lets other applications start a
// song rescan. The listener thread never touches IL2CPP; it forwards each request to the Unity
// window with SendMessageTimeout, so the window procedure handles it on the main thread.
#pragma once
#include <windows.h>

void Log(const char* fmt, ...);

// wParam of the trigger message sent to the Unity window.
enum TriggerCommand : WPARAM
{
    kCmdRescan = 1,
    kCmdStatus = 2,
};

// Result of kCmdRescan, returned by the window procedure.
enum RescanResult : LRESULT
{
    kRescanStarted = 1,
    kRescanNotReady,      // Encore not initialised, or detection failed
    kRescanNotInSongList, // the song list is not open
    kRescanBusy,          // a scan is already running
    kRescanFailed,        // game objects missing (no SongScan instance, null container)
};

// Result of kCmdStatus: kStatusValid plus flag bits.
enum StatusFlags : LRESULT
{
    kStatusValid = 0x100,
    kStatusReady = 0x1,
    kStatusScanning = 0x2,
    kStatusSongList = 0x4,
};

// Starts the listener thread on 127.0.0.1:port.
void StartTriggerServer(HWND unityWnd, UINT triggerMsg, unsigned short port);
