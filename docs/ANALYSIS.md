# How the rescan function was found

This is a record of the reverse engineering behind Encore: the steps, why each was taken, the
dead ends, and the evidence. It is meant to let you (or a future Claude session) repeat the
analysis after a game update.

## Target

| | |
|---|---|
| Game | Clone Hero v1.1.0.6142-final (`Clone Hero_Data\version.json`), Windows x64 |
| Engine | Unity, IL2CPP backend, metadata version 31 (`global-metadata.dat` header `AF 1B B1 FA 1F 00 00 00`), not encrypted |
| `Clone Hero.exe` | SHA-256 `3ff57876872fdeac94cf88052a69b19e70c7fc58a70919353c9bfcf64dfd2c87` |
| `GameAssembly.dll` | SHA-256 `12bc21e5a88a2fc9abc259d5d67f24835145eabca6c3f863b075ba37ad75f197`, image base `0x180000000` |
| Tools | REA (Ghidra 12.1.4 provider), MSVC 14.44 (VS 2022 Build Tools), PowerShell 5.1 |

All RVAs below refer to this `GameAssembly.dll`.

## 1. What `Clone Hero.exe` is

REA identified `Clone Hero.exe` as Unity's stock launcher: its debug info references
`WindowsPlayer_player_Master_il2cpp_x64.pdb`, and it contains no game logic. The game's C# code
is compiled to native code in `GameAssembly.dll`. Class, method and field names are stored
separately in `Clone Hero_Data\il2cpp_data\Metadata\global-metadata.dat`.

## 2. Candidate names from the metadata

A text search for "scan" in `global-metadata.dat` found, among others: `SongScan`, `SongScan.cs`,
`StartScan`, `isScanning`, `fullScan`, `ScanSong`, `promptRescan`, `SongSelect`.

These narrowed the search to the classes `SongScan` and `SongSelect`. The metadata alone does not
say where their code is.

## 3. Runtime dump through the IL2CPP API

Dumpers like Il2CppDumper or Cpp2IL were not installed. Instead, Encore's DLL asks the game's own
IL2CPP runtime: `GameAssembly.dll` exports `il2cpp_class_get_methods` and friends, and a method's
native address is the first field of its `MethodInfo`.

How the DLL gets into the game and runs safely:
- **Loading:** it is a `version.dll` proxy. `UnityPlayer.dll` imports `VERSION.dll`, and the
  application folder is searched first.
- **Waiting for IL2CPP:** a background thread polls `il2cpp_get_corlib()`, which only reads a
  global.
- **Main thread:** it then subclasses the `UnityWndClass` window. All IL2CPP calls run in that
  window procedure, i.e. on Unity's main thread.

Dead ends here:
- **Crash on startup.** Calling `il2cpp_domain_get()` from the background thread before IL2CPP
  was ready aborted the game with "Fatal error in GC". That function allocates memory through the
  garbage collector, which is not allowed from an unregistered thread.
- **Wrong assembly name.** The game code is not in `Assembly-CSharp.dll` (Unity's default name)
  but in **`CloneHero.dll`**. The log's assembly list showed this.

The dump (now enabled with `ENCORE_DUMP=1`) showed that method names and some field names are
**obfuscated**, e.g. `ʺʹˀˁʿʻʳʹʻʵʳ`. Class names and most fields are not. Relevant parts:

```
class SongScan : MonoBehaviour
    bool isScanning                ; +0x20
    Coroutine <obf>                ; +0x28   running scan coroutine
    SongSelect songSelect          ; +0x58
    MainMenu mainMenu              ; +0x60
    GameObject container           ; +0x68   scan overlay
    bool waitForFinish             ; +0x70
    Coroutine <obf>(bool)          ; RVA 0x1b4fd0
    Coroutine <obf>(bool)          ; RVA 0x1b57d0
    Coroutine <obf>(bool)          ; RVA 0x1b5910
    Coroutine <obf>(bool)          ; RVA 0x1b5c10
    void <obf>()                   ; RVA 0x1b5d80
    nested <obf> : IEnumerator     ; fields: <>4__this, fullScan, <songCache>5__3 ; MoveNext RVA 0x1bb600
```

So there are four methods that could be `StartScan(bool)`.

## 4. Decompiling with REA

Opening the full `GameAssembly.dll` in REA failed: Ghidra's auto-analysis of 70 MB exceeded REA's
~330 s startup deadline. Loading carved code bytes as raw x86-64 also failed, because REA on
Windows only accepts PE files for x86-64.

**Workaround:** [`tools/make-slice.ps1`](../tools/make-slice.ps1) builds a small PE.
- It contains only the code ranges of interest, at their original addresses and with the original
  image base.
- Every method from the runtime address map becomes a named export, which gives Ghidra function
  starts and labels.
- Ghidra analyses it in seconds, and the addresses in the decompiled code are the real ones.
- Calls into code outside the slice show up as `func_0x…`; those are mostly IL2CPP runtime helpers.

Ranges used: `88000-8f000` (SongSelect), `1b4000-1bc000` (SongScan), `1c0000-1c6000` (MainMenu),
`230000-236000` (GlobalVariables), `3a5000-3a8000` (GeneralSettingsMenu).

## 5. Findings

### The four candidates

All four have the same shape:

```c
if (!this->isScanning)
    this->coroutine = StartCoroutine(<scan IEnumerator>(fullScan));
return this->coroutine;
```

They differ only in a preceding static call with a different metadata token, and in the initial
state the IEnumerator factory sets. All of them build the same nested state machine (the one with
the `fullScan` field).

### Separating the real one from the decoys

[`tools/find-callers.ps1`](../tools/find-callers.ps1) scans every executable section for
`call rel32` / `jmp rel32` to each candidate:

```
jmp  at 0x1c2f52 -> 0x1b5c10   in MainMenu::<obf>()
call at 0x23362a -> 0x1b5c10   in GlobalVariables::Start()
call at 0x3a64e7 -> 0x1b5c10   in GeneralSettingsMenu/<coroutine>::MoveNext()
0x1b4fd0: 0 caller(s)
0x1b57d0: 0 caller(s)
0x1b5910: 0 caller(s)
0x1b5c10: 3 caller(s)
```

**`0x1b5c10` is `StartScan(bool fullScan)`.** The other three are dead code planted by the
obfuscator.

### The callers

- **Settings → General → Scan Songs** (`GeneralSettingsMenu` coroutine, `0x3a6450`):
  ```c
  GlobalVariables.instance.<obf>();          // 0x235760
  if (!songScan.isScanning) songScan.StartScan(true);
  songScan.Show();
  ```
- **MainMenu** (`0x1c2f20`):
  ```c
  songScan.Show();
  songScan.StartScan(true);
  ```
- **GlobalVariables.Start** (`0x232e90`): finds the SongScan component and calls
  `StartScan(false)`. This is the quick scan at game start, which uses the song cache.

### Show

`0x1b5d80`:

```c
container.SetActive(true);
waitForFinish = true;
```

### End of the scan coroutine

`MoveNext`, `0x1bb600`, at the end of the scan:
- sets the result text and resets some static state;
- `if (songSelect != null) songSelect.<obf>();` calls `0x8aea0`, the list refresh;
- `container.SetActive(false); waitForFinish = false; isScanning = false;`
- if `mainMenu` is active, re-enables its input.

### SongSelect list refresh

`0x8aea0`:
- if the song library is not empty, resets the selection (`+0x3c = 0`, `+0x40 = 1`);
- calls two virtual `BaseMenu` methods (vtable slots `+0x1a8`, `+0x488`) that rebuild the list.

### Conclusion

To rescan from the song list, do what the main menu does, and also link the open song list so the
coroutine refreshes it:
1. `songScan.songSelect = <active SongSelect>`
2. Show (`container.SetActive(true); waitForFinish = true`)
3. `StartScan(true)`

## 6. Runtime notes

- `Resources.FindObjectsOfTypeAll` is stripped from this build, so Encore uses
  `Object.FindObjectsOfType(Type)`.
- The game must be reached through its main thread. Encore uses the window procedure, which also
  provides the F5 key messages.
- Testing could not be automated:
  - Clone Hero reads input through Rewired/raw input, so posted key messages don't move through
    the menus.
  - `PrintWindow` captures of the game window come out black.
  - What [`tools/gamectl.ps1`](../tools/gamectl.ps1) can verify is the F5 handler outside the song
    list. The real F5 test in the song list was done by hand.

## 7. Song queueing research

**Goal:** queue a song from outside the game (e.g. a phone) so that it plays next, also while
others are already playing. Status: static analysis done and a prototype (`POST /queue`) tested in
the game; see [Prototype and runtime results](#prototype-and-runtime-results) for what the runtime
confirmed. The rest of this section is the static analysis as it was done.

### Method

- `ENCORE_DUMP_FILTER` selects which classes the dump covers; `*` dumps all 1451 classes of
  `CloneHero.dll`. The full dump is what made it possible to find obfuscated classes by their field
  types: searching for `List<SongEntry>` revealed the song library class.
- [`tools/find-callers.ps1`](../tools/find-callers.ps1) separated real methods from decoys again.
- [`tools/find-refs.ps1`](../tools/find-refs.ps1) is new: it lists RIP-relative references to a
  global. IL2CPP reaches a class's static fields through a global holding the class pointer (e.g.
  `DAT_183c9d098`), so this finds every function that uses a class's statics, which call-site
  scanning can't. Sanity check: it finds all 5 accesses in `SetlistMenu.Add`.
- Slice ranges used: `88000-8f000` (SongSelect), `13c000-13f000` (SetlistMenu), `15d000-164000`
  (EndOfSong), `16f000-179000` (song library), `232000-23a000` (GlobalVariables), `296000-29b000`
  (SongOptions).

### The song library class (obfuscated name, class pointer global `DAT_183c9d098`)

A static class with only static fields; it is identifiable by holding a
`Dictionary<checksum, List<SongEntry>>` and several `List<SongEntry>`. Static field offsets follow
declaration order:

| Offset | Type | Meaning |
|---|---|---|
| `+0x00` | `List<SongEntry>` | **all songs** (runtime: 1649 of 1649; read by the rescan's list refresh) |
| `+0x08` | `Dictionary<checksum, List<SongEntry>>` | songs by chart checksum |
| `+0x10` | `List<SongEntry>` | runtime: empty in the song list |
| `+0x20` | `List<SongEntry>` | **setlist songs** |
| `+0x28` | `List<int>` | **setlist song speed per entry** |
| `+0x30` | `List<string>` | setlist string per entry (runtime: `""` for songs added in the song list) |
| `+0x38` | `List<SongEntry>` | runtime: also 1649 songs (a second full list, probably sorted) |
| `+0x48` | `List<entry>` | entries visible in the song list (songs and folders) |
| `+0xb8` | `List<SongEntry>` | runtime: 1541 songs (a filtered subset) |

Relevant static methods:

| RVA | Signature | Meaning | Evidence |
|---|---|---|---|
| `0x16f370` | `entry X(int)` | visible list entry at index | reads `+0x48`; 18 callers in SongSelect |
| `0x171ff0` | `void X()` | clear the setlist | empties `+0x20`, `+0x28`, `+0x30` |
| `0x174950` | `bool X(checksum, out SongEntry)` | find a song by checksum | 9 callers |
| `0x1779f0` | `SongEntry X(Func<SongEntry,bool>)` | find first matching song | callers only in `SongSelectSearch` |

`SongEntry` exposes readable getters such as `get_Name_StrippedTags`, `get_Artist_StrippedTags` and
`get_ChecksumString`, which a lookup by title/artist or checksum can use.

### Adding to the setlist: `SetlistMenu.Add(SongEntry, int, string)` @ `0x13deb0`

`SetlistMenu` has three methods with this signature. `find-callers` shows `0x13deb0` with 8 callers
(SongSelect, SongOptions, one more class), `0x13c650` with 1 and `0x13cd10` with none. `0x13c650`
is a near-identical copy of `0x13deb0` (it differs only in two internal calls) and has one caller
in that third class, so "the only candidate with outside callers" does not hold here. Encore
accepts the candidate with clearly the most outside callers (at least 3, and at least twice any
other).

`0x13deb0`:
1. if the setlist panel is not active: activates and shows it;
2. appends the song to `+0x20`, the int to `+0x28`, the string to `+0x30`;
3. refreshes the setlist display through several virtual `BaseMenu` calls.

The caller in the song list (`SongSelect` @ `0x88c20`, the "add to setlist" button):
- returns in practice mode (`GlobalVariables.isPracticeEnabled`);
- takes the highlighted entry via `0x16f370(selectedIndex)`;
  - single song: `SetlistMenu.Add(song, <song speed setting>, <string constant>)`;
  - folder: adds every song in it;
- if the setlist has focus and is not empty, the same button starts the setlist instead
  (`SongSelect` @ `0x8d260`).

The int is the current value of a settings object (settings class statics `+0x10`). In
`EndOfSong` the same object is set to `GlobalVariables.originalSongSpeed` and to the setlist's int,
so the int is the **song speed**.

### Playing the setlist: `EndOfSong` "continue" @ `0x160260`

`GlobalVariables` (static `instance`) holds `currentSongEntry` (+0x28), `setlistIndex` (+0x58),
`originalSongSpeed` (+0x5c) and `isPlayingSetlist` (+0x74). The continue action on the end-of-song
screen does:

```c
if (!gv.isPlayingSetlist) {                 // quickplay
    songSpeed.Set(gv.originalSongSpeed); load menu scene;
} else if (!this.isLastSong) {              // EndOfSong +0xa1
    gv.setlistIndex++;
    songSpeed.Set(setlistSpeeds[gv.setlistIndex]);
    gv.currentSongEntry = setlistSongs[gv.setlistIndex];
    load gameplay scene;
} else {                                    // setlist finished
    ClearSetlist(); gv.setlistIndex = 1; songSpeed.Set(gv.originalSongSpeed); load menu scene;
}
```

The next song is read **live from the static setlist list** when "continue" is pressed.

### Conclusions for queueing

- **In the menu:** queueing a song means adding it to the setlist, either through
  `SetlistMenu.Add` (needs the menu scene, shows the setlist panel) or by appending to the three
  static lists. The players then start the setlist as usual.
- **During a setlist:** appending to the three static lists (song, speed, string, kept in sync)
  makes the song play after the current setlist songs. `SetlistMenu` does not exist in the gameplay
  scene, so this is the only way there.
- **During quickplay** there is no queue: the end screen returns to the menu. Turning a quickplay
  session into a setlist session is not covered by this research.
- **Finding the song:** by chart checksum (`0x174950`, or the `+0x08` dictionary), which matches how
  chart sites identify charts, or by iterating a `List<SongEntry>` and comparing title/artist.

### When `isLastSong` is set

`EndOfSong.Start` (`0x15dc20`) decides it once, when the end screen opens (byte search for
`mov byte [reg+0xa1]` in the EndOfSong range finds only `Start` and one other method):

```c
if (gv.isPlayingSetlist) {
    if (gv.setlistIndex == setlistSongs.Count - 1) { this.isLastSong = true; panel = this.setlist_end; }
    else                                            { panel = this.setlist; }
    panel.SetActive(true);
}
```

It also writes a "next song" text (`nextSong`, from `setlistSongs[setlistIndex + 1]`) when the
screen opens. So a song appended while the last song's end screen is already showing would be
dropped by "continue" (which then clears the setlist). The prototype handles this: if the end
screen is open with `isLastSong` set and the `setlist_end` panel active, it clears the flag and
swaps the panels to `setlist`. The "next song" text is not updated.

### Prototype and runtime results

Tested 2026-10-15 with v1.1.0.6142 (`POST /queue`, `GET /setlist`, `tools/encore-queue.ps1`):

- **Detection** works as designed: one library class, one setlist triple at static offsets
  `0x20/0x28/0x30`, `SetlistMenu.Add` = `0x13deb0` (8 vs 1 vs 0 outside callers), `isLastSong`
  = EndOfSong `+0xa1` (first own bool that is not a backing field).
- **All songs:** `+0x00` and `+0x38` hold the full library (1649), `+0xb8` 1541, `+0x10` is empty.
  Encore looks songs up in the largest list.
- **The string list** holds `""` for a song added with the game's button; the speed was 100.
  `GlobalVariables.originalSongSpeed` is 0 until a song has been played, so Encore falls back to
  100 then.
- **Song list:** queueing through `SetlistMenu.Add` showed the song in the setlist panel
  immediately (Shadow Moses after Biology, added by the user).
- **Running setlist:** a song appended to the three lists while the first setlist song was
  playing came up in turn after the existing setlist songs (Teardrops as song 4 of 4, confirmed
  by the user).
- Between setlist songs no scene object is active; `/queue` answers `503 loading` there.

### Open questions

- The end-screen fix (`isLastSong` reset and panel swap) is not yet tested in the game, and the
  "next song" text on that screen stays stale.
- The speed the game passes is the speed setting (settings class static `+0x10`), which Encore
  does not read yet; it uses `originalSongSpeed`, which can be stale if the speed was changed in
  the menu since the last song (or 0 → 100 before the first one).
- Online/multiplayer setlists were not tested.
- During quickplay there is still no queue; turning a quickplay song into a setlist is not covered.

## After a game update

1. Install the freshly built `version.dll`, start the game and read `encore.log`.
2. If it says `ready`, you're done.
3. **If `StartScan ambiguous`:**
   - set `ENCORE_DUMP=1` and start the game once;
   - run `tools/find-callers.ps1` with the candidate RVAs from the log and the new
     `encore_methods.tsv`;
   - decompile the candidates and callers with `tools/make-slice.ps1` + REA, and see what changed
     (step 5 above is the reference).
4. **If `resolve … FAILED`:** check in the dump whether `SongScan`/`SongSelect` or one of the
   fields was renamed.

Keep dumps, slices and decompiled output local. They are derived from the game's code.
