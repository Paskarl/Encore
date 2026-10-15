// Pick trigger interface: a loopback-only HTTP listener that lets other applications start a
// song rescan and queue songs. The listener thread never touches IL2CPP; it forwards each request
// to the Unity window with SendMessageTimeout, so the window procedure handles it on the main
// thread.
#pragma once
#include <windows.h>
#include <string>

void Log(const char* fmt, ...);

// wParam of the trigger message sent to the Unity window.
enum TriggerCommand : WPARAM
{
    kCmdRescan = 1,
    kCmdStatus = 2,
    kCmdQueue = 3,   // lParam = JsonRequest*
    kCmdSetlist = 4, // lParam = JsonRequest*
    kCmdLibrary = 5, // lParam = JsonRequest*
};

// Result of kCmdRescan, returned by the window procedure.
enum RescanResult : LRESULT
{
    kRescanStarted = 1,
    kRescanNotReady,      // Pick not initialised, or detection failed
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

// Bounds of GET /library results.
static const int kLibraryDefaultLimit = 50;
static const int kLibraryMaxLimit = 200;

// A request whose answer is a JSON body built on the main thread (kCmdQueue, kCmdSetlist,
// kCmdLibrary).
// Both threads hold a reference: if SendMessageTimeout gives up, the window procedure may still
// handle the message later, so the request must outlive the listener's wait.
struct JsonRequest
{
    LONG refs = 2;
    // Input (kCmdQueue). Empty / -1 when not given.
    std::string checksum, title, artist;
    int speed = -1;
    // Input (kCmdLibrary): search text and maximum number of results.
    std::string query;
    int limit = kLibraryDefaultLimit;
    // Output, filled on the main thread. httpStatus 0 = not handled.
    int httpStatus = 0;
    std::string body;
};

inline void ReleaseRequest(JsonRequest* r)
{
    if (InterlockedDecrement(&r->refs) == 0)
        delete r;
}

// Starts the listener thread on 127.0.0.1:port.
void StartTriggerServer(HWND unityWnd, UINT triggerMsg, unsigned short port);
