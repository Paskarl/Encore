// Encore: rescan songs from Clone Hero's song list.
// Runs inside the game (loaded through the version.dll proxy), subclasses the Unity window
// so all IL2CPP calls happen on the Unity main thread, and starts a song rescan when F5 is
// pressed in the song list or another application sends POST /rescan to the loopback HTTP
// listener (trigger_server.cpp). The listener can also queue songs into the game's setlist
// (POST /queue). With ENCORE_DUMP=1 it also dumps the analysed classes.
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

// GET /setlist
static void HandleSetlist(JsonRequest* req)
{
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
