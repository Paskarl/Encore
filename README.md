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
| Trigger interface for other applications (local HTTP: `POST /rescan`, `GET /status`) | **Working**, see [Trigger from other applications](#trigger-from-other-applications) |
| Bridge / folder-watcher integration | **Working** as a song folder watcher that works with any downloader, see [Rescan automatically after downloads](#rescan-automatically-after-downloads) |
| Companion web app: search and download | Planned |
| Remote song queueing | Research needed: song selection in the game is not reverse engineered yet |

Today Encore does one thing: start the same full scan as *Settings → General → Scan Songs* while
the song list is open, either when you press **F5** or when another application asks for it over
a local HTTP endpoint. When the scan finishes the list refreshes, so new charts show up without
leaving the song list.

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

### Trigger from other applications

While the game runs, Encore listens on **`http://127.0.0.1:47321`**. It binds to loopback only,
so only programs on the same PC can reach it.

| Request | Response |
|---|---|
| `POST /rescan` | `202 {"result":"started"}`: the scan started, the list refreshes when it's done. |
| | `409 {"result":"not_in_song_list"}`: the song list is not open. Nothing happens. |
| | `409 {"result":"scanning"}`: a scan is already running. |
| | `503 {"result":"not_ready"}` / `{"result":"timeout"}`: Encore isn't initialised, or the game didn't answer within 3 s (e.g. while loading). Try again. |
| `GET /status` | `200 {"ready":true,"scanning":false,"songList":true}` |

An external rescan behaves exactly like F5: it only runs while the song list is open. In other
menus or during a song it is refused with `not_in_song_list`, because a scan during gameplay or
a scene load isn't safe and the list refresh only applies to an open song list. A downloader
should call `/rescan` after a chart is complete; if the answer is `not_in_song_list`, retry later
or poll `/status` until `songList` is true.

Example client ([`tools/encore-rescan.ps1`](tools/encore-rescan.ps1)):

```powershell
.\tools\encore-rescan.ps1           # start a rescan
.\tools\encore-rescan.ps1 -Wait     # start a rescan and wait until it has finished
.\tools\encore-rescan.ps1 -Status   # show status
```

Or with curl: `curl -X POST http://127.0.0.1:47321/rescan`.

To use a different port, set the environment variable `ENCORE_PORT` before starting Clone Hero.
`ENCORE_PORT=0` turns the listener off.

### Rescan automatically after downloads

[`tools/encore-watch.ps1`](tools/encore-watch.ps1) watches your song folders and asks Encore for a
rescan when charts are added or changed. It works with any downloader (Bridge, a browser, a zip
tool) without changes to that downloader.

```powershell
.\tools\encore-watch.ps1                                   # watches Documents\Clone Hero\Songs
.\tools\encore-watch.ps1 -Path 'D:\Songs', 'E:\More Songs'  # other or several song folders
.\tools\encore-watch.ps1 -QuietSeconds 10                  # wait longer after the last change
```

- It reacts to `song.ini`, `notes.chart`, `notes.mid` and `.sng` files, and to new or renamed
  folders that contain them. Other files are ignored.
- After a burst of changes it waits until nothing has changed for `-QuietSeconds` (default 5), so
  a download can finish extracting, then sends one `POST /rescan`.
- If the song list isn't open, the rescan stays pending until you open it, then runs. If a scan
  is already running, it runs again after that scan. If the game isn't running, it keeps retrying.
- Leave it running in a console window during a session and stop it with Ctrl+C.

## Log

Encore writes `encore.log` next to `Clone Hero.exe`:

| Line | Meaning |
|---|---|
| `ready: press F5 in the song list (or POST /rescan) to rescan songs` | Everything resolved, F5 and `/rescan` are active. |
| `http: listening on http://127.0.0.1:47321 …` | The trigger interface is up. |
| `http: cannot listen on …` | The port is taken (another program, or a second game instance). F5 still works. |
| `StartScan candidate RVA=0x… external callers=N` | One line per candidate method, see below. |
| `StartScan ambiguous: …` | The game changed so that StartScan can't be identified safely. F5 stays off. |
| `resolve: … -> FAILED` / `resolve failed: …` | A class, field or Unity method Encore needs was not found. F5 stays off. |
| `F5 ignored: …` / `http ignored: …` | F5 or `/rescan` came outside the song list, or a scan was already running. |
| `rescan started from song list (F5)` / `(http)` | F5 or `/rescan` started a scan. |

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
| `src/encore.cpp` | Startup, main-thread hook, StartScan detection, rescan and status on the main thread |
| `src/trigger_server.cpp`, `src/trigger_server.h` | Loopback HTTP listener (`/rescan`, `/status`) |
| `src/il2cpp_api.h` | The IL2CPP runtime functions Encore calls |
| `src/version_proxy.cpp`, `src/version.def` | `version.dll` export forwarding |
| `tools/encore-rescan.ps1` | Example client for the trigger interface |
| `tools/encore-watch.ps1` | Song folder watcher that starts a rescan after downloads |
| `tools/` (other scripts) | Scripts for repeating the analysis after a game update |
| `docs/ANALYSIS.md` | How the rescan function was found, step by step |

## Debug dump

With the environment variable `ENCORE_DUMP=1`, Encore also writes:
- the fields and methods of all scan-related classes, to `encore.log`;
- an address map of every game method, to `encore_methods.tsv`.

The analysis tools take that map as input. These files are derived from the game's code. Keep
them local and don't commit or publish them.
