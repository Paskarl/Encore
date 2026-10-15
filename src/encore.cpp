// Encore: rescan songs from Clone Hero's song list.
// Runs inside the game (loaded through the version.dll proxy), subclasses the Unity window
// so all IL2CPP calls happen on the Unity main thread, and starts a song rescan when F5 is
// pressed in the song list or another application sends POST /rescan to the loopback HTTP
// listener (trigger_server.cpp). The listener can also queue songs into the game's setlist
// (POST /queue). Encore also adds an "Encore" main-menu entry that starts the companion. With
// ENCORE_DUMP=1 it also dumps the analysed classes.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include "il2cpp_api.h"
#include "trigger_server.h"
#include "companion_launcher.h"

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

// Class name substrings to dump. ENCORE_DUMP_FILTER (comma-separated) replaces the default, which
// covers the classes analysed for the rescan; "*" dumps every class (needed to find obfuscated
// classes by their field types).
static bool NameMatches(const char* name)
{
    static std::vector<std::string> filters = [] {
        std::vector<std::string> list;
        char buf[1024];
        DWORD n = GetEnvironmentVariableA("ENCORE_DUMP_FILTER", buf, sizeof(buf));
        std::string spec = (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : "Scan,SongSelect,SongLibrary,Rescan";
        size_t start = 0;
        while (start <= spec.size())
        {
            size_t end = spec.find(',', start);
            if (end == std::string::npos)
                end = spec.size();
            if (end > start)
                list.push_back(spec.substr(start, end - start));
            start = end + 1;
        }
        return list;
    }();
    for (const std::string& f : filters)
        if (f == "*" || strstr(name, f.c_str()))
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

static bool g_invokeFailed = false; // the last Invoke threw

static Il2CppObject* Invoke(const MethodInfo* m, void* obj, void** args)
{
    Il2CppObject* exc = nullptr;
    Il2CppObject* ret = il2cpp_runtime_invoke(m, obj, args, &exc);
    g_invokeFailed = exc != nullptr;
    if (exc)
    {
        char text[1024] = "", stack[4096] = "";
        il2cpp_format_exception(exc, text, sizeof(text));
        il2cpp_format_stack_trace(exc, stack, sizeof(stack));
        Log("exception while invoking %s: %s\n%s", il2cpp_method_get_name(m), text, stack);
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
// A song scan empties the setlist (at scan start). Rescans started by Encore save it first and
// put it back when the scan has finished; see "Setlist across rescans" below.
static void SaveSetlistForRescan();
static void RestoreSetlistIfScanDone();

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
    SaveSetlistForRescan();
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
    RestoreSetlistIfScanDone();
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

// ---------------------------------------------------------------------------------------------
// Song queue: the game's setlist is the queue (docs/ANALYSIS.md section 7).
//
//   - The song library is an obfuscated static class, found by its field types: a
//     Dictionary<checksum, List<SongEntry>> plus several List<SongEntry>.
//   - The setlist is three parallel static lists in that class, declared in a row:
//     List<SongEntry> songs, List<int> song speeds, List<string> (one string per entry).
//   - EndOfSong's "continue" reads the next song live from those lists, so appending to them
//     while a setlist plays queues a song after the current setlist songs.
//   - In the song list, SetlistMenu.Add(SongEntry, int, string) appends to the same lists and
//     updates the setlist panel. SetlistMenu has three methods with that signature (obfuscator
//     copies); the real one is called from far more places than the others.
//   - EndOfSong.Start decides whether the finished song was the last setlist song (a bool, and
//     the "setlist_end" instead of the "setlist" button panel). A song queued after that point
//     would be dropped by "continue", so queueing on the end screen also undoes that decision.
// ---------------------------------------------------------------------------------------------

struct QueueRefs
{
    Il2CppClass* library = nullptr;
    std::vector<FieldInfo*> songLists; // every static List<SongEntry> except the setlist
    FieldInfo* setSongs = nullptr;     // setlist triple
    FieldInfo* setSpeeds = nullptr;
    FieldInfo* setStrings = nullptr;
    const MethodInfo* checksumString = nullptr; // SongEntry getters
    const MethodInfo* nameStripped = nullptr;
    const MethodInfo* artistStripped = nullptr;
    const MethodInfo* albumStripped = nullptr;    // optional, for GET /library
    FieldInfo* songLength = nullptr;              // optional: song length in ms, null if not identified
    FieldInfo* gvInstance = nullptr; // GlobalVariables
    FieldInfo* gvPractice = nullptr;
    FieldInfo* gvPlayingSetlist = nullptr;
    FieldInfo* gvSetlistIndex = nullptr;
    FieldInfo* gvOriginalSpeed = nullptr;
    FieldInfo* ssSetlistMenu = nullptr;           // SongSelect.setlistMenu
    const MethodInfo* setlistAdd = nullptr;       // SetlistMenu.Add, null if ambiguous
    Il2CppClass* endOfSong = nullptr;
    Il2CppClass* gameManager = nullptr;
    FieldInfo* eosIsLastSong = nullptr;           // null if not identified
    FieldInfo* eosSetlistPanel = nullptr;
    FieldInfo* eosSetlistEndPanel = nullptr;
    const MethodInfo* activeSelf = nullptr;       // GameObject.get_activeSelf
    bool ok = false;
};
static QueueRefs g_queue;

static bool EndsWith(const std::string& s, const char* suffix)
{
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static bool IsStatic(FieldInfo* f)
{
    return (il2cpp_field_get_flags(f) & kAttrStatic) != 0;
}

// Finds the song library class: only static fields, among them a Dictionary<_, List<SongEntry>>
// and at least two List<SongEntry>. Must be unique in the image.
static Il2CppClass* FindSongLibrary(const Il2CppImage* game)
{
    Il2CppClass* found = nullptr;
    int matches = 0;
    size_t n = il2cpp_image_get_class_count(game);
    for (size_t i = 0; i < n; ++i)
    {
        Il2CppClass* k = il2cpp_image_get_class(game, i);
        int lists = 0, fields = 0;
        bool dict = false, allStatic = true;
        void* it = nullptr;
        while (FieldInfo* f = il2cpp_class_get_fields(k, &it))
        {
            ++fields;
            if (!IsStatic(f))
            {
                allStatic = false;
                break;
            }
            std::string t = TypeName(il2cpp_field_get_type(f));
            if (t == "System.Collections.Generic.List<SongEntry>")
                ++lists;
            else if (t.rfind("System.Collections.Generic.Dictionary<", 0) == 0 &&
                     EndsWith(t, ",System.Collections.Generic.List<SongEntry>>"))
                dict = true;
        }
        if (allStatic && dict && lists >= 2)
        {
            found = k;
            ++matches;
        }
    }
    if (matches != 1)
    {
        Log("queue: song library class not unique (%d candidates)", matches);
        return nullptr;
    }
    return found;
}

// Identifies SetlistMenu.Add(SongEntry, int, string) among its obfuscator copies by outside
// callers (8 for the real one in v1.1.0.6142, 1 and 0 for the copies). Accepted only if one
// candidate has clearly the most: at least 3, and at least twice as many as any other.
static const MethodInfo* IdentifySetlistAdd(Il2CppClass* setlistMenu)
{
    std::vector<const MethodInfo*> candidates;
    void* it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(setlistMenu, &it))
    {
        uint32_t iflags = 0;
        if ((il2cpp_method_get_flags(m, &iflags) & kAttrStatic) || il2cpp_method_get_param_count(m) != 3 ||
            !il2cpp_method_pointer(m))
            continue;
        if (TypeName(il2cpp_method_get_return_type(m)) == "System.Void" &&
            TypeName(il2cpp_method_get_param(m, 0)) == "SongEntry" &&
            TypeName(il2cpp_method_get_param(m, 1)) == "System.Int32" &&
            TypeName(il2cpp_method_get_param(m, 2)) == "System.String")
            candidates.push_back(m);
    }
    std::vector<uintptr_t> targets, own;
    for (const MethodInfo* m : candidates)
        targets.push_back(reinterpret_cast<uintptr_t>(il2cpp_method_pointer(m)));
    CollectMethodBodies(setlistMenu, own);
    std::vector<int> counts = CountExternalCallers(targets, own);
    int best = -1, bestCount = 0, second = 0;
    for (size_t i = 0; i < candidates.size(); ++i)
    {
        Log("  SetlistMenu.Add candidate RVA=0x%llx external callers=%d",
            static_cast<unsigned long long>(Rva(il2cpp_method_pointer(candidates[i]))), counts[i]);
        if (counts[i] > bestCount)
        {
            second = bestCount;
            bestCount = counts[i];
            best = static_cast<int>(i);
        }
        else if (counts[i] > second)
            second = counts[i];
    }
    if (best < 0 || bestCount < 3 || bestCount < 2 * second)
    {
        Log("SetlistMenu.Add ambiguous (best %d callers, next %d): queueing in the song list appends to the "
            "setlist lists without refreshing the setlist panel", bestCount, second);
        return nullptr;
    }
    return candidates[best];
}

// EndOfSong's "this was the last setlist song" flag: its first own bool field that is not an
// auto-property backing field (offset 0xa1 in v1.1.0.6142). Only ever written when it is true
// and the "setlist_end" panel is showing, which confirms the identification at runtime.
static FieldInfo* FindIsLastSong(Il2CppClass* endOfSong)
{
    void* it = nullptr;
    while (FieldInfo* f = il2cpp_class_get_fields(endOfSong, &it))
    {
        if (IsStatic(f) || TypeName(il2cpp_field_get_type(f)) != "System.Boolean")
            continue;
        if (strstr(il2cpp_field_get_name(f), "k__BackingField"))
            continue;
        return f;
    }
    return nullptr;
}

// SongEntry's song length (ms) has an obfuscated field name. get_HasLength is `length > 0`; its
// first instruction is `cmp dword ptr [rcx+disp], 0`, and the Int32 instance field at that offset
// is the length (0x80 in v1.1.0.6142).
static FieldInfo* FindSongLength(Il2CppClass* songEntry)
{
    const MethodInfo* hasLength = il2cpp_class_get_method_from_name(songEntry, "get_HasLength", 0);
    auto code = hasLength ? static_cast<const uint8_t*>(il2cpp_method_pointer(hasLength)) : nullptr;
    if (!code)
        return nullptr;
    size_t disp;
    if (code[0] == 0x83 && code[1] == 0xB9 && code[6] == 0x00) // cmp dword [rcx+disp32], 0
    {
        int32_t d;
        memcpy(&d, code + 2, sizeof(d));
        disp = static_cast<size_t>(d);
    }
    else if (code[0] == 0x83 && code[1] == 0x79 && code[3] == 0x00) // cmp dword [rcx+disp8], 0
        disp = code[2];
    else
    {
        Log("queue: get_HasLength has an unexpected shape (%02x %02x), song length not available", code[0], code[1]);
        return nullptr;
    }
    void* it = nullptr;
    while (FieldInfo* f = il2cpp_class_get_fields(songEntry, &it))
        if (!IsStatic(f) && il2cpp_field_get_offset(f) == disp &&
            TypeName(il2cpp_field_get_type(f)) == "System.Int32")
            return f;
    Log("queue: no Int32 field at SongEntry+0x%zx, song length not available", disp);
    return nullptr;
}

static bool ResolveQueueRefs(const Il2CppImage* game)
{
    QueueRefs& q = g_queue;
    q.library = FindSongLibrary(game);
    if (q.library)
    {
        std::vector<FieldInfo*> fields;
        void* it = nullptr;
        while (FieldInfo* f = il2cpp_class_get_fields(q.library, &it))
            fields.push_back(f);
        int triples = 0;
        for (size_t i = 0; i + 2 < fields.size(); ++i)
        {
            if (TypeName(il2cpp_field_get_type(fields[i])) == "System.Collections.Generic.List<SongEntry>" &&
                TypeName(il2cpp_field_get_type(fields[i + 1])) == "System.Collections.Generic.List<System.Int32>" &&
                TypeName(il2cpp_field_get_type(fields[i + 2])) == "System.Collections.Generic.List<System.String>")
            {
                ++triples;
                q.setSongs = fields[i];
                q.setSpeeds = fields[i + 1];
                q.setStrings = fields[i + 2];
            }
        }
        if (triples != 1)
        {
            Log("queue: setlist lists not unique (%d candidates)", triples);
            q.setSongs = q.setSpeeds = q.setStrings = nullptr;
        }
        for (FieldInfo* f : fields)
            if (f != q.setSongs && TypeName(il2cpp_field_get_type(f)) == "System.Collections.Generic.List<SongEntry>")
                q.songLists.push_back(f);
    }
    Il2CppClass* songEntry = il2cpp_class_from_name(game, "", "SongEntry");
    if (songEntry)
    {
        q.checksumString = il2cpp_class_get_method_from_name(songEntry, "get_ChecksumString", 0);
        q.nameStripped = il2cpp_class_get_method_from_name(songEntry, "get_Name_StrippedTags", 0);
        q.artistStripped = il2cpp_class_get_method_from_name(songEntry, "get_Artist_StrippedTags", 0);
        q.albumStripped = il2cpp_class_get_method_from_name(songEntry, "get_Album_StrippedTags", 0);
        q.songLength = FindSongLength(songEntry);
    }
    if (Il2CppClass* gv = il2cpp_class_from_name(game, "", "GlobalVariables"))
    {
        q.gvInstance = il2cpp_class_get_field_from_name(gv, "instance");
        q.gvPractice = il2cpp_class_get_field_from_name(gv, "isPracticeEnabled");
        q.gvPlayingSetlist = il2cpp_class_get_field_from_name(gv, "isPlayingSetlist");
        q.gvSetlistIndex = il2cpp_class_get_field_from_name(gv, "setlistIndex");
        q.gvOriginalSpeed = il2cpp_class_get_field_from_name(gv, "originalSongSpeed");
    }
    q.ssSetlistMenu = il2cpp_class_get_field_from_name(g_refs.songSelect, "setlistMenu");
    if (Il2CppClass* setlistMenu = il2cpp_class_from_name(game, "", "SetlistMenu"))
        q.setlistAdd = IdentifySetlistAdd(setlistMenu);
    q.endOfSong = il2cpp_class_from_name(game, "", "EndOfSong");
    q.gameManager = il2cpp_class_from_name(game, "", "GameManager");
    if (q.endOfSong)
    {
        q.eosIsLastSong = FindIsLastSong(q.endOfSong);
        q.eosSetlistPanel = il2cpp_class_get_field_from_name(q.endOfSong, "setlist");
        q.eosSetlistEndPanel = il2cpp_class_get_field_from_name(q.endOfSong, "setlist_end");
    }
    const Il2CppImage* core = FindImage("UnityEngine.CoreModule.dll");
    if (Il2CppClass* gameObject = core ? il2cpp_class_from_name(core, "UnityEngine", "GameObject") : nullptr)
        q.activeSelf = il2cpp_class_get_method_from_name(gameObject, "get_activeSelf", 0);

    q.ok = q.library && q.setSongs && !q.songLists.empty() && q.checksumString && q.nameStripped &&
           q.artistStripped && q.gvInstance && q.gvPractice && q.gvPlayingSetlist && q.gvSetlistIndex &&
           q.gvOriginalSpeed && q.ssSetlistMenu && q.endOfSong && q.gameManager;
    Log("queue resolve: library=%s setlist=%s/%s/%s (static offsets 0x%zx/0x%zx/0x%zx) songLists=%zu "
        "SetlistMenu.Add=%s isLastSong=%s (off 0x%zx) setlist panels=%p/%p album=%s length=%s (off 0x%zx) -> %s",
        q.library ? il2cpp_class_get_name(q.library) : "-", q.setSongs ? il2cpp_field_get_name(q.setSongs) : "-",
        q.setSpeeds ? il2cpp_field_get_name(q.setSpeeds) : "-", q.setStrings ? il2cpp_field_get_name(q.setStrings) : "-",
        q.setSongs ? il2cpp_field_get_offset(q.setSongs) : 0, q.setSpeeds ? il2cpp_field_get_offset(q.setSpeeds) : 0,
        q.setStrings ? il2cpp_field_get_offset(q.setStrings) : 0, q.songLists.size(),
        q.setlistAdd ? "ok" : "not identified", q.eosIsLastSong ? "ok" : "not identified",
        q.eosIsLastSong ? il2cpp_field_get_offset(q.eosIsLastSong) : 0, q.eosSetlistPanel, q.eosSetlistEndPanel,
        q.albumStripped ? "ok" : "-", q.songLength ? "ok" : "not identified",
        q.songLength ? il2cpp_field_get_offset(q.songLength) : 0, q.ok ? "ok" : "FAILED (game structure changed?)");
    return q.ok;
}

// --- Managed object helpers --------------------------------------------------------------------

// System.Collections.Generic.List<T>: _items at 0x10, _size at 0x18. Arrays: elements at 0x20.
static int ListCount(Il2CppObject* list)
{
    return list ? *reinterpret_cast<int*>(reinterpret_cast<char*>(list) + 0x18) : 0;
}

template <typename T>
static T ListItem(Il2CppObject* list, int i)
{
    char* items = *reinterpret_cast<char**>(reinterpret_cast<char*>(list) + 0x10);
    return reinterpret_cast<T*>(items + 0x20)[i];
}

// Appends through List<T>.Add, so the list grows and bumps its version like game code would.
static bool ListAdd(Il2CppObject* list, void* arg)
{
    const MethodInfo* add = il2cpp_class_get_method_from_name(il2cpp_object_get_class(list), "Add", 1);
    if (!add)
        return false;
    void* args[] = {arg};
    Il2CppObject* exc = nullptr;
    il2cpp_runtime_invoke(add, list, args, &exc);
    return exc == nullptr;
}

static Il2CppObject* StaticObject(FieldInfo* f)
{
    Il2CppObject* v = nullptr;
    il2cpp_field_static_get_value(f, &v);
    return v;
}

// System.String: length at 0x10, UTF-16 characters at 0x14.
static std::wstring WideString(Il2CppString* s)
{
    if (!s)
        return L"";
    int len = *reinterpret_cast<int*>(reinterpret_cast<char*>(s) + 0x10);
    return std::wstring(reinterpret_cast<wchar_t*>(reinterpret_cast<char*>(s) + 0x14), len);
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

static std::wstring Wide(const std::string& s)
{
    if (s.empty())
        return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

static std::wstring CallString(const MethodInfo* m, Il2CppObject* obj)
{
    return WideString(reinterpret_cast<Il2CppString*>(Invoke(m, obj, nullptr)));
}

static bool SameText(const std::wstring& a, const std::wstring& b)
{
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

static std::string JsonString(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += static_cast<char>(c);
        }
        else if (c < 0x20)
        {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        }
        else
            out += static_cast<char>(c);
    }
    return out + "\"";
}

static std::string SongJson(Il2CppObject* song)
{
    return "{\"title\":" + JsonString(Utf8(CallString(g_queue.nameStripped, song))) +
           ",\"artist\":" + JsonString(Utf8(CallString(g_queue.artistStripped, song))) +
           ",\"checksum\":" + JsonString(Utf8(CallString(g_queue.checksumString, song))) + "}";
}

static std::string SongLabel(Il2CppObject* song)
{
    if (!song)
        return "(null)";
    return Utf8(CallString(g_queue.artistStripped, song)) + " - " + Utf8(CallString(g_queue.nameStripped, song)) +
           " [" + Utf8(CallString(g_queue.checksumString, song)) + "]";
}

static Il2CppObject* GlobalVars()
{
    return g_queue.gvInstance ? StaticObject(g_queue.gvInstance) : nullptr;
}

template <typename T>
static T GetField(Il2CppObject* obj, FieldInfo* f)
{
    T v{};
    il2cpp_field_get_value(obj, f, &v);
    return v;
}

// --- Diagnostics -------------------------------------------------------------------------------

// Logs the sizes of the library's song lists and the full setlist triple. Answers the open
// questions of ANALYSIS.md section 7 (which list holds all songs, what the strings contain).
static void LogLibraryState(const char* reason)
{
    QueueRefs& q = g_queue;
    Log("library state (%s):", reason);
    for (FieldInfo* f : q.songLists)
        Log("  List<SongEntry> %s (static off 0x%zx): %d songs", il2cpp_field_get_name(f), il2cpp_field_get_offset(f),
            ListCount(StaticObject(f)));
    if (Il2CppObject* gv = GlobalVars())
        Log("  GlobalVariables: isPlayingSetlist=%d setlistIndex=%d originalSongSpeed=%d isPracticeEnabled=%d",
            GetField<bool>(gv, q.gvPlayingSetlist), GetField<int>(gv, q.gvSetlistIndex),
            GetField<int>(gv, q.gvOriginalSpeed), GetField<bool>(gv, q.gvPractice));
    Il2CppObject* songs = StaticObject(q.setSongs);
    Il2CppObject* speeds = StaticObject(q.setSpeeds);
    Il2CppObject* strings = StaticObject(q.setStrings);
    int ns = ListCount(songs), nv = ListCount(speeds), nt = ListCount(strings);
    Log("  setlist: %d songs, %d speeds, %d strings", ns, nv, nt);
    for (int i = 0; i < ns; ++i)
    {
        std::string str = "(missing)";
        if (i < nt)
        {
            Il2CppString* s = ListItem<Il2CppString*>(strings, i);
            str = s ? "\"" + Utf8(WideString(s)) + "\" (len " + std::to_string(WideString(s).size()) + ")" : "null";
        }
        Log("    [%d] %s speed=%d string=%s", i, SongLabel(ListItem<Il2CppObject*>(songs, i)).c_str(),
            i < nv ? ListItem<int>(speeds, i) : -1, str.c_str());
    }
}

// --- Requests ----------------------------------------------------------------------------------

static void Reply(JsonRequest* req, int status, const std::string& body)
{
    req->httpStatus = status;
    req->body = body;
}

static void ReplyResult(JsonRequest* req, int status, const char* result, const char* message)
{
    Reply(req, status, std::string("{\"result\":\"") + result + "\",\"message\":" + JsonString(message) + "}");
}

// The largest static List<SongEntry> of the library holds all songs.
static Il2CppObject* AllSongs()
{
    Il2CppObject* all = nullptr;
    for (FieldInfo* f : g_queue.songLists)
    {
        Il2CppObject* list = StaticObject(f);
        if (ListCount(list) > ListCount(all))
            all = list;
    }
    return all;
}

// --- Setlist across rescans ----------------------------------------------------------------------
//
// The scan clears the setlist lists (and the setlist panel) as soon as it starts; measured on
// v1.1.0.6142 with POST /rescan: 2 songs before, 0 within 25 ms, still 0 after the scan. Rescans
// started by Encore (F5, /rescan, so also the folder watcher and the companion's downloads) save
// the setlist before StartScan and add the songs back, found again by checksum, once isScanning
// is false. The restore runs from a timer and before every /status, /queue and /setlist request,
// so a song queued right after a rescan lands behind the restored ones.

struct SavedSetlistEntry
{
    std::wstring checksum;
    int speed;
};
static std::vector<SavedSetlistEntry> g_savedSetlist;
static bool g_restorePending = false;
static const UINT_PTR kRestoreTimer = 0x4C4A;

static void SaveSetlistForRescan()
{
    QueueRefs& q = g_queue;
    if (!q.ok || g_restorePending)
        return;
    Il2CppObject* songs = StaticObject(q.setSongs);
    Il2CppObject* speeds = StaticObject(q.setSpeeds);
    int n = ListCount(songs), nv = ListCount(speeds);
    g_savedSetlist.clear();
    for (int i = 0; i < n; ++i)
        if (Il2CppObject* song = ListItem<Il2CppObject*>(songs, i))
            g_savedSetlist.push_back({CallString(q.checksumString, song), i < nv ? ListItem<int>(speeds, i) : 100});
    if (g_savedSetlist.empty())
        return;
    g_restorePending = true;
    SetTimer(g_unityWnd, kRestoreTimer, 250, nullptr);
    Log("rescan: saved %zu setlist songs to restore after the scan", g_savedSetlist.size());
}

static void RestoreSetlistIfScanDone()
{
    QueueRefs& q = g_queue;
    if (!g_restorePending)
        return;
    if (Il2CppObject* songScan = FindActiveInstance(g_refs.songScan))
        if (GetField<bool>(songScan, g_refs.isScanning))
            return;
    g_restorePending = false;
    KillTimer(g_unityWnd, kRestoreTimer);

    // One pass over the library: the new SongEntry objects for the saved checksums.
    std::vector<Il2CppObject*> found(g_savedSetlist.size(), nullptr);
    Il2CppObject* all = AllSongs();
    int n = ListCount(all);
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject* song = ListItem<Il2CppObject*>(all, i);
        if (!song)
            continue;
        std::wstring checksum = CallString(q.checksumString, song);
        for (size_t k = 0; k < g_savedSetlist.size(); ++k)
            if (!found[k] && SameText(g_savedSetlist[k].checksum, checksum))
                found[k] = song;
    }

    Il2CppObject* songs = StaticObject(q.setSongs);
    Il2CppObject* speeds = StaticObject(q.setSpeeds);
    Il2CppObject* strings = StaticObject(q.setStrings);
    if (!songs || !speeds || !strings)
        return;
    int before = ListCount(songs);
    Il2CppObject* songSelect = FindActiveInstance(g_refs.songSelect);
    Il2CppObject* menu = songSelect && q.setlistAdd ? GetField<Il2CppObject*>(songSelect, q.ssSetlistMenu) : nullptr;
    int restored = 0, missing = 0;
    for (size_t k = 0; k < g_savedSetlist.size(); ++k)
    {
        if (!found[k])
        {
            ++missing;
            Log("rescan: setlist song %s is no longer in the library, not restored", Utf8(g_savedSetlist[k].checksum).c_str());
            continue;
        }
        int speed = g_savedSetlist[k].speed;
        Il2CppObject* str = reinterpret_cast<Il2CppObject*>(il2cpp_string_new(""));
        if (menu)
        {
            // Like the "add to setlist" button, so the setlist panel shows the song again.
            void* args[] = {found[k], &speed, str};
            Invoke(q.setlistAdd, menu, args);
        }
        else if (!ListAdd(speeds, &speed) || !ListAdd(strings, str) || !ListAdd(songs, found[k]))
        {
            Log("rescan: List.Add failed while restoring the setlist");
            break;
        }
        ++restored;
    }
    Log("rescan: restored %d of %zu setlist songs (%d missing, %d were already in the setlist)%s", restored,
        g_savedSetlist.size(), missing, before, menu ? "" : " without the setlist panel");
    g_savedSetlist.clear();
}

// GET /setlist
static void HandleSetlist(JsonRequest* req)
{
    RestoreSetlistIfScanDone();
    QueueRefs& q = g_queue;
    if (!g_initDone || !q.ok)
        return ReplyResult(req, 503, "not_ready", "Encore is not ready, see encore.log");
    Il2CppObject* gv = GlobalVars();
    Il2CppObject* songs = StaticObject(q.setSongs);
    Il2CppObject* speeds = StaticObject(q.setSpeeds);
    int n = ListCount(songs), nv = ListCount(speeds);
    // Clients poll this; log the state only when the setlist or its position changed.
    static std::string lastLogged;
    std::string state = std::to_string(n) + "/" + std::to_string(ListCount(AllSongs())) + "/" +
                        std::to_string(gv ? GetField<int>(gv, q.gvSetlistIndex) : -1) + "/" +
                        std::to_string(gv && GetField<bool>(gv, q.gvPlayingSetlist));
    if (state != lastLogged)
    {
        lastLogged = state;
        LogLibraryState("GET /setlist");
    }
    std::string body = std::string("{\"playing\":") + (gv && GetField<bool>(gv, q.gvPlayingSetlist) ? "true" : "false") +
                       ",\"index\":" + std::to_string(gv ? GetField<int>(gv, q.gvSetlistIndex) : -1) + ",\"songs\":[";
    for (int i = 0; i < n; ++i)
    {
        std::string song = SongJson(ListItem<Il2CppObject*>(songs, i));
        song.insert(song.size() - 1, ",\"speed\":" + std::to_string(i < nv ? ListItem<int>(speeds, i) : -1));
        body += (i ? "," : "") + song;
    }
    Reply(req, 200, body + "]}");
}

// Looks up a song in the list of all songs.
static Il2CppObject* FindSong(const JsonRequest* req, int& matches)
{
    Il2CppObject* all = AllSongs();
    matches = 0;
    Il2CppObject* found = nullptr;
    std::wstring checksum = Wide(req->checksum), title = Wide(req->title), artist = Wide(req->artist);
    int n = ListCount(all);
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject* song = ListItem<Il2CppObject*>(all, i);
        if (!song)
            continue;
        bool hit;
        if (!checksum.empty())
            hit = SameText(CallString(g_queue.checksumString, song), checksum);
        else
            hit = SameText(CallString(g_queue.nameStripped, song), title) &&
                  (artist.empty() || SameText(CallString(g_queue.artistStripped, song), artist));
        if (hit)
        {
            if (!found)
                found = song;
            ++matches;
        }
    }
    return found;
}

// --- Library search (GET /library) -------------------------------------------------------------
//
// Reading a song's strings means several managed calls, so the list of all songs is copied once
// into native strings and searched from there. The copy is rebuilt when the list changes (another
// list object, a different items array, size or List<T>._version, e.g. after a rescan).

struct LibrarySong
{
    std::string title, artist, album, checksum; // UTF-8, without formatting tags
    std::wstring titleKey, artistKey;           // lower case, for matching
    int lengthMs = -1;
};

struct LibraryCache
{
    Il2CppObject* list = nullptr;
    void* items = nullptr;
    int count = -1, version = -1;
    std::vector<LibrarySong> songs;
};
static LibraryCache g_library;

static std::wstring Lower(std::wstring s)
{
    if (!s.empty())
        CharLowerBuffW(&s[0], static_cast<DWORD>(s.size()));
    return s;
}

static void RefreshLibraryCache()
{
    LibraryCache& c = g_library;
    Il2CppObject* all = AllSongs();
    void* items = all ? *reinterpret_cast<void**>(reinterpret_cast<char*>(all) + 0x10) : nullptr;
    int count = ListCount(all);
    int version = all ? *reinterpret_cast<int*>(reinterpret_cast<char*>(all) + 0x1c) : -1; // List<T>._version
    if (all == c.list && items == c.items && count == c.count && version == c.version)
        return;
    c.list = all;
    c.items = items;
    c.count = count;
    c.version = version;
    c.songs.clear();
    c.songs.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        Il2CppObject* song = ListItem<Il2CppObject*>(all, i);
        if (!song)
            continue;
        LibrarySong s;
        std::wstring title = CallString(g_queue.nameStripped, song), artist = CallString(g_queue.artistStripped, song);
        s.title = Utf8(title);
        s.artist = Utf8(artist);
        s.titleKey = Lower(title);
        s.artistKey = Lower(artist);
        if (g_queue.albumStripped)
            s.album = Utf8(CallString(g_queue.albumStripped, song));
        s.checksum = Utf8(CallString(g_queue.checksumString, song));
        if (g_queue.songLength)
            s.lengthMs = GetField<int>(song, g_queue.songLength);
        c.songs.push_back(std::move(s));
    }
    Log("library: indexed %zu songs for search", c.songs.size());
}

// Splits the search text into lower-case words.
static std::vector<std::wstring> SearchWords(const std::wstring& text)
{
    std::vector<std::wstring> words;
    std::wstring lower = Lower(text), word;
    for (wchar_t ch : lower + L" ")
    {
        if (iswspace(ch))
        {
            if (!word.empty())
                words.push_back(word);
            word.clear();
        }
        else
            word += ch;
    }
    return words;
}

// GET /library: songs whose title or artist contain every word of the search text. Exact and
// prefix title matches come first, then artist matches, then the rest; each group by artist and
// title. An empty search lists the library from the start.
static void HandleLibrary(JsonRequest* req)
{
    if (!g_initDone || !g_queue.ok)
        return ReplyResult(req, 503, "not_ready", "Encore is not ready, see encore.log");
    RefreshLibraryCache();
    const std::vector<LibrarySong>& songs = g_library.songs;
    std::wstring query = Lower(Wide(req->query));
    while (!query.empty() && iswspace(query.back()))
        query.pop_back();
    while (!query.empty() && iswspace(query.front()))
        query.erase(0, 1);
    std::vector<std::wstring> words = SearchWords(query);

    struct Hit
    {
        int rank;
        const LibrarySong* song;
    };
    std::vector<Hit> hits;
    for (const LibrarySong& s : songs)
    {
        bool all = true;
        for (const std::wstring& w : words)
            if (s.titleKey.find(w) == std::wstring::npos && s.artistKey.find(w) == std::wstring::npos)
            {
                all = false;
                break;
            }
        if (!all)
            continue;
        int rank = 4;
        if (query.empty())
            rank = 0;
        else if (s.titleKey == query)
            rank = 0;
        else if (s.titleKey.compare(0, query.size(), query) == 0)
            rank = 1;
        else if (s.artistKey.compare(0, query.size(), query) == 0)
            rank = 2;
        else if (s.titleKey.find(query) != std::wstring::npos)
            rank = 3;
        hits.push_back({rank, &s});
    }
    size_t shown = (std::min)(hits.size(), static_cast<size_t>(req->limit));
    std::partial_sort(hits.begin(), hits.begin() + shown, hits.end(), [](const Hit& a, const Hit& b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.song->artistKey != b.song->artistKey)
            return a.song->artistKey < b.song->artistKey;
        return a.song->titleKey < b.song->titleKey;
    });
    std::string body = "{\"query\":" + JsonString(req->query) + ",\"total\":" + std::to_string(hits.size()) +
                       ",\"library\":" + std::to_string(songs.size()) + ",\"songs\":[";
    for (size_t i = 0; i < shown; ++i)
    {
        const LibrarySong& s = *hits[i].song;
        body += std::string(i ? "," : "") + "{\"title\":" + JsonString(s.title) + ",\"artist\":" +
                JsonString(s.artist) + ",\"album\":" + JsonString(s.album) + ",\"checksum\":" +
                JsonString(s.checksum) + ",\"length\":" +
                (s.lengthMs > 0 ? std::to_string((s.lengthMs + 500) / 1000) : std::string("null")) + "}";
    }
    Reply(req, 200, body + "]}");
}

static bool GameObjectActive(Il2CppObject* go)
{
    if (!go || !g_queue.activeSelf)
        return false;
    Il2CppObject* boxed = Invoke(g_queue.activeSelf, go, nullptr);
    return boxed && *static_cast<bool*>(il2cpp_object_unbox(boxed));
}

// If the end-of-song screen already decided that the setlist is over, but songs were queued
// since, reverts that decision so "continue" plays the next song.
static void ReopenEndOfSongSetlist(int setlistIndex, int count)
{
    QueueRefs& q = g_queue;
    if (!q.eosIsLastSong || !q.eosSetlistPanel || !q.eosSetlistEndPanel || setlistIndex >= count - 1)
        return;
    Il2CppObject* eos = FindActiveInstance(q.endOfSong);
    if (!eos)
        return;
    Il2CppObject* endPanel = GetField<Il2CppObject*>(eos, q.eosSetlistEndPanel);
    Il2CppObject* nextPanel = GetField<Il2CppObject*>(eos, q.eosSetlistPanel);
    bool isLast = GetField<bool>(eos, q.eosIsLastSong);
    if (!isLast || !GameObjectActive(endPanel))
    {
        Log("queue: end screen open, isLastSong=%d setlist_end active=%d, nothing to undo", isLast,
            GameObjectActive(endPanel));
        return;
    }
    bool off = false, on = true;
    il2cpp_field_set_value(eos, q.eosIsLastSong, &off);
    void* offArgs[] = {&off};
    void* onArgs[] = {&on};
    Invoke(g_refs.setActive, endPanel, offArgs);
    if (nextPanel)
        Invoke(g_refs.setActive, nextPanel, onArgs);
    Log("queue: end screen had ended the setlist; switched it back to \"continue\"");
}

// POST /queue
static void HandleQueue(JsonRequest* req)
{
    QueueRefs& q = g_queue;
    if (!g_initDone || !g_refs.ok || !q.ok)
        return ReplyResult(req, 503, "not_ready", "Encore is not ready, see encore.log");
    RestoreSetlistIfScanDone();
    Il2CppObject* gv = GlobalVars();
    if (!gv)
        return ReplyResult(req, 503, "not_ready", "GlobalVariables not available yet");
    if (GetField<bool>(gv, q.gvPractice))
    {
        Log("queue refused: practice mode");
        return ReplyResult(req, 409, "practice", "practice mode has no setlist");
    }

    Il2CppObject* songSelect = FindActiveInstance(g_refs.songSelect);
    bool playingSetlist = GetField<bool>(gv, q.gvPlayingSetlist);
    bool inGame = FindActiveInstance(q.gameManager) || FindActiveInstance(q.endOfSong);
    Il2CppObject* songs = StaticObject(q.setSongs);
    Il2CppObject* speeds = StaticObject(q.setSpeeds);
    Il2CppObject* strings = StaticObject(q.setStrings);
    if (!songs || !speeds || !strings)
        return ReplyResult(req, 500, "failed", "setlist lists are null, see encore.log");
    int count = ListCount(songs);
    if (ListCount(speeds) != count || ListCount(strings) != count)
    {
        Log("queue refused: setlist lists out of sync (%d/%d/%d)", count, ListCount(speeds), ListCount(strings));
        return ReplyResult(req, 500, "failed", "setlist lists out of sync, see encore.log");
    }
    const char* mode;
    if (songSelect)
        mode = "song_list";
    else if (inGame && playingSetlist)
        mode = "setlist_running";
    else if (inGame)
    {
        Log("queue refused: quickplay (no setlist running)");
        return ReplyResult(req, 409, "quickplay",
                           "a single song is playing, not a setlist; queue from the song list or during a setlist");
    }
    else if (playingSetlist && count > 0)
    {
        // Between songs of a setlist no scene object is active yet.
        Log("queue refused: setlist scene is loading");
        return ReplyResult(req, 503, "loading", "the next setlist song is loading, try again in a moment");
    }
    else
    {
        Log("queue refused: not in the song list or a setlist");
        return ReplyResult(req, 409, "not_in_song_list", "open the song list (or play a setlist) and try again");
    }

    int matches = 0;
    Il2CppObject* song = FindSong(req, matches);
    if (!song)
    {
        Log("queue: no song for checksum=\"%s\" title=\"%s\" artist=\"%s\"", req->checksum.c_str(),
            req->title.c_str(), req->artist.c_str());
        return ReplyResult(req, 404, "not_found", "no song with that checksum/title in the library (rescan first?)");
    }

    // The game passes the song speed setting; the player's chosen speed is kept in
    // GlobalVariables.originalSongSpeed while a setlist overrides the setting per song.
    int speed = req->speed;
    if (speed <= 0)
        speed = GetField<int>(gv, q.gvOriginalSpeed);
    if (speed <= 0)
        speed = 100;
    // Reuse the string object of an existing entry so ours matches what the game stores; an empty
    // setlist gets an empty string (to be confirmed by the runtime check).
    Il2CppObject* str = count > 0 ? ListItem<Il2CppObject*>(strings, count - 1)
                                  : reinterpret_cast<Il2CppObject*>(il2cpp_string_new(""));

    if (songSelect && q.setlistAdd)
    {
        Il2CppObject* menu = GetField<Il2CppObject*>(songSelect, q.ssSetlistMenu);
        if (!menu)
            return ReplyResult(req, 500, "failed", "SongSelect.setlistMenu is null, see encore.log");
        void* args[] = {song, &speed, str};
        Invoke(q.setlistAdd, menu, args);
        mode = "song_list";
    }
    else
    {
        // In a running setlist SetlistMenu doesn't exist; append to the three lists, songs last
        // so a reader never sees a song without its speed and string.
        if (!ListAdd(speeds, &speed) || !ListAdd(strings, str) || !ListAdd(songs, song))
        {
            Log("queue: List.Add failed, lists now %d/%d/%d", ListCount(songs), ListCount(speeds), ListCount(strings));
            return ReplyResult(req, 500, "failed", "appending to the setlist failed, see encore.log");
        }
        if (songSelect)
            mode = "song_list_lists"; // SetlistMenu.Add not identified: panel not refreshed
    }
    int newCount = ListCount(songs);
    if (newCount != count + 1 || ListCount(speeds) != newCount || ListCount(strings) != newCount)
    {
        Log("queue: unexpected setlist sizes after adding: %d/%d/%d (was %d)", newCount, ListCount(speeds),
            ListCount(strings), count);
        return ReplyResult(req, 500, "failed", "setlist did not grow as expected, see encore.log");
    }
    int setlistIndex = GetField<int>(gv, q.gvSetlistIndex);
    if (strcmp(mode, "setlist_running") == 0)
        ReopenEndOfSongSetlist(setlistIndex, newCount);
    Log("queued (%s, %d matches): %s speed=%d -> setlist position %d of %d", mode, matches, SongLabel(song).c_str(),
        speed, newCount, newCount);
    LogLibraryState("after queue");
    Reply(req, 200,
          std::string("{\"result\":\"queued\",\"mode\":\"") + mode + "\",\"song\":" + SongJson(song) +
              ",\"speed\":" + std::to_string(speed) + ",\"position\":" + std::to_string(newCount) +
              ",\"count\":" + std::to_string(newCount) + ",\"matches\":" + std::to_string(matches) + "}");
}

// ---------------------------------------------------------------------------------------------
// "Encore" entry in the main menu (docs/ANALYSIS.md section 8).
//
//   - BaseMenu draws menuStrings into textObjects/backgroundObjects. Its visible count is
//     min(textObjects.Length, menuStrings.Length) and the rest scrolls; both ints are obfuscated
//     fields, the 3rd and 4th Int32 field of BaseMenu (max scroll, visible count). Encore checks
//     their values against that formula on the live menu before it changes anything.
//   - Confirming an entry calls a virtual BaseMenu method that MainMenu overrides. The override
//     reads the selected menu string (BaseMenu's only `string X()` method) and switches on it;
//     an unknown string does nothing. It is the only MainMenu method that calls that getter and
//     it is only reached through MainMenu's vtable (no direct callers), so Encore replaces the
//     method pointer in that vtable slot with a wrapper (a data change, no code patching): the
//     wrapper handles "Encore" and passes everything else to the original.
//   - The dialog is MainMenu.confirmMenu (ConfirmationMenu). Its "show" method takes
//     (title, option 1, option 2, on option 1, on option 2, body); title and options go through
//     the game's text lookup, which returns the key itself for unknown keys. ConfirmationMenu has
//     several copies of it (obfuscator); the real one is the only one in its vtable.
//   - The text lookup also handles the menu label: "Encore" is not a key, so it is shown as is.
// ---------------------------------------------------------------------------------------------

static const UINT WM_ENCORE_COMPANION = WM_APP + 0x4C4A; // lParam = CompanionResult*
static const UINT_PTR kMenuTimer = 0x4C4B;
static const wchar_t kEntryLabel[] = L"Encore";
static const char kEntryLabelUtf8[] = "Encore";
static const char kEntryTip[] = "Start the phone companion and pair a phone";

using ConfirmFn = void (*)(Il2CppObject*, const MethodInfo*);
static unsigned short TriggerPort();

struct MenuRefs
{
    Il2CppClass* mainMenu = nullptr;
    Il2CppClass* baseMenu = nullptr;
    Il2CppClass* confirmClass = nullptr;
    FieldInfo* menuStrings = nullptr;
    FieldInfo* tipStrings = nullptr;
    FieldInfo* textObjects = nullptr;
    FieldInfo* backgrounds = nullptr;
    FieldInfo* useUppercase = nullptr;
    FieldInfo* isActive = nullptr;
    FieldInfo* maxScroll = nullptr;    // 3rd Int32 field of BaseMenu
    FieldInfo* visibleCount = nullptr; // 4th Int32 field of BaseMenu
    FieldInfo* confirmMenu = nullptr;  // MainMenu
    std::vector<FieldInfo*> dialogs;   // MainMenu's ConfirmationMenu fields
    FieldInfo* dialogTitle = nullptr;  // ConfirmationMenu.messageTitleText
    FieldInfo* dialogBody = nullptr;   // ConfirmationMenu.messageBodyText
    FieldInfo* mmSongScan = nullptr;
    FieldInfo* subMenus[3] = {};       // MainMenu songSelect, settingsMenu, onlineMenu (GameObjects)
    const MethodInfo* selectedString = nullptr;
    const MethodInfo* confirm = nullptr;     // MainMenu override
    const MethodInfo* baseConfirm = nullptr; // BaseMenu method it overrides
    std::vector<const MethodInfo*> showCandidates;
    const MethodInfo* showDialog = nullptr;
    // Unity
    const MethodInfo* instantiate = nullptr; // Object.Instantiate(Object, Transform, bool), or
    const MethodInfo* instantiate1 = nullptr; // Object.Instantiate(Object) + Transform.SetParent
    const MethodInfo* setParent = nullptr;    // Transform.SetParent(Transform, bool)
    const MethodInfo* getTransform = nullptr;
    const MethodInfo* getParent = nullptr;
    const MethodInfo* getLocalPos = nullptr;
    const MethodInfo* setLocalPos = nullptr;
    const MethodInfo* inChildren = nullptr;  // Component.GetComponentInChildren(Type)
    const MethodInfo* setText = nullptr;     // TMP_Text.set_text
    const MethodInfo* activeSelf = nullptr;
    bool resolved = false; // static part done
    bool hooked = false;
    bool disabled = false; // detection failed at runtime: leave the menu alone
};
static MenuRefs g_menu;
static ConfirmFn g_origConfirm = nullptr;
static Il2CppObject* g_menuRefused = nullptr; // last MainMenu instance the entry was refused for

static int ArrayLength(Il2CppObject* a)
{
    return a ? static_cast<int>(il2cpp_array_length(a)) : 0;
}

static Il2CppObject*& ArrayItem(Il2CppObject* a, int i)
{
    return reinterpret_cast<Il2CppObject**>(reinterpret_cast<char*>(a) + 0x20)[i];
}

// A copy of a reference-type array with one more element.
static Il2CppObject* ArrayAppend(Il2CppObject* arr, Il2CppObject* extra)
{
    Il2CppClass* elem = il2cpp_class_get_element_class(il2cpp_object_get_class(arr));
    int n = ArrayLength(arr);
    Il2CppObject* out = elem ? il2cpp_array_new(elem, n + 1) : nullptr;
    if (!out)
        return nullptr;
    for (int i = 0; i < n; ++i)
        il2cpp_gc_wbarrier_set_field(out, reinterpret_cast<void**>(&ArrayItem(out, i)), ArrayItem(arr, i));
    il2cpp_gc_wbarrier_set_field(out, reinterpret_cast<void**>(&ArrayItem(out, n)), extra);
    return out;
}

// The function a RUNTIME_FUNCTION belongs to: MSVC splits large functions into chunks whose unwind
// info is chained (UNW_FLAG_CHAININFO) to the primary entry.
static const RUNTIME_FUNCTION* PrimaryEntry(DWORD64 imageBase, const RUNTIME_FUNCTION* rf)
{
    for (int depth = 0; rf && depth < 32; ++depth)
    {
        auto info = reinterpret_cast<const uint8_t*>(imageBase + rf->UnwindData);
        if (!((info[0] >> 3) & UNW_FLAG_CHAININFO))
            return rf;
        uint8_t codes = info[2];
        rf = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
    }
    return nullptr;
}

// The machine code of the function starting at fn. Its extent comes from the x64 unwind tables:
// the primary entry plus the chained chunks right after it.
static bool FunctionCode(void* fn, const uint8_t*& code, size_t& size)
{
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(fn), &imageBase, nullptr);
    if (!rf || imageBase + rf->BeginAddress != reinterpret_cast<DWORD64>(fn))
        return false;
    DWORD begin = rf->BeginAddress, end = rf->EndAddress;
    for (int chunks = 0; chunks < 64; ++chunks)
    {
        DWORD64 nextBase = 0;
        PRUNTIME_FUNCTION next = RtlLookupFunctionEntry(imageBase + end, &nextBase, nullptr);
        if (!next || next->BeginAddress != end || PrimaryEntry(imageBase, next) == nullptr ||
            PrimaryEntry(imageBase, next)->BeginAddress != begin)
            break;
        end = next->EndAddress;
    }
    code = reinterpret_cast<const uint8_t*>(imageBase + begin);
    size = end - begin;
    return true;
}

// True if the function starting at fn contains a call/jmp rel32 to target.
static bool CallsTarget(void* fn, void* target)
{
    const uint8_t* code;
    size_t size;
    if (!FunctionCode(fn, code, size))
        return false;
    for (size_t i = 0; i + 5 <= size; ++i)
    {
        if (code[i] != 0xE8 && code[i] != 0xE9)
            continue;
        int32_t rel;
        memcpy(&rel, code + i + 1, sizeof(rel));
        if (code + i + 5 + rel == target)
            return true;
    }
    return false;
}

// Counts virtual calls to a 6-parameter method through the given vtable offset in the methods of k
// and its nested types. IL2CPP calls virtual methods as klass->vtable[slot].methodPtr and passes
// vtable[slot].method as the last argument; with `this` plus 6 parameters that is the 8th argument,
// on the stack at [rsp+0x38]. The pattern is `mov r64, [reg+offset+8]` directly followed by
// `mov [rsp+0x38], r64`. Matching the argument position keeps calls of other classes' methods that
// happen to sit at the same vtable offset (e.g. TMP_Text.set_text) out of the count.
static int CountVirtualCalls(Il2CppClass* k, int32_t offset)
{
    std::vector<uintptr_t> bodies;
    CollectMethodBodies(k, bodies);
    std::sort(bodies.begin(), bodies.end());
    bodies.erase(std::unique(bodies.begin(), bodies.end()), bodies.end());
    int count = 0;
    for (uintptr_t fn : bodies)
    {
        const uint8_t* code;
        size_t size;
        if (!FunctionCode(reinterpret_cast<void*>(fn), code, size))
            continue;
        for (size_t i = 0; i + 12 <= size; ++i)
        {
            // mov r64, [base+disp32]: REX.W 8B modrm(mod=10, rm!=100)
            const uint8_t* p = code + i;
            if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8B || (p[2] >> 6) != 2 || (p[2] & 7) == 4)
                continue;
            int32_t disp;
            memcpy(&disp, p + 3, sizeof(disp));
            if (disp != offset + 8)
                continue;
            int reg = ((p[0] >> 2) & 1) << 3 | ((p[2] >> 3) & 7);
            // mov [rsp+0x38], r64: REX.W 89 modrm(mod=01, rm=100) SIB 24 disp8 38
            const uint8_t* q = p + 7;
            if ((q[0] & 0xF8) == 0x48 && q[1] == 0x89 && (q[2] & 0xC7) == 0x44 && q[3] == 0x24 && q[4] == 0x38 &&
                (((q[0] >> 2) & 1) << 3 | ((q[2] >> 3) & 7)) == reg)
                ++count;
        }
    }
    return count;
}

// The vtable entry {methodPtr, MethodInfo*} of m in class k (IL2CPP keeps the vtable inline at the
// end of Il2CppClass). Returns the address of methodPtr, or null unless there is exactly one.
static void** FindVtableSlot(Il2CppClass* k, const MethodInfo* m)
{
    void* ptr = il2cpp_method_pointer(m);
    MEMORY_BASIC_INFORMATION mbi;
    if (!ptr || !VirtualQuery(k, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
        return nullptr;
    uintptr_t end = (std::min)(reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize,
                               reinterpret_cast<uintptr_t>(k) + 0x2000);
    void** found = nullptr;
    int matches = 0;
    for (auto p = reinterpret_cast<void**>(k); reinterpret_cast<uintptr_t>(p + 2) <= end; ++p)
        if (p[0] == ptr && p[1] == m)
        {
            found = p;
            ++matches;
        }
    return matches == 1 ? found : nullptr;
}

static bool IsInstance(const MethodInfo* m)
{
    uint32_t iflags = 0;
    return !(il2cpp_method_get_flags(m, &iflags) & kAttrStatic);
}

static const MethodInfo* FindMethod(Il2CppClass* k, const char* name, std::initializer_list<const char*> params,
                                    const char* ret = nullptr)
{
    const MethodInfo* found = nullptr;
    void* it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(k, &it))
    {
        if (strcmp(il2cpp_method_get_name(m), name) != 0 || il2cpp_method_get_param_count(m) != params.size())
            continue;
        if (ret && TypeName(il2cpp_method_get_return_type(m)) != ret)
            continue;
        uint32_t i = 0;
        bool match = true;
        for (const char* p : params)
            match = match && TypeName(il2cpp_method_get_param(m, i++)) == p;
        if (match)
            found = m;
    }
    return found;
}

// Everything that can be resolved without a live menu. The vtable checks need initialised classes
// and run when the first MainMenu instance shows up (InstallMenuHook).
static bool ResolveMenuRefs(const Il2CppImage* game)
{
    MenuRefs& r = g_menu;
    r.mainMenu = il2cpp_class_from_name(game, "", "MainMenu");
    r.baseMenu = il2cpp_class_from_name(game, "", "BaseMenu");
    r.confirmClass = il2cpp_class_from_name(game, "", "ConfirmationMenu");
    if (!r.mainMenu || !r.baseMenu || !r.confirmClass || il2cpp_class_get_parent(r.mainMenu) != r.baseMenu ||
        il2cpp_class_get_parent(r.confirmClass) != r.baseMenu)
    {
        Log("menu: MainMenu/BaseMenu/ConfirmationMenu not found or hierarchy changed, no Encore menu entry");
        return false;
    }
    r.menuStrings = il2cpp_class_get_field_from_name(r.baseMenu, "menuStrings");
    r.tipStrings = il2cpp_class_get_field_from_name(r.baseMenu, "tipStrings");
    r.textObjects = il2cpp_class_get_field_from_name(r.baseMenu, "textObjects");
    r.backgrounds = il2cpp_class_get_field_from_name(r.baseMenu, "backgroundObjects");
    r.useUppercase = il2cpp_class_get_field_from_name(r.baseMenu, "useUppercase");
    r.isActive = il2cpp_class_get_field_from_name(r.baseMenu, "isActive");
    r.confirmMenu = il2cpp_class_get_field_from_name(r.mainMenu, "confirmMenu");
    r.dialogTitle = il2cpp_class_get_field_from_name(r.confirmClass, "messageTitleText");
    r.dialogBody = il2cpp_class_get_field_from_name(r.confirmClass, "messageBodyText");
    {
        void* fit = nullptr;
        while (FieldInfo* f = il2cpp_class_get_fields(r.mainMenu, &fit))
            if (!IsStatic(f) && TypeName(il2cpp_field_get_type(f)) == "ConfirmationMenu")
                r.dialogs.push_back(f);
    }
    r.mmSongScan = il2cpp_class_get_field_from_name(r.mainMenu, "songScan");
    r.subMenus[0] = il2cpp_class_get_field_from_name(r.mainMenu, "songSelect");
    r.subMenus[1] = il2cpp_class_get_field_from_name(r.mainMenu, "settingsMenu");
    r.subMenus[2] = il2cpp_class_get_field_from_name(r.mainMenu, "onlineMenu");

    // BaseMenu's first four Int32 fields: scroll offset, selection, max scroll, visible count.
    std::vector<FieldInfo*> ints;
    void* it = nullptr;
    while (FieldInfo* f = il2cpp_class_get_fields(r.baseMenu, &it))
        if (!IsStatic(f) && TypeName(il2cpp_field_get_type(f)) == "System.Int32")
            ints.push_back(f);
    if (ints.size() >= 4 && il2cpp_field_get_offset(ints[3]) == il2cpp_field_get_offset(ints[2]) + 4)
    {
        r.maxScroll = ints[2];
        r.visibleCount = ints[3];
    }

    // The selected-string getter: BaseMenu's only instance method `string X()`.
    int getters = 0;
    it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(r.baseMenu, &it))
        if (IsInstance(m) && il2cpp_method_get_param_count(m) == 0 &&
            TypeName(il2cpp_method_get_return_type(m)) == "System.String")
        {
            r.selectedString = m;
            ++getters;
        }
    if (getters != 1)
        r.selectedString = nullptr;

    // The confirm override: a MainMenu `void X()` that calls the getter and overrides a BaseMenu
    // method of the same name. Uniqueness is checked against the vtable in InstallMenuHook.
    std::vector<const MethodInfo*> confirms;
    if (r.selectedString)
    {
        void* getter = il2cpp_method_pointer(r.selectedString);
        it = nullptr;
        while (const MethodInfo* m = il2cpp_class_get_methods(r.mainMenu, &it))
            if (IsInstance(m) && il2cpp_method_get_param_count(m) == 0 && il2cpp_method_pointer(m) &&
                TypeName(il2cpp_method_get_return_type(m)) == "System.Void" && CallsTarget(il2cpp_method_pointer(m), getter))
                confirms.push_back(m);
    }
    if (confirms.size() == 1)
    {
        r.confirm = confirms[0];
        r.baseConfirm = FindMethod(r.baseMenu, il2cpp_method_get_name(r.confirm), {}, "System.Void");
    }

    // ConfirmationMenu "show" candidates: (string, string, string, D, D, string) with D its delegate.
    it = nullptr;
    while (const MethodInfo* m = il2cpp_class_get_methods(r.confirmClass, &it))
    {
        if (!IsInstance(m) || il2cpp_method_get_param_count(m) != 6 || !il2cpp_method_pointer(m))
            continue;
        std::string d = TypeName(il2cpp_method_get_param(m, 3));
        if (TypeName(il2cpp_method_get_param(m, 0)) == "System.String" &&
            TypeName(il2cpp_method_get_param(m, 1)) == "System.String" &&
            TypeName(il2cpp_method_get_param(m, 2)) == "System.String" && d.rfind("ConfirmationMenu.", 0) == 0 &&
            TypeName(il2cpp_method_get_param(m, 4)) == d && TypeName(il2cpp_method_get_param(m, 5)) == "System.String")
            r.showCandidates.push_back(m);
    }

    const Il2CppImage* core = FindImage("UnityEngine.CoreModule.dll");
    const Il2CppImage* tmp = FindImage("Unity.TextMeshPro.dll");
    Il2CppClass* object = core ? il2cpp_class_from_name(core, "UnityEngine", "Object") : nullptr;
    Il2CppClass* component = core ? il2cpp_class_from_name(core, "UnityEngine", "Component") : nullptr;
    Il2CppClass* transform = core ? il2cpp_class_from_name(core, "UnityEngine", "Transform") : nullptr;
    Il2CppClass* gameObject = core ? il2cpp_class_from_name(core, "UnityEngine", "GameObject") : nullptr;
    Il2CppClass* tmpText = tmp ? il2cpp_class_from_name(tmp, "TMPro", "TMP_Text") : nullptr;
    if (object)
    {
        r.instantiate = FindMethod(object, "Instantiate", {"UnityEngine.Object", "UnityEngine.Transform", "System.Boolean"},
                                   "UnityEngine.Object");
        r.instantiate1 = FindMethod(object, "Instantiate", {"UnityEngine.Object"}, "UnityEngine.Object");
        it = nullptr;
        while (const MethodInfo* m = il2cpp_class_get_methods(object, &it))
            if (strcmp(il2cpp_method_get_name(m), "Instantiate") == 0 || strstr(il2cpp_method_get_name(m), "Clone"))
            {
                std::string sig;
                for (uint32_t i = 0; i < il2cpp_method_get_param_count(m); ++i)
                    sig += (i ? ", " : "") + TypeName(il2cpp_method_get_param(m, i));
                Log("  Object.%s(%s) -> %s", il2cpp_method_get_name(m), sig.c_str(),
                    TypeName(il2cpp_method_get_return_type(m)).c_str());
            }
    }
    if (component)
    {
        r.getTransform = FindMethod(component, "get_transform", {});
        r.inChildren = FindMethod(component, "GetComponentInChildren", {"System.Type"});
    }
    if (transform)
    {
        r.getParent = FindMethod(transform, "get_parent", {});
        r.getLocalPos = FindMethod(transform, "get_localPosition", {});
        r.setLocalPos = FindMethod(transform, "set_localPosition", {"UnityEngine.Vector3"});
        r.setParent = FindMethod(transform, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    }
    if (gameObject)
        r.activeSelf = FindMethod(gameObject, "get_activeSelf", {});
    if (tmpText)
        r.setText = FindMethod(tmpText, "set_text", {"System.String"});

    // Required pieces; cloning (Instantiate, GetComponentInChildren) is checked when it is needed.
    std::string missing;
    auto need = [&missing](const void* p, const char* name) {
        if (!p)
            missing += std::string(missing.empty() ? "" : ", ") + name;
    };
    need(r.menuStrings, "menuStrings");
    need(r.tipStrings, "tipStrings");
    need(r.textObjects, "textObjects");
    need(r.backgrounds, "backgroundObjects");
    need(r.useUppercase, "useUppercase");
    need(r.isActive, "isActive");
    need(r.maxScroll, "max scroll/visible count fields");
    need(r.confirmMenu, "MainMenu.confirmMenu");
    need(r.dialogTitle, "ConfirmationMenu.messageTitleText");
    need(r.dialogBody, "ConfirmationMenu.messageBodyText");
    need(r.mmSongScan, "MainMenu.songScan");
    need(r.subMenus[0], "MainMenu.songSelect");
    need(r.subMenus[1], "MainMenu.settingsMenu");
    need(r.subMenus[2], "MainMenu.onlineMenu");
    need(r.selectedString, "selected-string getter");
    need(r.confirm, "confirm method");
    need(r.baseConfirm, "BaseMenu method overridden by it");
    need(r.showCandidates.empty() ? nullptr : &r.showCandidates, "ConfirmationMenu show method");
    need(r.getTransform, "Component.get_transform");
    need(r.getParent, "Transform.get_parent");
    need(r.getLocalPos, "Transform.get_localPosition");
    need(r.setLocalPos, "Transform.set_localPosition");
    need(r.activeSelf, "GameObject.get_activeSelf");
    need(r.setText, "TMP_Text.set_text");
    need(g_refs.waitForFinish, "SongScan.waitForFinish");
    r.resolved = missing.empty();
    Log("menu resolve: Int32 fields=%zu (max scroll off 0x%zx, visible off 0x%zx) selected-string getters=%d "
        "confirm candidates=%zu (%s, RVA 0x%llx) show candidates=%zu Instantiate=%p/%p SetParent=%p "
        "GetComponentInChildren=%p -> %s%s",
        ints.size(), r.maxScroll ? il2cpp_field_get_offset(r.maxScroll) : 0,
        r.visibleCount ? il2cpp_field_get_offset(r.visibleCount) : 0, getters, confirms.size(),
        r.baseConfirm ? "overrides BaseMenu" : "no BaseMenu method of that name",
        static_cast<unsigned long long>(Rva(r.confirm ? il2cpp_method_pointer(r.confirm) : nullptr)), r.showCandidates.size(),
        r.instantiate, r.instantiate1, r.setParent, r.inChildren,
        r.resolved ? "ok" : "FAILED, no Encore menu entry; missing: ", missing.c_str());
    return r.resolved;
}

static void MenuEncoreSelected(Il2CppObject* menu);

static bool SelectedIsEncore(Il2CppObject* menu)
{
    Il2CppString* s = reinterpret_cast<Il2CppString*>(Invoke(g_menu.selectedString, menu, nullptr));
    return s && WideString(s) == kEntryLabel;
}

// Replaces MainMenu's confirm method in its vtable. Runs on the main thread from inside the game's
// Update, like the original.
static void HookedConfirm(Il2CppObject* self, const MethodInfo* method)
{
    if (!g_menu.disabled && SelectedIsEncore(self))
    {
        MenuEncoreSelected(self);
        return;
    }
    g_origConfirm(self, method);
}

// Needs initialised MainMenu and ConfirmationMenu classes, i.e. a live main menu.
static bool InstallMenuHook()
{
    MenuRefs& r = g_menu;
    void** slot = FindVtableSlot(r.mainMenu, r.confirm);
    void** baseSlot = FindVtableSlot(r.baseMenu, r.baseConfirm);
    ptrdiff_t off = slot ? reinterpret_cast<char*>(slot) - reinterpret_cast<char*>(r.mainMenu) : -1;
    ptrdiff_t baseOff = baseSlot ? reinterpret_cast<char*>(baseSlot) - reinterpret_cast<char*>(r.baseMenu) : -2;
    MEMORY_BASIC_INFORMATION mbi = {};
    bool writable = slot && VirtualQuery(slot, &mbi, sizeof(mbi)) &&
                    (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE));
    // All copies of the show method are virtual; the real one is the one MainMenu's code calls
    // through its vtable slot.
    int called = 0;
    for (const MethodInfo* m : r.showCandidates)
    {
        void** s = FindVtableSlot(r.confirmClass, m);
        int32_t slotOff = s ? static_cast<int32_t>(reinterpret_cast<char*>(s) - reinterpret_cast<char*>(r.confirmClass)) : -1;
        int calls = s ? CountVirtualCalls(r.mainMenu, slotOff) : 0;
        Log("  show candidate RVA 0x%llx: vtable +0x%x, %d virtual calls from MainMenu",
            static_cast<unsigned long long>(Rva(il2cpp_method_pointer(m))), slotOff, calls);
        if (calls > 0)
        {
            r.showDialog = m;
            ++called;
        }
    }
    Log("menu: confirm slot at class+0x%llx (BaseMenu +0x%llx) writable=%d, show candidates called by MainMenu=%d of %zu",
        static_cast<long long>(off), static_cast<long long>(baseOff), writable, called, r.showCandidates.size());
    if (!slot || off != baseOff || !writable || called != 1)
    {
        Log("menu: vtable check failed, no Encore menu entry");
        r.showDialog = nullptr;
        return false;
    }
    g_origConfirm = reinterpret_cast<ConfirmFn>(*slot);
    *slot = reinterpret_cast<void*>(&HookedConfirm);
    Log("menu: confirm hook installed (show dialog RVA 0x%llx)",
        static_cast<unsigned long long>(Rva(il2cpp_method_pointer(r.showDialog))));
    return true;
}

static Il2CppObject* TransformOf(Il2CppObject* component)
{
    return component ? Invoke(g_menu.getTransform, component, nullptr) : nullptr;
}

static bool LocalPosition(Il2CppObject* t, float out[3])
{
    Il2CppObject* boxed = t ? Invoke(g_menu.getLocalPos, t, nullptr) : nullptr;
    if (!boxed)
        return false;
    memcpy(out, il2cpp_object_unbox(boxed), 3 * sizeof(float));
    return true;
}

static bool IsAncestor(Il2CppObject* ancestor, Il2CppObject* t)
{
    for (int depth = 0; t && depth < 32; ++depth)
    {
        t = Invoke(g_menu.getParent, t, nullptr);
        if (t == ancestor)
            return true;
    }
    return false;
}

// Instantiates a copy of component (with its GameObject) next to the original and moves it one
// step further than the original is from prev.
static Il2CppObject* CloneAfter(Il2CppObject* component, Il2CppObject* prev)
{
    Il2CppObject* t = TransformOf(component);
    Il2CppObject* parent = t ? Invoke(g_menu.getParent, t, nullptr) : nullptr;
    float last[3], before[3];
    if (!parent || !LocalPosition(t, last) || !LocalPosition(TransformOf(prev), before))
        return nullptr;
    Il2CppObject* clone;
    if (g_menu.instantiate)
    {
        bool worldPositionStays = false;
        void* args[] = {component, parent, &worldPositionStays};
        clone = Invoke(g_menu.instantiate, nullptr, args);
    }
    else
    {
        void* args[] = {component};
        clone = Invoke(g_menu.instantiate1, nullptr, args);
        bool worldPositionStays = false;
        void* parentArgs[] = {parent, &worldPositionStays};
        if (Il2CppObject* t1 = TransformOf(clone))
            Invoke(g_menu.setParent, t1, parentArgs);
    }
    Il2CppObject* ct = TransformOf(clone);
    if (!ct)
        return nullptr;
    float pos[3] = {2 * last[0] - before[0], 2 * last[1] - before[1], 2 * last[2] - before[2]};
    void* posArgs[] = {pos};
    Invoke(g_menu.setLocalPos, ct, posArgs);
    return clone;
}

static Il2CppObject* ChildComponent(Il2CppObject* component, Il2CppObject* array)
{
    if (!g_menu.inChildren)
    {
        Log("menu: entry text and background are nested, but Component.GetComponentInChildren(Type) is not available");
        return nullptr;
    }
    Il2CppClass* elem = il2cpp_class_get_element_class(il2cpp_object_get_class(array));
    void* args[] = {il2cpp_type_get_object(il2cpp_class_get_type(elem))};
    return Invoke(g_menu.inChildren, component, args);
}

static bool HasEncoreEntry(Il2CppObject* strings)
{
    for (int i = 0; i < ArrayLength(strings); ++i)
        if (WideString(reinterpret_cast<Il2CppString*>(ArrayItem(strings, i))) == kEntryLabel)
            return true;
    return false;
}

// The cloned entry of the current main menu and the entry it was copied from. The menu slides its
// entries in whenever it opens; MenuTick keeps the copy one step after its source.
struct ClonedEntry
{
    Il2CppObject* menu = nullptr;
    Il2CppObject* srcText = nullptr;
    Il2CppObject* text = nullptr;
    Il2CppObject* srcBg = nullptr;
    Il2CppObject* bg = nullptr;
    Il2CppObject* srcTextBefore = nullptr; // the entries before the sources, for the step
    Il2CppObject* srcBgBefore = nullptr;
};
static ClonedEntry g_clone;

// The menu slides its entries in when it opens. The last entry is copied only after the last two
// entries' positions have stayed the same for a few ticks and are apart.
struct SettleWatch
{
    Il2CppObject* menu = nullptr;
    float last[3] = {}, prev[3] = {};
    int stableTicks = 0;
};
static SettleWatch g_settle;

static bool LocalPosition(Il2CppObject* t, float out[3]);
static Il2CppObject* TransformOf(Il2CppObject* component);

static bool EntriesSettled(Il2CppObject* menu, Il2CppObject* lastText, Il2CppObject* prevText)
{
    float last[3], prev[3];
    if (!LocalPosition(TransformOf(lastText), last) || !LocalPosition(TransformOf(prevText), prev))
        return false;
    if (g_settle.menu != menu || memcmp(last, g_settle.last, sizeof(last)) != 0 || memcmp(prev, g_settle.prev, sizeof(prev)) != 0)
    {
        g_settle.menu = menu;
        memcpy(g_settle.last, last, sizeof(last));
        memcpy(g_settle.prev, prev, sizeof(prev));
        g_settle.stableTicks = 0;
        return false;
    }
    float dx = last[0] - prev[0], dy = last[1] - prev[1];
    return ++g_settle.stableTicks >= 3 && dx * dx + dy * dy > 1.0f;
}

// Puts a copied element one step after the source element (same step as source minus the one
// before it), as when it was created.
static void PlaceAfter(Il2CppObject* clone, Il2CppObject* src, Il2CppObject* before)
{
    Il2CppObject* ct = TransformOf(clone);
    float last[3], prev[3], cur[3];
    if (!ct || !LocalPosition(TransformOf(src), last) || !LocalPosition(TransformOf(before), prev) || !LocalPosition(ct, cur))
        return;
    float pos[3] = {2 * last[0] - prev[0], 2 * last[1] - prev[1], 2 * last[2] - prev[2]};
    if (memcmp(pos, cur, sizeof(pos)) == 0)
        return;
    void* args[] = {pos};
    Invoke(g_menu.setLocalPos, ct, args);
}

// Keeps the copied entry with its source while the menu animates its entries (on every return to
// the main menu).
static void SyncClonedEntry(Il2CppObject* menu)
{
    if (menu != g_clone.menu)
        return;
    PlaceAfter(g_clone.text, g_clone.srcText, g_clone.srcTextBefore);
    if (g_clone.bg)
        PlaceAfter(g_clone.bg, g_clone.srcBg, g_clone.srcBgBefore);
}

// Appends "Encore" to a live main menu. Uses a spare text object if there is one, otherwise
// clones the last entry's text and background.
static void AddMenuEntry(Il2CppObject* menu)
{
    MenuRefs& r = g_menu;
    Il2CppObject* strings = GetField<Il2CppObject*>(menu, r.menuStrings);
    Il2CppObject* tips = GetField<Il2CppObject*>(menu, r.tipStrings);
    Il2CppObject* texts = GetField<Il2CppObject*>(menu, r.textObjects);
    Il2CppObject* bgs = GetField<Il2CppObject*>(menu, r.backgrounds);
    if (!strings || HasEncoreEntry(strings) || menu == g_menuRefused)
        return;
    int n = ArrayLength(strings), nt = ArrayLength(texts), nb = ArrayLength(bgs);
    int visible = GetField<int>(menu, r.visibleCount), maxScroll = GetField<int>(menu, r.maxScroll);
    int expectVisible = (std::min)(n, nt), expectMax = n > nt ? n - nt : 0;
    bool started = visible == expectVisible && maxScroll == expectMax;
    bool notStarted = visible == 0 && maxScroll == 0;
    static Il2CppObject* loggedFor = nullptr; // log the layout once per menu instance, not every tick
    if (loggedFor != menu)
        Log("menu: main menu has %d entries, %d text objects, %d backgrounds, %d tips; visible=%d max scroll=%d "
            "(expected %d/%d)", n, nt, nb, ArrayLength(tips), visible, maxScroll, expectVisible, expectMax);
    loggedFor = menu;
    if (n < 2 || nt < 2 || (!started && !notStarted) || (nb != 0 && nb != nt))
    {
        Log("menu: main menu layout not as analysed, entry not added");
        g_menuRefused = menu;
        return;
    }

    Il2CppObject* newTexts = nullptr;
    Il2CppObject* newBgs = nullptr;
    Il2CppObject* text = nullptr; // text object of the new entry
    if (nt > n)
        text = ArrayItem(texts, n); // spare text object, hidden while it has no string
    else
    {
        if (!r.instantiate && !(r.instantiate1 && r.setParent))
        {
            Log("menu: no spare text object and no usable Object.Instantiate, entry not added");
            g_menuRefused = menu;
            return;
        }
        int last = nt - 1;
        Il2CppObject* lt = ArrayItem(texts, last);
        Il2CppObject* lb = nb ? ArrayItem(bgs, last) : nullptr;
        if (!EntriesSettled(menu, lt, ArrayItem(texts, last - 1)))
            return; // still animating; try again on the next tick
        Il2CppObject* bg = nullptr;
        bool bgHoldsText = lb && IsAncestor(TransformOf(lb), TransformOf(lt));
        bool textHoldsBg = lb && !bgHoldsText && IsAncestor(TransformOf(lt), TransformOf(lb));
        Log("menu: cloning the last entry (background %s text)",
            bgHoldsText ? "contains the" : textHoldsBg ? "is inside the" : !lb ? "missing, only" : "separate from the");
        if ((bgHoldsText || textHoldsBg) && !r.inChildren)
        {
            Log("menu: entry text and background are nested, but Component.GetComponentInChildren(Type) is not "
                "available; entry not added");
            g_menuRefused = menu;
            return;
        }
        if (bgHoldsText)
        {
            bg = CloneAfter(lb, ArrayItem(bgs, last - 1));
            text = bg ? ChildComponent(bg, texts) : nullptr;
        }
        else if (textHoldsBg)
        {
            text = CloneAfter(lt, ArrayItem(texts, last - 1));
            bg = text ? ChildComponent(text, bgs) : nullptr;
        }
        else
        {
            // A clone becomes its parent's last child, and later children are drawn on top: clone
            // the background first so the label is drawn over it.
            bg = lb ? CloneAfter(lb, ArrayItem(bgs, last - 1)) : nullptr;
            text = CloneAfter(lt, ArrayItem(texts, last - 1));
        }
        if (!text || (lb && !bg))
        {
            Log("menu: cloning the last entry failed (text=%p background=%p), entry not added", text, bg);
            g_menuRefused = menu;
            return;
        }
        newTexts = ArrayAppend(texts, text);
        newBgs = lb ? ArrayAppend(bgs, bg) : nullptr;
        g_clone = {menu, lt, text, lb, bg, ArrayItem(texts, last - 1), lb ? ArrayItem(bgs, last - 1) : nullptr};
    }
    Il2CppObject* newStrings = ArrayAppend(strings, reinterpret_cast<Il2CppObject*>(il2cpp_string_new(kEntryLabelUtf8)));
    Il2CppObject* newTips =
        tips && ArrayLength(tips) == n ? ArrayAppend(tips, reinterpret_cast<Il2CppObject*>(il2cpp_string_new(kEntryTip))) : nullptr;
    if (!newStrings || (nt <= n && !newTexts) || (nt <= n && nb && !newBgs) || (tips && ArrayLength(tips) == n && !newTips))
    {
        Log("menu: allocating arrays failed, entry not added");
        g_menuRefused = menu;
        return;
    }
    if (newTexts)
        il2cpp_field_set_value(menu, r.textObjects, newTexts);
    if (newBgs)
        il2cpp_field_set_value(menu, r.backgrounds, newBgs);
    if (newTips)
        il2cpp_field_set_value(menu, r.tipStrings, newTips);
    il2cpp_field_set_value(menu, r.menuStrings, newStrings);
    if (started)
    {
        // Same formula as the game's own count method; before Start the game computes it itself.
        int total = n + 1, textCount = ArrayLength(GetField<Il2CppObject*>(menu, r.textObjects));
        int v = (std::min)(total, textCount), m = total > textCount ? total - textCount : 0;
        il2cpp_field_set_value(menu, r.visibleCount, &v);
        il2cpp_field_set_value(menu, r.maxScroll, &m);
    }
    std::string label = GetField<bool>(menu, r.useUppercase) ? "ENCORE" : kEntryLabelUtf8;
    void* textArgs[] = {il2cpp_string_new(label.c_str())};
    Invoke(r.setText, text, textArgs);
    Log("menu: added \"%s\" as entry %d (%s)", kEntryLabelUtf8, n + 1, nt > n ? "spare text object" : "cloned the last entry");
}

// Timer: finds the live main menu (it is created again whenever the menu scene loads), installs
// the hook once and adds the entry to each new MainMenu instance.
static void MenuTick()
{
    MenuRefs& r = g_menu;
    if (!r.resolved || r.disabled)
        return;
    Il2CppObject* menu = FindActiveInstance(r.mainMenu);
    if (!menu)
        return;
    if (!r.hooked)
    {
        r.hooked = InstallMenuHook();
        if (!r.hooked)
        {
            r.disabled = true;
            KillTimer(g_unityWnd, kMenuTimer);
            return;
        }
    }
    AddMenuEntry(menu);
    SyncClonedEntry(menu);
}


// The player confirmed "Encore": start the companion (worker thread) and show the result later.
static void MenuEncoreSelected(Il2CppObject* menu)
{
    Il2CppObject* songScan = GetField<Il2CppObject*>(menu, g_menu.mmSongScan);
    if (songScan && GetField<bool>(songScan, g_refs.waitForFinish))
        return; // the game ignores menu input while the scan overlay shows, too
    if (!RequestCompanionPairing(g_unityWnd, WM_ENCORE_COMPANION, TriggerPort()))
    {
        Log("menu: Encore selected, pairing request already running");
        return;
    }
    Log("menu: Encore selected, asking the companion for a pairing code");
}

static bool MenuGameObjectActive(Il2CppObject* go)
{
    Il2CppObject* boxed = go ? Invoke(g_menu.activeSelf, go, nullptr) : nullptr;
    return boxed && *static_cast<bool*>(il2cpp_object_unbox(boxed));
}

static void ShowCompanionDialog(const CompanionResult& res)
{
    MenuRefs& r = g_menu;
    Il2CppObject* menu = r.showDialog ? FindActiveInstance(r.mainMenu) : nullptr;
    if (!menu)
    {
        Log("menu: main menu gone, pairing dialog not shown");
        return;
    }
    for (FieldInfo* f : r.subMenus)
        if (MenuGameObjectActive(GetField<Il2CppObject*>(menu, f)))
        {
            Log("menu: another menu is open, pairing dialog not shown");
            return;
        }
    // MainMenu has several ConfirmationMenu dialogs; MainMenu.Update blocks menu input while any
    // of them is open. confirmMenu has no body text object (the game always passes null there), so
    // take the first dialog that has one, and confirmMenu with everything in the title otherwise.
    Il2CppObject* dialog = nullptr;
    const char* dialogName = "";
    for (FieldInfo* f : r.dialogs)
    {
        Il2CppObject* d = GetField<Il2CppObject*>(menu, f);
        if (d && GetField<bool>(d, r.isActive))
        {
            Log("menu: a dialog is already open, pairing dialog not shown");
            return;
        }
        if (d && !dialog && GetField<Il2CppObject*>(d, r.dialogBody) && GetField<Il2CppObject*>(d, r.dialogTitle))
        {
            dialog = d;
            dialogName = il2cpp_field_get_name(f);
        }
    }
    bool titleOnly = !dialog;
    if (titleOnly)
    {
        dialog = GetField<Il2CppObject*>(menu, r.confirmMenu);
        dialogName = "confirmMenu, title only";
    }
    if (!dialog || !GetField<Il2CppObject*>(dialog, r.dialogTitle))
    {
        Log("menu: no usable dialog, pairing dialog not shown");
        return;
    }
    std::string body;
    if (res.ok)
    {
        std::string url = res.url;
        if (!url.empty() && url.back() == '/')
            url.pop_back();
        body = "On your phone (same Wi-Fi), open\n" + (url.empty() ? std::string("(no network address found)") : url) +
               "\nand enter the code\n" + res.code.substr(0, 3) + " " + res.code.substr(3) + "\n\nThe code works once and expires in " +
               std::to_string(res.expiresMinutes > 0 ? res.expiresMinutes : 10) + " minutes.";
        if (res.started)
            body += " The companion runs in its own window; close it to stop it.";
    }
    else
        body = res.error + "\n\nSee encore.log and the README for how to set up the companion.";
    std::string title = titleOnly ? "Encore companion\n" + body : "Encore companion";
    void* args[] = {il2cpp_string_new(title.c_str()), il2cpp_string_new("OK"), nullptr, nullptr, nullptr,
                    titleOnly ? nullptr : il2cpp_string_new(body.c_str())};
    Invoke(r.showDialog, dialog, args);
    Log("menu: pairing dialog (%s, %s) %s", res.ok ? "code" : "error", dialogName, g_invokeFailed ? "FAILED" : "shown");
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
    {
        Log("ready: press F5 in the song list (or POST /rescan) to rescan songs");
        if (ResolveQueueRefs(game))
        {
            Log("queue ready: POST /queue adds a song to the setlist, GET /setlist shows it");
            LogLibraryState("startup");
        }
        // ENCORE_MENU=0 leaves the main menu untouched.
        wchar_t menuEnv[4];
        if (GetEnvironmentVariableW(L"ENCORE_MENU", menuEnv, 4) > 0 && menuEnv[0] == L'0')
            Log("menu: Encore menu entry off (ENCORE_MENU=0)");
        else if (ResolveMenuRefs(game))
            SetTimer(hwnd, kMenuTimer, 500, nullptr);
    }
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
    if (msg == WM_TIMER && wp == kRestoreTimer)
    {
        RestoreSetlistIfScanDone();
        return 0;
    }
    if (msg == WM_TIMER && wp == kMenuTimer)
    {
        MenuTick();
        return 0;
    }
    // Posted by the companion launcher thread.
    if (msg == WM_ENCORE_COMPANION)
    {
        auto* res = reinterpret_cast<CompanionResult*>(lp);
        ShowCompanionDialog(*res);
        delete res;
        return 0;
    }
    // Sent by the HTTP listener thread with SendMessageTimeout; handled here on the main thread.
    if (msg == WM_ENCORE_TRIGGER)
    {
        if (wp == kCmdRescan)
            return TriggerRescanFromSongList("http");
        if (wp == kCmdStatus)
            return QueryStatus();
        if (wp == kCmdQueue || wp == kCmdSetlist || wp == kCmdLibrary)
        {
            // Returning 1 tells the listener that this side took (and released) its reference.
            auto* req = reinterpret_cast<JsonRequest*>(lp);
            if (wp == kCmdQueue)
                HandleQueue(req);
            else if (wp == kCmdSetlist)
                HandleSetlist(req);
            else
                HandleLibrary(req);
            if (req->httpStatus == 0)
                ReplyResult(req, 500, "failed", "request not handled, see encore.log");
            ReleaseRequest(req);
            return 1;
        }
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
