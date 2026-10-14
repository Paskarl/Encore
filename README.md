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
| Companion web app, step 1: search your library from a phone, queue songs, rescan (`GET /library`) | **Working** (tested from a phone), see [Companion web app](#companion-web-app) |
| Companion web app, step 2: find missing charts on enchor.us, download and queue them | **Prototype working** (tested on the PC), see [Download missing charts](#download-missing-charts) |
| Remote song queueing (`POST /queue`, `GET /setlist`) | **Prototype working**: queues into the game's setlist from the song list and while a setlist plays, see [Queue songs](#queue-songs) |

Encore does two things:
- start the same full scan as *Settings → General → Scan Songs* while the song list is open,
  either when you press **F5** or when another application asks for it over a local HTTP
  endpoint. When the scan finishes the list refreshes, so new charts show up without leaving the
  song list;
- let other applications queue a song: it is added to the game's setlist, also while the setlist
  is already being played.

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
| `POST /queue`, `GET /setlist` | Song queue, see [Queue songs](#queue-songs). |
| `GET /library?q=…&limit=…` | Library search, see [Search the library](#search-the-library). |

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

### Queue songs

Clone Hero's setlist is the queue. `POST /queue` finds a song in the library and appends it to the
setlist:

- **In the song list** it is added exactly like the game's "add to setlist" button: the setlist
  panel opens and shows it. Start the setlist as usual.
- **While a setlist is playing** it is appended to the running setlist and played after the songs
  already in it. If the end screen of the last song is already showing ("setlist finished"),
  Encore switches it back to "continue" so the queued song is played.

Parameters, as query string, form body or JSON body (`{"checksum":"…"}`):

| Parameter | Meaning |
|---|---|
| `checksum` | Chart checksum (MD5, as shown by enchor.us and in `GET /setlist`). Preferred: unique. |
| `title`, `artist` | Alternative to `checksum`: exact title, optionally artist (case-insensitive, without formatting tags). The first match is queued; `matches` in the answer tells whether there were more. |
| `speed` | Song speed in percent. Default: the speed the players chose before the setlist (100 if no song was played yet). |

| Answer | Meaning |
|---|---|
| `200 {"result":"queued","mode":"song_list"\|"setlist_running","song":{…},"speed":100,"position":4,"count":4,"matches":1}` | Queued; `position` is its place in the setlist. |
| `400 bad_request` | Neither `checksum` nor `title` given, or an invalid `speed`. |
| `404 not_found` | No such song in the library. After downloading a chart, rescan first. |
| `409 practice` | Practice mode has no setlist. |
| `409 quickplay` | A single song is playing, not a setlist. It ends back in the menu, so there is nothing to queue into. |
| `409 not_in_song_list` | Another menu is open. Open the song list and try again. |
| `503 loading` / `timeout` / `not_ready` | The next setlist song is loading, the game didn't answer within 3 s, or Encore isn't initialised. Try again. |

`GET /setlist` answers `{"playing":true,"index":1,"songs":[{"title":…,"artist":…,"checksum":…,"speed":100},…]}`
(`index` is the zero-based position of the song being played).

Example client ([`tools/encore-queue.ps1`](tools/encore-queue.ps1)):

```powershell
.\tools\encore-queue.ps1 -Checksum 0F57BDA89A2B6A2A91FA8A3938419B2F
.\tools\encore-queue.ps1 -Title 'Shadow Moses' -Artist 'Bring Me The Horizon'
.\tools\encore-queue.ps1 -Title 'Teardrops' -Speed 80
.\tools\encore-queue.ps1 -List      # show the setlist
```

Or with curl: `curl -X POST "http://127.0.0.1:47321/queue?checksum=0F57BDA89A2B6A2A91FA8A3938419B2F"`.

### Search the library

`GET /library?q=<text>&limit=<n>` searches the songs Clone Hero has loaded. Every word of `q` must
appear in the title or the artist (case-insensitive, without formatting tags). Exact and prefix
title matches come first, then artist matches, then the rest, each group sorted by artist and
title. An empty `q` lists the library from the start. `limit` defaults to 50, maximum 200.

```json
{"query":"moses","total":1,"library":1649,"songs":[{"title":"Shadow Moses","artist":"Bring Me The Horizon",
 "album":"Sempiternal","checksum":"9614F4EFD22D895A94C20637B69552E9","length":247}]}
```

`total` is the number of matches (more than `songs` when the limit cut them off), `library` the
number of songs, `length` the song length in seconds (`null` if unknown). Queue a result with
`POST /queue?checksum=…`. Encore indexes the library on the first search and again whenever the
song list changes (after a rescan); a search then takes a few milliseconds.

Or with curl: `curl "http://127.0.0.1:47321/library?q=bring%20horizon&limit=5"`.

### Companion web app

The companion ([`companion/`](companion/)) is a small web server on the PC that lets phones on the
same Wi-Fi search the library, queue songs and start a rescan. Encore itself stays reachable only
from the PC; the companion forwards a fixed set of requests to it.

Requires the .NET 8 SDK. Start it in a console window and leave it running during a session:

```powershell
dotnet run -c Release --project companion
```

1. The console shows a QR code and the address `http://<PC address>:47322/#pair=<code>`. Scan it
   with the phone's camera. The page opens and remembers the pairing, so later you can just open
   `http://<PC address>:47322/` (add it to the home screen).
   If the QR code in the console doesn't scan, open `http://localhost:47322/pair` on the PC; it
   shows the same code larger. On the first start that page opens by itself.
2. Search for a song and tap **Queue**. It goes to the end of the setlist, like `POST /queue`:
   from the song list (start the setlist as usual) or while a setlist is playing.
3. The page shows the setlist (the current song highlighted), whether the game is running, and a
   **Rescan** button.
4. Not in your library? Tap **Search enchor.us**, see [Download missing charts](#download-missing-charts).

**Firewall:** the first start makes Windows ask whether to allow the app on networks. Allow it for
**private networks** only. If the phone can't load the page, check that the Wi-Fi is set to
*Private* in Windows and that the prompt wasn't dismissed (*Windows Defender Firewall → Allow an
app*).

**Security:**
- Every request except the empty page needs the pairing code, a random 128-bit token stored in
  `%LOCALAPPDATA%\Encore\companion-token.txt`. Requests without it are refused (`401 unpaired`).
- The code is shown only on the PC: in the console and on the `/pair` page, which answers only to
  a browser on the PC itself.
- Requests from addresses outside private networks (anything but `10.x`, `172.16–31.x`,
  `192.168.x`, link-local and loopback) are refused, so the companion isn't usable from the
  internet even through a port forward. Don't forward the port.
- Anyone with the code can queue songs, start rescans and download charts that showed up in their
  enchor.us search, while the companion runs; nothing more. To revoke all paired phones, start it
  once with `--new-token` and scan the new code.

Options: `--port <n>` (default 47322), `--encore-port <n>` (default 47321, or `ENCORE_PORT`),
`--songs-dir <folder>` (where downloads go, see below), `--new-token`.

#### Download missing charts

If a song isn't in your library, **Search enchor.us for "…"** below the library results searches
[enchor.us](https://www.enchor.us/), the community chart database, for that text. Results show
title, artist, charter, length and instruments, and whether you already have the song:

- **In your library**: this exact chart was downloaded here before. **Queue** queues it.
- **You have this song (maybe another chart)**: a song with the same title and artist is in the
  library. **Queue yours** queues that one; **Download** fetches this chart anyway.
- Otherwise **Download & queue**.

Download & queue saves the chart as a single `.sng` file (Clone Hero reads those directly) to
`Songs\Encore Downloads\` in your first Clone Hero song folder, then waits until Clone Hero has it
and queues it. The **Downloads** list on the page shows each step:

1. *Downloading* with progress. The file is written as `….sng.part` and renamed when complete, so
   neither Clone Hero nor `encore-watch.ps1` ever sees a partial file.
2. *Scanning*: if the song list is open, the companion starts a rescan itself (the folder watcher
   may already have). Otherwise it waits: *Open the song list in Clone Hero to load it*.
3. *Queued at position N*, like `POST /queue`.

Charts with a video background are downloaded without the video. If the downloaded chart turns out
to be one you already have, the file is discarded and your copy is queued. Downloads are recorded
in `%LOCALAPPDATA%\Encore\downloads.json`, so a chart is never fetched twice.

Clone Hero identifies a song by the MD5 of its chart file (`notes.mid` or `notes.chart`; for a
`.sng`, the one packed inside). enchor.us's `md5` is a different value (it names the download), so
before downloading, the library can only be matched by title and artist. After downloading, the
companion reads the checksum from the `.sng` and queues by it.

**About the enchor.us API:** enchor.us has no official, documented API, terms of use or published
rate limits. The companion uses the same two endpoints as
[Bridge](https://github.com/Geomitron/Bridge) (`POST api.enchor.us/search`, `GET files.enchor.us/<md5>.sng`)
and keeps its use small:
- only on request: one search per tap on *Search enchor.us* or *More results* (never while
  typing), one download per tap; no background crawling and no bulk downloads;
- at most one search per second and one download at a time; downloads only for charts from a
  search result;
- it honours the `X-RateLimit-Remaining`/`-Reset` headers and backs off after `429` (the page
  says when to try again);
- it identifies itself as `EncoreCompanion/0.2 (+https://github.com/Paskarl/Encore)`.


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
| `queue resolve: … -> ok` / `queue ready: …` | The song library, setlist lists and `SetlistMenu.Add` were found; `/queue` works. |
| `queue resolve: … -> FAILED` / `… not unique` | The game changed so that the setlist can't be found safely. `/queue` answers `not_ready`; rescans still work. |
| `SetlistMenu.Add ambiguous …` | Queueing from the song list still works, but the setlist panel only shows the song after it is reopened. |
| `library state (…)` | Sizes of the library's song lists and every setlist entry (song, speed, string). Written at startup, on `GET /setlist` when the setlist changed, and after each queued song. |
| `library: indexed N songs for search` | `GET /library` read the song list (first search, or the list changed). |
| `queue: … song length not available` | The song length field wasn't found; `/library` answers `"length":null`, everything else works. |
| `queued (mode, N matches): …` / `queue refused: …` | Result of a `/queue` request. |
| `queue: end screen had ended the setlist; switched it back to "continue"` | A song was queued on the last song's end screen. |

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
- The song library class has an obfuscated name. Encore finds it by its fields (only statics,
  a `Dictionary<_, List<SongEntry>>` and several `List<SongEntry>`), and the setlist as the one
  `List<SongEntry>`, `List<int>`, `List<string>` declared in a row.
- `SetlistMenu.Add` has two obfuscator copies; the real one is called from far more places (8 vs
  1 and 0), which Encore checks with the same call-site scan. `GlobalVariables`, `SongSelect`,
  `EndOfSong` and their fields are found by name.
- The song length field of `SongEntry` has an obfuscated name. `get_HasLength` is `length > 0`;
  Encore decodes its first instruction (`cmp dword [rcx+offset], 0`) and takes the `int` field at
  that offset. If the instruction looks different, `/library` reports no lengths.

## Repository layout

| Path | Contents |
|---|---|
| `src/encore.cpp` | Startup, main-thread hook, StartScan detection, rescan, status and song queue on the main thread |
| `src/trigger_server.cpp`, `src/trigger_server.h` | Loopback HTTP listener (`/rescan`, `/status`, `/queue`, `/setlist`, `/library`) |
| `companion/` | Companion web server (.NET 8): `Program.cs` (server, pairing, Encore calls), `Enchor.cs` (enchor.us search, downloads, rescan and queue), `Sng.cs` (Clone Hero checksum of a `.sng`), `index.html` (phone page) |
| `src/il2cpp_api.h` | The IL2CPP runtime functions Encore calls |
| `src/version_proxy.cpp`, `src/version.def` | `version.dll` export forwarding |
| `tools/encore-rescan.ps1` | Example client for the trigger interface |
| `tools/encore-queue.ps1` | Example client for the song queue |
| `tools/encore-watch.ps1` | Song folder watcher that starts a rescan after downloads |
| `tools/` (other scripts) | Scripts for repeating the analysis after a game update |
| `docs/ANALYSIS.md` | How the rescan function was found, step by step |

## Debug dump

With the environment variable `ENCORE_DUMP=1`, Encore also writes:
- the fields and methods of all scan-related classes, to `encore.log`;
- an address map of every game method, to `encore_methods.tsv`.

`ENCORE_DUMP_FILTER` chooses which classes are dumped: a comma-separated list of class-name
substrings (e.g. `SongSelect,Setlist,EndOfSong`), or `*` for every class. Dumping everything is how
classes with obfuscated names are found by their field types.

The analysis tools take that map as input. These files are derived from the game's code. Keep
them local and don't commit or publish them.
