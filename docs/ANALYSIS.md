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
