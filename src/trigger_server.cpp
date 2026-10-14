// Loopback HTTP listener for external rescan requests (see trigger_server.h).
//   POST /rescan  -> starts a full song scan if the song list is open
//   GET  /status  -> {"ready":..,"scanning":..,"songList":..}
// One connection at a time, Connection: close. The listener only does socket work; every
// question about game state goes to the window procedure on Unity's main thread.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "trigger_server.h"

static HWND s_wnd = nullptr;
static UINT s_msg = 0;
static unsigned short s_port = 0;

// How long a request waits for the main thread (it pumps messages every frame, but stalls while
// a scene loads).
static const UINT kMainThreadTimeoutMs = 3000;
static const size_t kMaxHeader = 8192;
static const size_t kMaxBody = 65536;

static bool AskMainThread(WPARAM cmd, LRESULT& result)
{
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(s_wnd, s_msg, cmd, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, kMainThreadTimeoutMs, &r))
        return false;
    result = static_cast<LRESULT>(r);
    return true;
}

static void SendAll(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0)
            return;
        sent += n;
    }
}

static void Respond(SOCKET s, int code, const char* reason, const std::string& json, const char* extraHeader = "")
{
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n",
             code, reason, json.size(), extraHeader);
    SendAll(s, head + json + "\n");
}

static const char* Bool(bool b)
{
    return b ? "true" : "false";
}

static void HandleRescan(SOCKET s)
{
    LRESULT r = 0;
    if (!AskMainThread(kCmdRescan, r))
    {
        Log("http: POST /rescan -> main thread did not answer");
        Respond(s, 503, "Service Unavailable",
                "{\"result\":\"timeout\",\"message\":\"game did not respond in time (loading?), try again\"}");
        return;
    }
    switch (r)
    {
    case kRescanStarted:
        Respond(s, 202, "Accepted", "{\"result\":\"started\",\"message\":\"song scan started\"}");
        break;
    case kRescanBusy:
        Respond(s, 409, "Conflict", "{\"result\":\"scanning\",\"message\":\"a scan is already running\"}");
        break;
    case kRescanNotInSongList:
        Respond(s, 409, "Conflict",
                "{\"result\":\"not_in_song_list\",\"message\":\"open the song list and try again\"}");
        break;
    case kRescanFailed:
        Respond(s, 500, "Internal Server Error",
                "{\"result\":\"failed\",\"message\":\"scan objects not found, see encore.log\"}");
        break;
    default: // kRescanNotReady, or 0 if the message did not reach Encore's window procedure
        Respond(s, 503, "Service Unavailable",
                "{\"result\":\"not_ready\",\"message\":\"Encore is not ready, see encore.log\"}");
        break;
    }
}

static void HandleStatus(SOCKET s)
{
    LRESULT r = 0;
    if (!AskMainThread(kCmdStatus, r) || !(r & kStatusValid))
    {
        Respond(s, 503, "Service Unavailable",
                "{\"ready\":false,\"message\":\"game did not respond in time (loading?), try again\"}");
        return;
    }
    char body[128];
    snprintf(body, sizeof(body), "{\"ready\":%s,\"scanning\":%s,\"songList\":%s}", Bool(r & kStatusReady),
             Bool(r & kStatusScanning), Bool(r & kStatusSongList));
    Respond(s, 200, "OK", body);
}

static size_t ContentLength(const std::string& head)
{
    // Header names are case-insensitive.
    std::string lower = head;
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t pos = lower.find("\r\ncontent-length:");
    if (pos == std::string::npos)
        return 0;
    return strtoul(lower.c_str() + pos + 17, nullptr, 10);
}

static void HandleConnection(SOCKET s)
{
    DWORD timeout = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string req;
    size_t headerEnd;
    char buf[2048];
    while ((headerEnd = req.find("\r\n\r\n")) == std::string::npos)
    {
        if (req.size() > kMaxHeader)
            return;
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            return;
        req.append(buf, n);
    }
    // Read (and ignore) the body so closing the socket doesn't reset the connection before the
    // client has read the response.
    size_t bodyLen = ContentLength(req.substr(0, headerEnd + 2));
    if (bodyLen > kMaxBody)
    {
        Respond(s, 413, "Payload Too Large", "{\"result\":\"error\",\"message\":\"body too large\"}");
        return;
    }
    size_t have = req.size() - (headerEnd + 4);
    while (have < bodyLen)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        have += n;
    }

    char method[16] = {}, target[256] = {};
    if (sscanf(req.c_str(), "%15s %255s", method, target) != 2)
    {
        Respond(s, 400, "Bad Request", "{\"result\":\"error\",\"message\":\"bad request\"}");
        return;
    }
    if (char* q = strchr(target, '?'))
        *q = 0;
    bool isGet = strcmp(method, "GET") == 0, isPost = strcmp(method, "POST") == 0;
    if (strcmp(target, "/rescan") == 0)
    {
        if (isPost)
            HandleRescan(s);
        else
            Respond(s, 405, "Method Not Allowed", "{\"result\":\"error\",\"message\":\"use POST /rescan\"}",
                    "Allow: POST\r\n");
    }
    else if (strcmp(target, "/status") == 0)
    {
        if (isGet)
            HandleStatus(s);
        else
            Respond(s, 405, "Method Not Allowed", "{\"result\":\"error\",\"message\":\"use GET /status\"}",
                    "Allow: GET\r\n");
    }
    else
    {
        Respond(s, 404, "Not Found",
                "{\"result\":\"error\",\"message\":\"endpoints: POST /rescan, GET /status\"}");
    }
}

static DWORD WINAPI ListenerThread(LPVOID)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        Log("http: WSAStartup failed");
        return 0;
    }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET)
    {
        Log("http: socket failed (%d)", WSAGetLastError());
        return 0;
    }
    BOOL exclusive = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(s_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // loopback only, never reachable from the network
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 4) == SOCKET_ERROR)
    {
        Log("http: cannot listen on 127.0.0.1:%u (error %d, port in use?), trigger interface off", s_port,
            WSAGetLastError());
        closesocket(ls);
        return 0;
    }
    Log("http: listening on http://127.0.0.1:%u (POST /rescan, GET /status)", s_port);
    for (;;)
    {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET)
        {
            Sleep(100);
            continue;
        }
        HandleConnection(c);
        shutdown(c, SD_SEND);
        closesocket(c);
    }
}

void StartTriggerServer(HWND unityWnd, UINT triggerMsg, unsigned short port)
{
    s_wnd = unityWnd;
    s_msg = triggerMsg;
    s_port = port;
    CloseHandle(CreateThread(nullptr, 0, ListenerThread, nullptr, 0, nullptr));
}
