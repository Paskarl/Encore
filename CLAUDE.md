# Encore

Native Windows DLL that adds a song rescan to Clone Hero's song list, triggered by F5 or by other
applications through a loopback HTTP endpoint (`POST /rescan`, `GET /status` on
127.0.0.1:47321, `src/trigger_server.cpp`). It is loaded as a `version.dll` proxy into the Unity
IL2CPP game and calls game code through the IL2CPP runtime API.

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

## Rules

- Call IL2CPP only on Unity's main thread (the subclassed window procedure). Off-thread calls that
  allocate abort the game with "Fatal error in GC". The bootstrap thread may only poll
  `il2cpp_get_corlib()`. The HTTP listener thread follows the same rule: it only does socket work
  and hands every request to the window procedure with `SendMessageTimeout(WM_ENCORE_TRIGGER)`.
- Every rescan trigger goes through `TriggerRescanFromSongList(source)`; don't duplicate its checks.
  Rescans are refused outside the song list (unsafe during gameplay/loading).
- The listener binds to `127.0.0.1` only. Don't widen it to other interfaces without an explicit
  decision about authentication.
- Don't hard-code RVAs or obfuscated names in `src/`. Resolve by unobfuscated class/field names, or
  by behaviour (see the StartScan caller test). If detection is ambiguous, disable the feature and
  log it; never guess.
- Never commit dumps, slices or decompiled output (`*.tsv`, `encore.log`, `*_slice*.dll`). They are
  derived from the game's code; `.gitignore` covers them.
- REA can't analyse the full 70 MB `GameAssembly.dll` (Ghidra startup timeout). Use
  `tools/make-slice.ps1` and open the slice instead.
- PowerShell 5.1: pass commit messages with `git commit -F <file>`, because quotes in `-m` get
  mangled. Write files without a BOM.
