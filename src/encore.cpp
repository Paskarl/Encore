// Encore: rescan songs from Clone Hero's song list.
// Runs inside the game (loaded through the version.dll proxy), subclasses the Unity window
// so all IL2CPP calls happen on the Unity main thread, and starts a song rescan when F5 is
// pressed in the song list or another application sends POST /rescan to the loopback HTTP
// listener (trigger_server.cpp). With ENCORE_DUMP=1 it also dumps the analysed classes.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "il2cpp_api.h"
#include "trigger_server.h"

static FILE* g_log = nullptr;
static HWND g_unityWnd = nullptr;
static WNDPROC g_origWndProc = nullptr;
static bool g_initDone = false;
static const UINT WM_POC_INIT = WM_APP + 0x4C48;
static const UINT WM_ENCORE_TRIGGER = WM_APP + 0x4C49; // wParam = TriggerCommand
static const unsigned short kDefaultPort = 47321;

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static std::string TypeName(const Il2CppType* t)
{
    if (!t)
        return "?";
    char* n = il2cpp_type_get_name(t);
    std::string s = n ? n : "?";
    if (n)
        il2cpp_free(n);
    return s;
}

static uintptr_t Rva(const void* p)
{
    return p ? reinterpret_cast<uintptr_t>(p) - reinterpret_cast<uintptr_t>(g_gameAssembly) : 0;
}

static void DumpClass(Il2CppClass* k)
{
    Il2CppClass* parent = il2cpp_class_get_parent(k);
    Log("class %s.%s : %s", il2cpp_class_get_namespace(k), il2cpp_class_get_name(k),
        parent ? il2cpp_class_get_name(parent) : "-");
    void* it = nullptr;
    while (FieldInfo* f = il2cpp_class_get_fields(k, &it))
    {
        bool isStatic = (il2cpp_field_get_flags(f) & kAttrStatic) != 0;
        Log("    field %s%s %s ; off=0x%zx", isStatic ? "static " : "",
            TypeName(il2cpp_field_get_type(f)).c_str(), il2cpp_field_get_name(f),
            isStatic ? 0 : il2cpp_field_get_offset(f));
    }
    it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(k, &it))
    {
        uint32_t iflags = 0;
        bool isStatic = (il2cpp_method_get_flags(m, &iflags) & kAttrStatic) != 0;
        std::string sig;
        uint32_t pc = il2cpp_method_get_param_count(m);
        for (uint32_t i = 0; i < pc; ++i)
        {
            if (i)
                sig += ", ";
            sig += TypeName(il2cpp_method_get_param(m, i)) + " " + il2cpp_method_get_param_name(m, i);
        }
        Log("    method %s%s %s(%s) ; RVA=0x%llx", isStatic ? "static " : "",
            TypeName(il2cpp_method_get_return_type(m)).c_str(), il2cpp_method_get_name(m), sig.c_str(),
            static_cast<unsigned long long>(Rva(il2cpp_method_pointer(m))));
    }
}

static bool NameMatches(const char* name)
{
    static const char* kFilters[] = {"Scan", "SongSelect", "SongLibrary", "Rescan"};
    for (const char* f : kFilters)
        if (strstr(name, f))
            return true;
    return false;
}

static const Il2CppImage* FindImage(const char* name)
{
    size_t count = 0;
    const Il2CppAssembly** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
    for (size_t a = 0; a < count; ++a)
    {
        const Il2CppImage* img = il2cpp_assembly_get_image(assemblies[a]);
        if (_stricmp(il2cpp_image_get_name(img), name) == 0)
            return img;
    }
    return nullptr;
}

static void DumpInteresting(const Il2CppImage* img)
{
    size_t n = il2cpp_image_get_class_count(img);
    Log("== %s: %zu classes", il2cpp_image_get_name(img), n);
    for (size_t i = 0; i < n; ++i)
    {
        Il2CppClass* k = il2cpp_image_get_class(img, i);
        Il2CppClass* decl = il2cpp_class_get_declaring_type(k);
        if (NameMatches(il2cpp_class_get_name(k)) || (decl && NameMatches(il2cpp_class_get_name(decl))))
        {
            if (decl)
                Log("(nested in %s)", il2cpp_class_get_name(decl));
            DumpClass(k);
        }
    }
}

// Writes "RVA<TAB>DeclaringType/Class<TAB>method" for every method of the image so call sites
// found in GameAssembly.dll can be attributed to a class.
static void DumpMethodMap(const Il2CppImage* img, const wchar_t* path)
{
    FILE* out = _wfopen(path, L"w");
    if (!out)
        return;
    size_t n = il2cpp_image_get_class_count(img);
    for (size_t i = 0; i < n; ++i)
    {
        Il2CppClass* k = il2cpp_image_get_class(img, i);
        Il2CppClass* decl = il2cpp_class_get_declaring_type(k);
        void* it = nullptr;
        while (const MethodInfo* m = il2cpp_class_get_methods(k, &it))
        {
            fprintf(out, "%llx\t%s%s%s\t%s(%u)\n", static_cast<unsigned long long>(Rva(il2cpp_method_pointer(m))),
                    decl ? il2cpp_class_get_name(decl) : "", decl ? "/" : "", il2cpp_class_get_name(k),
                    il2cpp_method_get_name(m), il2cpp_method_get_param_count(m));
        }
    }
    fclose(out);
}

// ---------------------------------------------------------------------------------------------
// Rescan from the song list.
//
// Findings (Clone Hero v1.1.0.6142-final, GameAssembly.dll, see README.md):
//   SongScan.StartScan(bool fullScan) -> Coroutine   (obfuscated; @+0x1b5c10 in that build)
//       if (!isScanning) coroutine = StartCoroutine(Scan(fullScan));
//   SongScan.Show()                                   (obfuscated; @+0x1b5d80 in that build)
//       container.SetActive(true); waitForFinish = true;
//   Settings "Scan Songs" and MainMenu both do Show() + StartScan(true).
//   When the scan coroutine finishes it calls songScan.songSelect.<RefreshList>() if
//   songScan.songSelect is set, then hides the overlay again.
//
// Nothing here depends on addresses or obfuscated names, so it survives game updates:
//   - Class and field names (SongScan, isScanning, songSelect, container, waitForFinish) are
//     not obfuscated and are looked up by name.
//   - Show() is replaced by doing its two field writes directly.
//   - SongScan has four `Coroutine X(bool)` methods; three are obfuscator decoys that nothing
//     calls. StartScan is the only one with call/jmp sites outside SongScan, which is found by
//     scanning GameAssembly.dll's executable sections at startup.
// ---------------------------------------------------------------------------------------------

struct GameRefs
{
    Il2CppClass* songScan = nullptr;
    Il2CppClass* songSelect = nullptr;
    const MethodInfo* startScan = nullptr;
    FieldInfo* isScanning = nullptr;
    FieldInfo* songSelectField = nullptr;
    FieldInfo* container = nullptr;
    FieldInfo* waitForFinish = nullptr;
    const MethodInfo* setActive = nullptr;            // UnityEngine.GameObject
    const MethodInfo* findObjectsOfType = nullptr;    // UnityEngine.Object
    const MethodInfo* isActiveAndEnabled = nullptr;   // UnityEngine.Behaviour
    bool ok = false;
};
static GameRefs g_refs;

// Collects the native entry points of every method of k and its nested types (coroutine
// state machines, lambdas), i.e. all code that belongs to the class itself.
static void CollectMethodBodies(Il2CppClass* k, std::vector<uintptr_t>& out)
{
    void* it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(k, &it))
        if (void* p = il2cpp_method_pointer(m))
            out.push_back(reinterpret_cast<uintptr_t>(p));
    it = nullptr;
    while (Il2CppClass* nested = il2cpp_class_get_nested_types(k, &it))
        CollectMethodBodies(nested, out);
}

// Counts `call rel32` / `jmp rel32` sites in GameAssembly.dll's executable sections that target
// each candidate, ignoring sites inside the class's own methods. Sites must lie inside a function
// known to the x64 unwind tables, which filters out byte patterns in padding and data.
static std::vector<int> CountExternalCallers(const std::vector<uintptr_t>& targets,
                                             const std::vector<uintptr_t>& ownBodies)
{
    std::vector<int> counts(targets.size(), 0);
    auto base = reinterpret_cast<uint8_t*>(g_gameAssembly);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) || sec->Misc.VirtualSize < 5)
            continue;
        uint8_t* code = base + sec->VirtualAddress;
        size_t size = sec->Misc.VirtualSize;
        for (size_t i = 0; i + 5 <= size; ++i)
        {
            if (code[i] != 0xE8 && code[i] != 0xE9)
                continue;
            int32_t rel;
            memcpy(&rel, code + i + 1, sizeof(rel));
            uintptr_t site = reinterpret_cast<uintptr_t>(code + i);
            uintptr_t target = site + 5 + rel;
            for (size_t t = 0; t < targets.size(); ++t)
            {
                if (target != targets[t])
                    continue;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(site, &imageBase, nullptr);
                if (!fn)
                    break;
                uintptr_t caller = static_cast<uintptr_t>(imageBase) + fn->BeginAddress;
                if (std::find(ownBodies.begin(), ownBodies.end(), caller) == ownBodies.end())
                    ++counts[t];
                break;
            }
        }
    }
    return counts;
}

static const MethodInfo* IdentifyStartScan(Il2CppClass* songScan)
{
    std::vector<const MethodInfo*> candidates;
    void* it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(songScan, &it))
    {
        uint32_t iflags = 0;
        if ((il2cpp_method_get_flags(m, &iflags) & kAttrStatic) || il2cpp_method_get_param_count(m) != 1 ||
            !il2cpp_method_pointer(m))
            continue;
        if (TypeName(il2cpp_method_get_return_type(m)) == "UnityEngine.Coroutine" &&
            TypeName(il2cpp_method_get_param(m, 0)) == "System.Boolean")
            candidates.push_back(m);
    }
    std::vector<uintptr_t> targets, own;
    for (const MethodInfo* m : candidates)
        targets.push_back(reinterpret_cast<uintptr_t>(il2cpp_method_pointer(m)));
    CollectMethodBodies(songScan, own);
    std::vector<int> counts = CountExternalCallers(targets, own);

    const MethodInfo* found = nullptr;
    int withCallers = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        Log("  StartScan candidate RVA=0x%llx external callers=%d",
            static_cast<unsigned long long>(Rva(il2cpp_method_pointer(candidates[i]))), counts[i]);
        if (counts[i] > 0)
        {
            ++withCallers;
            found = candidates[i];
        }
    }
    if (withCallers != 1)
    {
        // Do not guess: if the decoys gained callers (or the real one lost its callers) the
        // heuristic no longer holds for this build.
        Log("StartScan ambiguous: %d of %zu Coroutine(bool) candidates have external callers", withCallers,
            candidates.size());
        return nullptr;
    }
    return found;
}

static bool ResolveGameRefs(const Il2CppImage* game)
{
    GameRefs& r = g_refs;
    r.songScan = il2cpp_class_from_name(game, "", "SongScan");
    r.songSelect = il2cpp_class_from_name(game, "", "SongSelect");
    const Il2CppImage* core = FindImage("UnityEngine.CoreModule.dll");
    Il2CppClass* unityObject = core ? il2cpp_class_from_name(core, "UnityEngine", "Object") : nullptr;
    Il2CppClass* behaviour = core ? il2cpp_class_from_name(core, "UnityEngine", "Behaviour") : nullptr;
    Il2CppClass* gameObject = core ? il2cpp_class_from_name(core, "UnityEngine", "GameObject") : nullptr;
    if (!r.songScan || !r.songSelect || !unityObject || !behaviour || !gameObject)
    {
        Log("resolve failed: SongScan=%p SongSelect=%p Object=%p Behaviour=%p GameObject=%p", r.songScan,
            r.songSelect, unityObject, behaviour, gameObject);
        return false;
    }
    r.startScan = IdentifyStartScan(r.songScan);
    r.isScanning = il2cpp_class_get_field_from_name(r.songScan, "isScanning");
    r.songSelectField = il2cpp_class_get_field_from_name(r.songScan, "songSelect");
    r.container = il2cpp_class_get_field_from_name(r.songScan, "container");
    r.waitForFinish = il2cpp_class_get_field_from_name(r.songScan, "waitForFinish");
    r.setActive = il2cpp_class_get_method_from_name(gameObject, "SetActive", 1);
    // Resources.FindObjectsOfTypeAll is stripped from this build; Object.FindObjectsOfType(Type)
    // survives and only returns objects in loaded scenes.
    r.findObjectsOfType = il2cpp_class_get_method_from_name(unityObject, "FindObjectsOfType", 1);
    r.isActiveAndEnabled = il2cpp_class_get_method_from_name(behaviour, "get_isActiveAndEnabled", 0);
    r.ok = r.startScan && r.isScanning && r.songSelectField && r.container && r.waitForFinish && r.setActive &&
           r.findObjectsOfType && r.isActiveAndEnabled;
    Log("resolve: StartScan=%p (RVA 0x%llx) isScanning=%p songSelect=%p container=%p waitForFinish=%p "
        "SetActive=%p FindObjectsOfType=%p isActiveAndEnabled=%p -> %s",
        r.startScan, static_cast<unsigned long long>(Rva(il2cpp_method_pointer(r.startScan))), r.isScanning,
        r.songSelectField, r.container, r.waitForFinish, r.setActive, r.findObjectsOfType, r.isActiveAndEnabled,
        r.ok ? "ok" : "FAILED (game structure changed?)");
    return r.ok;
}

static Il2CppObject* Invoke(const MethodInfo* m, void* obj, void** args)
{
    Il2CppObject* exc = nullptr;
    Il2CppObject* ret = il2cpp_runtime_invoke(m, obj, args, &exc);
    if (exc)
    {
        Log("exception while invoking %s", il2cpp_method_get_name(m));
        return nullptr;
    }
    return ret;
}

static bool IsActiveAndEnabled(Il2CppObject* behaviour)
{
    Il2CppObject* boxed = Invoke(g_refs.isActiveAndEnabled, behaviour, nullptr);
    return boxed && *static_cast<bool*>(il2cpp_object_unbox(boxed));
}

// Returns the first active-and-enabled scene instance of the given MonoBehaviour class.
static Il2CppObject* FindActiveInstance(Il2CppClass* k)
{
    void* args[] = {il2cpp_type_get_object(il2cpp_class_get_type(k))};
    Il2CppObject* arr = Invoke(g_refs.findObjectsOfType, nullptr, args);
    if (!arr)
        return nullptr;
    uint32_t n = il2cpp_array_length(arr);
    auto items = reinterpret_cast<Il2CppObject**>(reinterpret_cast<char*>(arr) + 0x20); // Il2CppArray::vector
    for (uint32_t i = 0; i < n; ++i)
        if (items[i] && IsActiveAndEnabled(items[i]))
            return items[i];
    return nullptr;
}

// Starts a full rescan if the song list is open and no scan is running. Used by F5 and by the
// HTTP trigger; source names the trigger in the log. Outside the song list the request is
// refused: the scan coroutine would also run from other menus, but during gameplay or a scene
// load that is not safe, and the list only refreshes when a song list is open anyway.
static RescanResult TriggerRescanFromSongList(const char* source)
{
    if (!g_initDone || !g_refs.ok)
    {
        Log("%s ignored: Encore is not ready", source);
        return kRescanNotReady;
    }
    Il2CppObject* songSelect = FindActiveInstance(g_refs.songSelect);
    if (!songSelect)
    {
        Log("%s ignored: song list is not open", source);
        return kRescanNotInSongList;
    }
    Il2CppObject* songScan = FindActiveInstance(g_refs.songScan);
    if (!songScan)
    {
        Log("%s ignored: no active SongScan instance", source);
        return kRescanFailed;
    }
    bool scanning = false;
    il2cpp_field_get_value(songScan, g_refs.isScanning, &scanning);
    if (scanning)
    {
        Log("%s ignored: scan already running", source);
        return kRescanBusy;
    }
    // Make sure the scan coroutine refreshes this song list when it finishes.
    Il2CppObject* linked = nullptr;
    il2cpp_field_get_value(songScan, g_refs.songSelectField, &linked);
    if (linked != songSelect)
    {
        il2cpp_field_set_value(songScan, g_refs.songSelectField, songSelect);
        Log("linked SongScan.songSelect (was %p)", linked);
    }
    // Same sequence as the main menu: Show() (container.SetActive(true); waitForFinish = true),
    // then StartScan(fullScan: true).
    Il2CppObject* container = nullptr;
    il2cpp_field_get_value(songScan, g_refs.container, &container);
    if (!container)
    {
        Log("%s ignored: SongScan.container is null", source);
        return kRescanFailed;
    }
    bool on = true;
    void* activeArgs[] = {&on};
    Invoke(g_refs.setActive, container, activeArgs);
    il2cpp_field_set_value(songScan, g_refs.waitForFinish, &on);
    bool fullScan = true;
    void* args[] = {&fullScan};
    Invoke(g_refs.startScan, songScan, args);
    Log("rescan started from song list (%s)", source);
    return kRescanStarted;
}

// Answers GET /status. Runs on the main thread like everything else that touches IL2CPP.
static LRESULT QueryStatus()
{
    LRESULT r = kStatusValid;
    if (!g_initDone || !g_refs.ok)
        return r;
    r |= kStatusReady;
    if (FindActiveInstance(g_refs.songSelect))
        r |= kStatusSongList;
    if (Il2CppObject* songScan = FindActiveInstance(g_refs.songScan))
    {
        bool scanning = false;
        il2cpp_field_get_value(songScan, g_refs.isScanning, &scanning);
        if (scanning)
            r |= kStatusScanning;
    }
    return r;
}

static bool DumpRequested()
{
    wchar_t v[8];
    return GetEnvironmentVariableW(L"ENCORE_DUMP", v, 8) > 0 && v[0] == L'1';
}

static const UINT_PTR kInitTimer = 0x4C48;

static void OnMainThreadInit(HWND hwnd)
{
    if (g_initDone)
        return;
    const Il2CppImage* game = FindImage("CloneHero.dll");
    if (!game)
        game = FindImage("Assembly-CSharp.dll");
    if (!game)
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            size_t count = 0;
            const Il2CppAssembly** as = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &count);
            Log("game assembly not found yet, %zu assemblies:", count);
            for (size_t a = 0; a < count; ++a)
                Log("  %s", il2cpp_image_get_name(il2cpp_assembly_get_image(as[a])));
        }
        SetTimer(hwnd, kInitTimer, 500, nullptr); // assemblies not loaded yet, retry
        return;
    }
    KillTimer(hwnd, kInitTimer);
    g_initDone = true;
    Log("main-thread init, GameAssembly base=%p", g_gameAssembly);
    if (DumpRequested())
    {
        DumpInteresting(game);
        wchar_t mapPath[MAX_PATH];
        GetModuleFileNameW(nullptr, mapPath, MAX_PATH);
        if (wchar_t* slash = wcsrchr(mapPath, L'\\'))
            lstrcpyW(slash + 1, L"encore_methods.tsv");
        DumpMethodMap(game, mapPath);
        Log("dump done");
    }
    if (ResolveGameRefs(game))
        Log("ready: press F5 in the song list (or POST /rescan) to rescan songs");
}

static LRESULT CALLBACK HookWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static bool first = true;
    if (first)
    {
        first = false;
        Log("wndproc hook active (msg 0x%x)", msg);
    }
    if (msg == WM_POC_INIT || (msg == WM_TIMER && wp == kInitTimer))
    {
        OnMainThreadInit(hwnd);
        return 0;
    }
    // Sent by the HTTP listener thread with SendMessageTimeout; handled here on the main thread.
    if (msg == WM_ENCORE_TRIGGER)
    {
        if (wp == kCmdRescan)
            return TriggerRescanFromSongList("http");
        if (wp == kCmdStatus)
            return QueryStatus();
        return 0;
    }
    // Bit 30 of lParam = key was already down (auto-repeat).
    if (msg == WM_KEYDOWN && wp == VK_F5 && !(lp & (1 << 30)) && g_initDone)
        TriggerRescanFromSongList("F5");
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

static BOOL CALLBACK FindUnityWindow(HWND hwnd, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"UnityWndClass") == 0)
    {
        g_unityWnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

// ENCORE_PORT overrides the listener port; 0 turns the trigger interface off.
static unsigned short TriggerPort()
{
    wchar_t v[16];
    if (GetEnvironmentVariableW(L"ENCORE_PORT", v, 16) == 0)
        return kDefaultPort;
    wchar_t* end = nullptr;
    long p = wcstol(v, &end, 10);
    if (end == v || *end || p < 0 || p > 65535)
    {
        Log("http: invalid ENCORE_PORT, using %u", kDefaultPort);
        return kDefaultPort;
    }
    return static_cast<unsigned short>(p);
}

static DWORD WINAPI Bootstrap(LPVOID)
{
    // Wait for GameAssembly.dll and a populated IL2CPP domain.
    while (!il2cpp_resolve_api())
        Sleep(100);
    Log("il2cpp API resolved");
    // Only touch state that is safe off the main thread: il2cpp_get_corlib just reads a
    // global, while il2cpp_domain_get would allocate through the GC from an unattached thread.
    while (!il2cpp_get_corlib())
        Sleep(200);
    while (!g_unityWnd)
    {
        EnumWindows(FindUnityWindow, 0);
        Sleep(200);
    }
    Log("unity window %p", g_unityWnd);
    g_origWndProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g_unityWnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookWndProc)));
    PostMessageW(g_unityWnd, WM_POC_INIT, 0, 0);
    if (unsigned short port = TriggerPort())
        StartTriggerServer(g_unityWnd, WM_ENCORE_TRIGGER, port);
    else
        Log("http: trigger interface off (ENCORE_PORT=0)");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(inst);
        // UnityCrashHandler64.exe sits in the same folder and loads this version.dll too. Only the
        // game itself has UnityPlayer.dll (the module importing us) mapped; elsewhere stay a plain
        // proxy so the crash handler does not overwrite encore.log or start a bootstrap thread.
        if (!GetModuleHandleW(L"UnityPlayer.dll"))
            return TRUE;
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash)
            lstrcpyW(slash + 1, L"encore.log");
        g_log = _wfsopen(path, L"w", _SH_DENYWR);
        Log("Encore loaded");
        CloseHandle(CreateThread(nullptr, 0, Bootstrap, nullptr, 0, nullptr));
    }
    return TRUE;
}
