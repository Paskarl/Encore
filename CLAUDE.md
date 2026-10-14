# Encore

Native Windows DLL that adds a song rescan to Clone Hero's song list, triggered by F5 or by other
applications through a loopback HTTP endpoint (`POST /rescan`, `GET /status` on
127.0.0.1:47321, `src/trigger_server.cpp`), lets other applications queue songs into the
game's setlist (`POST /queue`, `GET /setlist`) and search the library (`GET /library`). It is
loaded as a `version.dll` proxy into the Unity IL2CPP game and calls game code through the IL2CPP
runtime API. `companion/` is a separate .NET 8 web server that makes search, queue and rescan
usable from a phone on the LAN, and downloads missing charts from enchor.us.

- What it does and how to use it: `README.md`
- How the target was reverse engineered, with evidence and an update checklist: `docs/ANALYSIS.md`.
  Read this before changing detection logic or re-analysing a new game version.

## Build and run

- Build: `build.bat` (MSVC x64 from VS 2022 Build Tools, compiles with `/utf-8`) → `build\version.dll`
- Game folder: `C:\Users\pasca\Documents\cloneHero\Clone Hero`. Copy `build\version.dll` there.
  The copy fails while the game is running, because the DLL is loaded and locked.
- Log: `encore.log` in the game folder. `ENCORE_DUMP=1` adds a class dump and `encore_methods.tsv`.
- The F5 test inside the song list must be done by the user. Posted keys can't navigate the menus
  (Rewired/raw input), and window captures come out black. `tools/gamectl.ps1` can only test F5
  outside the song list.
- The HTTP trigger can be tested from here with `tools/encore-rescan.ps1` (`-Status`, `-Wait`).
  Outside the song list it must answer `not_in_song_list`; the in-list refresh needs the user.
- The queue can be driven from here with `tools/encore-queue.ps1` (`-Title`, `-Checksum`, `-List`)
  while the user navigates. `GET /setlist` also writes the library and setlist state to the log
  (only when it changed, because the companion polls it).
- `GET /library` works in any menu and can be tested from here with curl.
- Companion: `dotnet run -c Release --project companion` (port 47322). Run it in a Terminal panel
  tab so the user sees the QR code. This session's sandbox sees its own copy of `%LOCALAPPDATA%`,
  so the token file read from Bash may differ from the one the real process uses; take the token
  from the companion's console output. The phone test must be done by the user.

## Rules

- Call IL2CPP only on Unity's main thread (the subclassed window procedure). Off-thread calls that
  allocate abort the game with "Fatal error in GC". The bootstrap thread may only poll
  `il2cpp_get_corlib()`. The HTTP listener thread follows the same rule: it only does socket work
  and hands every request to the window procedure with `SendMessageTimeout(WM_ENCORE_TRIGGER)`.
- Every rescan trigger goes through `TriggerRescanFromSongList(source)`; don't duplicate its checks.
  Rescans are refused outside the song list (unsafe during gameplay/loading).
- Requests that answer with JSON from the main thread (`/queue`, `/setlist`, `/library`) pass a refcounted
  `JsonRequest` as lParam; the window procedure returns 1 when it took and released it.
- A song scan empties the setlist at scan start. `TriggerRescanFromSongList` saves it
  (`SaveSetlistForRescan`) and `RestoreSetlistIfScanDone` adds it back by checksum; that runs from
  a timer and before `/status`, `/queue` and `/setlist`, so queued songs land behind the restored
  ones. Keep that order when adding new requests that touch the setlist.
- The setlist is three parallel lists (song, speed, string); keep them the same length. Queue
  through `SetlistMenu.Add` when the song list is open, append to the lists otherwise.
- The listener binds to `127.0.0.1` only. Don't widen it to other interfaces without an explicit
  decision about authentication. LAN access goes through the companion only.
- Companion: every `/api` route needs the pairing token (`Authorization: Bearer`, constant-time
  compare); `/pair` (shows the token) answers only to loopback with a loopback Host header;
  non-private source addresses are refused. It forwards a fixed set of requests and validates
  parameters (checksum = 32 hex digits), never arbitrary paths. Don't add firewall rules or
  internet exposure without asking the user.
- enchor.us (unofficial API, no terms or published limits; the user should confirm with the
  maintainer on the Chorus Encore Discord) is called only from the companion, never from the DLL,
  and only on explicit user actions: one search per tap, one download per tap, no
  search-as-you-type, crawling, prefetching or bulk download. Keep the throttle (1 search/s, one
  download at a time), the rate-limit/429 handling and the User-Agent with the repo URL. Downloads
  only for md5s from a search result. Test against it sparingly.
- enchor.us's `md5` is NOT Clone Hero's checksum. Clone Hero's checksum is the MD5 of the chart
  file (`notes.mid`/`notes.chart`, inside the `.sng` for packed songs); `Sng.cs` computes it after
  download. Before download, match the library by title + artist or by `downloads.json`.
- Write downloads as `.part` in the target folder and rename when complete.
- Don't hard-code RVAs or obfuscated names in `src/`. Resolve by unobfuscated class/field names, or
  by behaviour (see the StartScan caller test). If detection is ambiguous, disable the feature and
  log it; never guess.
- Never commit dumps, slices or decompiled output (`*.tsv`, `encore.log`, `*_slice*.dll`). They are
  derived from the game's code; `.gitignore` covers them.
- REA can't analyse the full 70 MB `GameAssembly.dll` (Ghidra startup timeout). Use
  `tools/make-slice.ps1` and open the slice instead. To see who uses a class's statics, run
  `tools/find-refs.ps1` on its class-pointer global (the `DAT_183…` value in pseudocode); for who
  calls a method, `tools/find-callers.ps1`. `ENCORE_DUMP_FILTER=*` dumps every class.
- PowerShell 5.1: pass commit messages with `git commit -F <file>`, because quotes in `-m` get
  mangled. Write files without a BOM.
