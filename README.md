# Encore

Encore makes Clone Hero's song library refreshable without leaving the song list, as a first
step towards letting external tools add songs to a running game.

## Motivation

Picture a jam session: a few friends are playing Clone Hero, and someone asks *"Do you have this
song?"* With a library of a couple of thousand charts, the honest answer is usually "no idea".
And if the song is missing, getting it today is a chore:
1. Download the chart with a tool like [Bridge](https://github.com/Geomitron/Bridge) or from
   [enchor.us](https://www.enchor.us/).
2. Leave the song list and open *Settings*.
3. Run a song scan and go back to the song list.

That breaks the flow of the session every time.

The goal of Encore is to remove those last steps. Any application that downloads charts should be
able to tell Clone Hero "there is something new", and the song list should simply update while
everyone keeps playing.

## Vision

Encore is meant to become the bridge between Clone Hero and external tools:

- **Automatic refresh after downloads.** A chart downloader (Bridge, a companion app, a script
  watching the songs folder) notices a new chart and triggers the rescan through Encore. Seconds
  later the song shows up in the list.
- **Companion web app.** Open a page on your phone, search the library or chart sources like
  enchor.us, and download a missing song while others are still playing.
- **Remote queueing.** Pick the next song from your phone and have it ready in the game when the
  current song ends.

## Status

| Feature | State |
|---|---|
| Rescan from the song list (F5), list refreshes afterwards | **Working** (proof of concept) |
| Survives game updates (no hard-coded addresses) | **Working**, see [Update resistance](#update-resistance) |
| Trigger interface for other applications (e.g. local HTTP or named pipe) | Planned |
| Bridge / folder-watcher integration | Planned, needs the trigger interface |
| Companion web app: search and download | Planned |
| Remote song queueing | Research needed: song selection in the game is not reverse engineered yet |

Today Encore does one thing: press **F5** in the song list and the game runs the same full scan
as *Settings → General → Scan Songs*. When the scan finishes the list refreshes, so new charts show
up without leaving the song list. The rescan logic is written so that a different trigger (an
external request instead of a key press) can reuse it.

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
