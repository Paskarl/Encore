// Loopback HTTP listener for external requests (see trigger_server.h).
//   POST /rescan  -> starts a full song scan if the song list is open
//   GET  /status  -> {"ready":..,"scanning":..,"songList":..}
//   POST /queue   -> adds a song to the setlist (checksum, or title + optional artist)
//   GET  /setlist -> the current setlist
//   GET  /library -> library songs whose title or artist contain the search text (q, limit)
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

static bool AskMainThread(WPARAM cmd, LRESULT& result, LPARAM arg = 0)
{
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(s_wnd, s_msg, cmd, arg, SMTO_NORMAL | SMTO_ABORTIFHUNG, kMainThreadTimeoutMs, &r))
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

static const char* Reason(int code)
{
    switch (code)
    {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "Service Unavailable";
    }
}

static void Respond(SOCKET s, int code, const std::string& json, const char* extraHeader = "")
{
    char head[256];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\n%s"
             "Connection: close\r\n\r\n",
             code, Reason(code), json.size() + 1, extraHeader);
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
        Respond(s, 503, "{\"result\":\"timeout\",\"message\":\"game did not respond in time (loading?), try again\"}");
        return;
    }
    switch (r)
    {
    case kRescanStarted:
        Respond(s, 202, "{\"result\":\"started\",\"message\":\"song scan started\"}");
        break;
    case kRescanBusy:
        Respond(s, 409, "{\"result\":\"scanning\",\"message\":\"a scan is already running\"}");
        break;
    case kRescanNotInSongList:
        Respond(s, 409, "{\"result\":\"not_in_song_list\",\"message\":\"open the song list and try again\"}");
        break;
    case kRescanFailed:
        Respond(s, 500, "{\"result\":\"failed\",\"message\":\"scan objects not found, see pick.log\"}");
        break;
    default: // kRescanNotReady, or 0 if the message did not reach Pick's window procedure
        Respond(s, 503, "{\"result\":\"not_ready\",\"message\":\"Pick is not ready, see pick.log\"}");
        break;
    }
}

static void HandleStatus(SOCKET s)
{
    LRESULT r = 0;
    if (!AskMainThread(kCmdStatus, r) || !(r & kStatusValid))
    {
        Respond(s, 503, "{\"ready\":false,\"message\":\"game did not respond in time (loading?), try again\"}");
        return;
    }
    char body[128];
    snprintf(body, sizeof(body), "{\"ready\":%s,\"scanning\":%s,\"songList\":%s}", Bool(r & kStatusReady),
             Bool(r & kStatusScanning), Bool(r & kStatusSongList));
    Respond(s, 200, body);
}

// Sends a JsonRequest to the main thread and answers with whatever it filled in.
static void HandleJsonCommand(SOCKET s, WPARAM cmd, JsonRequest* req, const char* what)
{
    LRESULT r = 0;
    bool answered = AskMainThread(cmd, r, reinterpret_cast<LPARAM>(req));
    if (!answered)
    {
        Log("http: %s -> main thread did not answer", what);
        Respond(s, 503, "{\"result\":\"timeout\",\"message\":\"game did not respond in time (loading?); the "
                        "request may still be applied, check GET /setlist\"}");
    }
    else if (r == 0)
    {
        // The window procedure did not take the request (returns 1 when it did), so it never
        // releases its reference.
        ReleaseRequest(req);
        Respond(s, 503, "{\"result\":\"not_ready\",\"message\":\"Pick is not ready, see pick.log\"}");
    }
    else
        Respond(s, req->httpStatus, req->body);
    ReleaseRequest(req);
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static std::string UrlDecode(const std::string& in)
{
    std::string out;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] == '+')
            out += ' ';
        else if (in[i] == '%' && i + 2 < in.size() && HexDigit(in[i + 1]) >= 0 && HexDigit(in[i + 2]) >= 0)
        {
            out += static_cast<char>(HexDigit(in[i + 1]) * 16 + HexDigit(in[i + 2]));
            i += 2;
        }
        else
            out += in[i];
    }
    return out;
}

// Value of key in a query string or form body ("a=1&b=2"); false if absent.
static bool FormValue(const std::string& form, const char* key, std::string& out)
{
    size_t start = 0, klen = strlen(key);
    while (start < form.size())
    {
        size_t end = form.find('&', start);
        if (end == std::string::npos)
            end = form.size();
        if (end - start > klen && form.compare(start, klen, key) == 0 && form[start + klen] == '=')
        {
            out = UrlDecode(form.substr(start + klen + 1, end - start - klen - 1));
            return true;
        }
        start = end + 1;
    }
    return false;
}

static void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Minimal lookup of a top-level "key": "string" or "key": number in a flat JSON object. Enough
// for {"checksum":"..."} style bodies; nested objects are not supported.
static bool JsonValue(const std::string& json, const char* key, std::string& out)
{
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos)
    {
        size_t p = pos + needle.size();
        while (p < json.size() && isspace(static_cast<unsigned char>(json[p])))
            ++p;
        if (p >= json.size() || json[p] != ':')
        {
            pos = p;
            continue;
        }
        ++p;
        while (p < json.size() && isspace(static_cast<unsigned char>(json[p])))
            ++p;
        if (p < json.size() && json[p] == '"')
        {
            out.clear();
            for (++p; p < json.size() && json[p] != '"'; ++p)
            {
                if (json[p] != '\\' || p + 1 >= json.size())
                {
                    out += json[p];
                    continue;
                }
                char e = json[++p];
                if (e == 'n')
                    out += '\n';
                else if (e == 't')
                    out += '\t';
                else if (e == 'u' && p + 4 < json.size())
                {
                    unsigned cp = strtoul(json.substr(p + 1, 4).c_str(), nullptr, 16);
                    p += 4;
                    // Surrogate pair
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 < json.size() && json[p + 1] == '\\' &&
                        json[p + 2] == 'u')
                    {
                        unsigned lo = strtoul(json.substr(p + 3, 4).c_str(), nullptr, 16);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                    AppendUtf8(out, cp);
                }
                else
                    out += e; // \" \\ \/ and anything else literally
            }
            return true;
        }
        size_t end = p;
        while (end < json.size() && (isalnum(static_cast<unsigned char>(json[end])) || json[end] == '-' || json[end] == '.'))
            ++end;
        out = json.substr(p, end - p);
        return !out.empty();
    }
    return false;
}

// Reads a parameter from the query string, a JSON body or a form body, in that order.
static bool Param(const std::string& query, const std::string& body, const char* key, std::string& out)
{
    if (FormValue(query, key, out))
        return true;
    size_t first = body.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && body[first] == '{')
        return JsonValue(body, key, out);
    return FormValue(body, key, out);
}

static void HandleQueue(SOCKET s, const std::string& query, const std::string& body)
{
    auto* req = new JsonRequest();
    Param(query, body, "checksum", req->checksum);
    Param(query, body, "title", req->title);
    Param(query, body, "artist", req->artist);
    std::string speed;
    if (Param(query, body, "speed", speed))
        req->speed = atoi(speed.c_str());
    if (req->checksum.empty() && req->title.empty())
    {
        delete req;
        Respond(s, 400, "{\"result\":\"bad_request\",\"message\":\"give a checksum, or a title (and optionally an "
                        "artist), as query parameters or a JSON body\"}");
        return;
    }
    if (speed.size() && (req->speed < 5 || req->speed > 1000))
    {
        delete req;
        Respond(s, 400, "{\"result\":\"bad_request\",\"message\":\"speed must be a percentage, e.g. 100\"}");
        return;
    }
    HandleJsonCommand(s, kCmdQueue, req, "POST /queue");
}

static void HandleLibrary(SOCKET s, const std::string& query)
{
    auto* req = new JsonRequest();
    FormValue(query, "q", req->query);
    std::string limit;
    if (FormValue(query, "limit", limit))
        req->limit = atoi(limit.c_str());
    if (req->limit < 1 || req->limit > kLibraryMaxLimit)
    {
        delete req;
        char body[128];
        snprintf(body, sizeof(body), "{\"result\":\"bad_request\",\"message\":\"limit must be 1 to %d\"}",
                 kLibraryMaxLimit);
        Respond(s, 400, body);
        return;
    }
    if (req->query.size() > 200)
    {
        delete req;
        Respond(s, 400, "{\"result\":\"bad_request\",\"message\":\"search text too long\"}");
        return;
    }
    HandleJsonCommand(s, kCmdLibrary, req, "GET /library");
}

// The value of a header (name in lower case, with the colon), trimmed; false if it is missing.
static bool HeaderValue(const std::string& head, const char* name, std::string& value)
{
    // Header names are case-insensitive.
    std::string lower = head;
    for (char& c : lower)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    size_t pos = lower.find(std::string("\r\n") + name);
    if (pos == std::string::npos)
        return false;
    pos += 2 + strlen(name);
    size_t end = lower.find("\r\n", pos);
    value = lower.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    size_t first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
    value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
    return true;
}

static size_t ContentLength(const std::string& head)
{
    std::string value;
    return HeaderValue(head, "content-length:", value) ? strtoul(value.c_str(), nullptr, 10) : 0;
}

// Browsers send Origin on cross-site requests, and a Host header with the page's own name; local
// programs (the companion, the tools, curl) send neither, or a loopback Host. Refusing both stops a
// web page from triggering requests here (CSRF) or reading answers through DNS rebinding.
static bool FromLocalProgram(const std::string& head)
{
    std::string value;
    if (HeaderValue(head, "origin:", value))
        return false;
    if (!HeaderValue(head, "host:", value))
        return true;
    size_t colon = value.rfind(':');
    if (colon != std::string::npos && value.find(']', colon) == std::string::npos)
        value.resize(colon);
    return value == "127.0.0.1" || value == "localhost" || value == "[::1]";
}

static void MethodNotAllowed(SOCKET s, const char* allow, const char* usage)
{
    char header[64], body[128];
    snprintf(header, sizeof(header), "Allow: %s\r\n", allow);
    snprintf(body, sizeof(body), "{\"result\":\"error\",\"message\":\"use %s\"}", usage);
    Respond(s, 405, body, header);
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
    // Read the whole body: /queue takes its parameters from it, and closing the socket with unread
    // data would reset the connection before the client has read the response.
    size_t bodyLen = ContentLength(req.substr(0, headerEnd + 2));
    if (!FromLocalProgram(req.substr(0, headerEnd + 2)))
    {
        Log("http: refused a request from a web page (Origin header or non-loopback Host)");
        Respond(s, 403, "{\"result\":\"forbidden\",\"message\":\"only programs on this PC may use this interface\"}");
        return;
    }
    if (bodyLen > kMaxBody)
    {
        Respond(s, 413, "{\"result\":\"error\",\"message\":\"body too large\"}");
        return;
    }
    std::string body = req.substr(headerEnd + 4);
    while (body.size() < bodyLen)
    {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        body.append(buf, n);
    }
    if (body.size() > bodyLen)
        body.resize(bodyLen);

    char method[16] = {}, target[2048] = {};
    if (sscanf(req.c_str(), "%15s %2047s", method, target) != 2)
    {
        Respond(s, 400, "{\"result\":\"error\",\"message\":\"bad request\"}");
        return;
    }
    std::string query;
    if (char* q = strchr(target, '?'))
    {
        query = q + 1;
        *q = 0;
    }
    bool isGet = strcmp(method, "GET") == 0, isPost = strcmp(method, "POST") == 0;
    if (strcmp(target, "/rescan") == 0)
    {
        if (isPost)
            HandleRescan(s);
        else
            MethodNotAllowed(s, "POST", "POST /rescan");
    }
    else if (strcmp(target, "/status") == 0)
    {
        if (isGet)
            HandleStatus(s);
        else
            MethodNotAllowed(s, "GET", "GET /status");
    }
    else if (strcmp(target, "/queue") == 0)
    {
        if (isPost)
            HandleQueue(s, query, body);
        else
            MethodNotAllowed(s, "POST", "POST /queue");
    }
    else if (strcmp(target, "/setlist") == 0)
    {
        if (isGet)
            HandleJsonCommand(s, kCmdSetlist, new JsonRequest(), "GET /setlist");
        else
            MethodNotAllowed(s, "GET", "GET /setlist");
    }
    else if (strcmp(target, "/library") == 0)
    {
        if (isGet)
            HandleLibrary(s, query);
        else
            MethodNotAllowed(s, "GET", "GET /library");
    }
    else
    {
        Respond(s, 404, "{\"result\":\"error\",\"message\":\"endpoints: POST /rescan, GET /status, POST /queue, "
                        "GET /setlist, GET /library\"}");
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
    Log("http: listening on http://127.0.0.1:%u (POST /rescan, GET /status, POST /queue, GET /setlist, GET /library)", s_port);
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
