// Starts the companion and fetches a pairing code for the "Encore" main-menu entry.
//
// Where the companion is: ENCORE_COMPANION (full path to EncoreCompanion.exe), otherwise
// EncoreCompanion\EncoreCompanion.exe next to the game's executable (a `dotnet publish` output
// folder). It is started with its own console window (minimized, so a fullscreen game keeps the
// focus); closing that window stops it. --no-browser keeps it from opening the pairing page over
// the game when it creates a new token.
//
// The code comes from the companion's POST /pair/code, which only answers to loopback with a
// loopback Host header. Encore itself stays loopback-only; the companion is the LAN-facing part.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include "companion_launcher.h"

void Log(const char* fmt, ...);

static volatile LONG s_pending = 0;

struct LaunchJob
{
    HWND wnd;
    UINT msg;
    unsigned short encorePort;
};

static std::wstring CompanionPath()
{
    // Long enough for long-path game folders; a truncated path would point somewhere else.
    std::wstring buf(32768, L'\0');
    DWORD n = GetEnvironmentVariableW(L"ENCORE_COMPANION", &buf[0], static_cast<DWORD>(buf.size()));
    if (n > 0 && n < buf.size())
        return buf.c_str();
    n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size())
        return L"";
    std::wstring path = buf.c_str();
    size_t slash = path.find_last_of(L'\\');
    path = path.substr(0, slash == std::wstring::npos ? 0 : slash + 1);
    return path + L"EncoreCompanion\\EncoreCompanion.exe";
}

static std::string Utf8(const std::wstring& w)
{
    if (w.empty())
        return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

// POST /pair/code to the companion on loopback. Returns the HTTP status (0 = no connection) and
// the response body.
static int PostPairCode(std::string& body)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 0;
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kCompanionPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        closesocket(s);
        return 0;
    }
    std::string req = "POST /pair/code HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(kCompanionPort) +
                      "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[4096];
    int n;
    while (resp.size() < 65536 && (n = recv(s, buf, sizeof(buf), 0)) > 0)
        resp.append(buf, n);
    closesocket(s);
    int status = 0;
    if (resp.compare(0, 9, "HTTP/1.1 ") == 0 || resp.compare(0, 9, "HTTP/1.0 ") == 0)
        status = atoi(resp.c_str() + 9);
    size_t split = resp.find("\r\n\r\n");
    body = split == std::string::npos ? "" : resp.substr(split + 4);
    // Kestrel answers small JSON bodies with Content-Length; a chunked body would start with the
    // chunk size line. Strip it if present.
    if (resp.find("Transfer-Encoding: chunked") != std::string::npos)
    {
        size_t line = body.find("\r\n");
        size_t end = body.rfind("\r\n0\r\n");
        if (line != std::string::npos && end != std::string::npos && end > line)
            body = body.substr(line + 2, end - line - 2);
    }
    return status == 0 ? -1 : status;
}

// The string value of "key" in a flat JSON object, or the first string of an array value. Enough
// for the companion's fixed answer ({"code":"123456","expiresIn":600,"urls":["http://..."]}).
static std::string JsonString(const std::string& json, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos)
        return "";
    p = json.find_first_not_of(" \t\r\n:[", p + k.size());
    if (p == std::string::npos || json[p] != '"')
        return "";
    size_t end = json.find('"', p + 1);
    return end == std::string::npos ? "" : json.substr(p + 1, end - p - 1);
}

static int JsonInt(const std::string& json, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos)
        return 0;
    p = json.find_first_not_of(" \t\r\n:", p + k.size());
    return p == std::string::npos ? 0 : atoi(json.c_str() + p);
}

static bool StartCompanion(unsigned short encorePort, std::string& error)
{
    std::wstring exe = CompanionPath();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = "The companion was not found at " + Utf8(exe) + ".";
        Log("companion: %s", error.c_str());
        return false;
    }
    std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    std::wstring cmd = L"\"" + exe + L"\" --encore-port " + std::to_wstring(encorePort) + L" --no-browser";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(), &si,
                        &pi))
    {
        error = "Starting the companion failed (error " + std::to_string(GetLastError()) + ").";
        Log("companion: %s", error.c_str());
        return false;
    }
    Log("companion: started %s (pid %lu)", Utf8(exe).c_str(), pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

static DWORD WINAPI LaunchThread(LPVOID param)
{
    LaunchJob job = *static_cast<LaunchJob*>(param);
    delete static_cast<LaunchJob*>(param);
    auto* result = new CompanionResult();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        result->error = "Network initialisation failed.";
    else
    {
        std::string body;
        int status = PostPairCode(body);
        if (status == 0)
        {
            // Not running: start it and wait until it answers (a .NET start takes a few seconds).
            if (StartCompanion(job.encorePort, result->error))
            {
                result->started = true;
                for (int i = 0; i < 100 && status == 0; ++i)
                {
                    Sleep(300);
                    status = PostPairCode(body);
                }
                if (status == 0)
                    result->error = "The companion did not answer within 30 seconds. Check its window.";
            }
        }
        if (status == 200)
        {
            result->code = JsonString(body, "code");
            result->url = JsonString(body, "urls");
            result->expiresMinutes = JsonInt(body, "expiresIn") / 60;
            result->ok = result->code.size() == 6;
            if (!result->ok)
                result->error = "The companion's answer was not understood (an older version?).";
            else if (result->url.empty())
                result->error = "No network address found. Is the PC connected to the Wi-Fi?";
        }
        else if (status != 0)
        {
            result->error = "The program on port " + std::to_string(kCompanionPort) + " answered with status " +
                            std::to_string(status) + " (an older companion, or another program?).";
        }
        WSACleanup();
    }
    Log("companion: pairing request %s%s%s", result->ok ? "ok" : "failed: ", result->ok ? "" : result->error.c_str(),
        result->started ? " (started it)" : "");
    InterlockedExchange(&s_pending, 0);
    if (!PostMessageW(job.wnd, job.msg, 0, reinterpret_cast<LPARAM>(result)))
        delete result;
    return 0;
}

bool RequestCompanionPairing(HWND wnd, UINT resultMsg, unsigned short encorePort)
{
    if (InterlockedCompareExchange(&s_pending, 1, 0) != 0)
        return false;
    auto* job = new LaunchJob{wnd, resultMsg, encorePort};
    HANDLE t = CreateThread(nullptr, 0, LaunchThread, job, 0, nullptr);
    if (!t)
    {
        delete job;
        InterlockedExchange(&s_pending, 0);
        return false;
    }
    CloseHandle(t);
    return true;
}
