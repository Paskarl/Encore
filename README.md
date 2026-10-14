# Encore

Rescan songs in Clone Hero right from the song list. Press **F5** and the game runs the same
full scan as *Settings → General → Scan Songs*. When the scan finishes the list refreshes, so new
charts show up without leaving the song list.

Tested with Clone Hero **v1.1.0.6142-final** (Windows x64). Encore does not depend on fixed
addresses or obfuscated names, so it should keep working across updates as long as the
structure of the scan code stays the same (see [Update resistance](#update-resistance)).

## Install

Requires Visual Studio 2022 Build Tools (MSVC x64).

```bat
build.bat
copy build\version.dll "C:\Users\pasca\Documents\cloneHero\Clone Hero\"
```

Clone Hero loads `version.dll` from its own folder, because `UnityPlayer.dll` imports
`VERSION.dll`. Encore forwards every export to the real `System32\version.dll`.

**Uninstall:** delete `version.dll` from the game folder.

## Use

1. Start Clone Hero and open the song list.
2. Press **F5**. The scan overlay appears, the scan runs, and the list refreshes when it's done.

F5 does nothing in other menus or while a scan is already running.

## Log

Encore writes `encore.log` next to `Clone Hero.exe`:

| Line | Meaning |
|---|---|
| `ready: press F5 in the song list to rescan songs` | Everything resolved, F5 is active. |
| `StartScan candidate RVA=0x… external callers=N` | One line per candidate method, see below. |
| `StartScan ambiguous: …` | The game changed so that StartScan can't be identified safely. F5 stays off. |
| `resolve: … -> FAILED` / `resolve failed: …` | A class, field or Unity method Encore needs was not found. F5 stays off. |
| `F5 ignored: …` | F5 was pressed outside the song list, or a scan was already running. |
| `rescan started from song list` | F5 started a scan. |

In every failure case Encore does nothing, and the game keeps running normally.

## Update resistance

- Class and field names (`SongScan`, `SongSelect`, `isScanning`, `songSelect`, `container`,
  `waitForFinish`) are not obfuscated and are looked up by name at runtime.
- The scan overlay is shown by setting those fields directly, instead of calling the obfuscated
  `Show()` method.
- `SongScan` has four methods of the form `Coroutine X(bool)`. Three are obfuscator decoys.
  At startup Encore scans `GameAssembly.dll` for `call`/`jmp` instructions to each one. Calls from
  inside `SongScan` don't count. Only the real `StartScan` is called from other classes (settings
  menu, main menu, game start). If exactly one candidate has outside callers, Encore uses it.
  Otherwise F5 stays off rather than guessing.

## Repository layout

| Path | Contents |
|---|---|
| `src/encore.cpp` | Startup, main-thread hook, StartScan detection, F5 handler |
| `src/il2cpp_api.h` | The IL2CPP runtime functions Encore calls |
| `src/version_proxy.cpp`, `src/version.def` | `version.dll` export forwarding |
| `tools/` | Scripts for repeating the analysis after a game update |
| `docs/ANALYSIS.md` | How the rescan function was found, step by step |

## Debug dump

With the environment variable `ENCORE_DUMP=1`, Encore also writes:
- the fields and methods of all scan-related classes, to `encore.log`;
- an address map of every game method, to `encore_methods.tsv`.

The analysis tools take that map as input. These files are derived from the game's code. Keep
them local and don't commit or publish them.
