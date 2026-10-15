<#
.SYNOPSIS
  Builds a small "slice" PE from selected code regions of GameAssembly.dll, for decompiling with REA/Ghidra.

.DESCRIPTION
  REA's Ghidra provider first runs a full auto-analysis, which for the 70 MB GameAssembly.dll exceeds
  its ~330 s startup deadline. REA on Windows also rejects headerless raw x86-64 dumps.

  This script copies only the requested RVA ranges (each widened to 4 KB boundaries) into a new PE
  that keeps the original image base, so every address in the decompiled output matches the real
  game. Every method from the Pick address map (pick_methods.tsv, written with PICK_DUMP=1)
  that falls inside a range becomes a named export. That gives Ghidra function starts and readable
  labels. Obfuscated names are replaced by short ids (o0, o1, ...); the mapping is written to
  <Out>.names.txt.

  Calls to code outside the slice show up as func_0x<address>; look those up in the address map.

.EXAMPLE
  .\make-slice.ps1 -Path "C:\...\Clone Hero\GameAssembly.dll" -MethodMap "C:\...\Clone Hero\pick_methods.tsv" `
      -Out "$env:TEMP\slice.dll" -Range 88000-8f000,1b4000-1bc000
#>
param(
    [Parameter(Mandatory)] [string]$Path,
    [Parameter(Mandatory)] [string]$MethodMap,
    [Parameter(Mandatory)] [string]$Out,
    [Parameter(Mandatory)] [string[]]$Range      # hex RVA ranges "start-end"
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\pe.ps1"

$src = [IO.File]::ReadAllBytes($Path)
$layout = Read-PeLayout $src
$rangeList = @()
foreach ($r in $Range) {
    $p = $r.Split('-')
    $start = [Convert]::ToInt64($p[0], 16); $end = [Convert]::ToInt64($p[1], 16)
    $rangeList += , @(($start - $start % 0x1000), ($end + (0x1000 - $end % 0x1000) % 0x1000))
}
function In-Ranges([long]$rva) { foreach ($r in $rangeList) { if ($rva -ge $r[0] -and $rva -lt $r[1]) { return $true } }; $false }

# Export names from the method map.
$obf = @{}
function Clean([string]$s) {
    if ($s -match '^[A-Za-z0-9_.<>`]+$') { return ($s -replace '[^A-Za-z0-9_]', '_') }
    if (-not $obf.ContainsKey($s)) { $obf[$s] = 'o' + $obf.Count }
    return $obf[$s]
}
$exports = @{}
foreach ($line in Get-Content $MethodMap -Encoding UTF8) {
    $p = $line.Split("`t")
    $rva = [Convert]::ToInt64($p[0], 16)
    if ($rva -eq 0 -or -not (In-Ranges $rva)) { continue }
    $cls = ($p[1].Split('/') | ForEach-Object { Clean $_ }) -join '_'
    $exports[('{0}__{1}__{2:x}' -f $cls, (Clean ($p[2] -replace '\(\d+\)$', '')), $rva)] = $rva
}
$names = [string[]]$exports.Keys
[Array]::Sort($names, [StringComparer]::Ordinal)     # export name table must be sorted

function W32([byte[]]$b, [long]$o, [long]$v) { [BitConverter]::GetBytes([uint32]$v).CopyTo($b, $o) }
function W16([byte[]]$b, [long]$o, [long]$v) { [BitConverter]::GetBytes([uint16]$v).CopyTo($b, $o) }
function Align([long]$v, [long]$a) { [long]([Math]::Ceiling($v / $a) * $a) }

# Sections: the code ranges, then the export directory behind the last original section.
$sections = @()
foreach ($r in $rangeList) {
    $data = New-Object byte[] ($r[1] - $r[0])
    for ($rva = $r[0]; $rva -lt $r[1]; $rva += 0x1000) {
        $off = Get-FileOffset $layout $rva
        if ($off -ge 0) { [Array]::Copy($src, $off, $data, $rva - $r[0], [Math]::Min(0x1000, $src.Length - $off)) }
    }
    $sections += , @('.text', $r[0], $data, 0x60000020)            # code | execute | read
}
$last = $layout.Sections[-1]
$expRva = Align ($last.VirtualAddress + $last.VirtualSize) 0x1000

$n = $names.Count
$eat = 40; $npt = $eat + 4 * $n; $ord = $npt + 4 * $n; $str = $ord + 2 * $n
$ms = New-Object IO.MemoryStream
$ms.Write((New-Object byte[] $str), 0, $str)
$dllNameOff = $ms.Position
$nameBytes = [Text.Encoding]::ASCII.GetBytes("GameAssemblySlice.dll`0"); $ms.Write($nameBytes, 0, $nameBytes.Length)
$nameOffs = foreach ($nm in $names) { $ms.Position; $b = [Text.Encoding]::ASCII.GetBytes($nm + "`0"); $ms.Write($b, 0, $b.Length) }
$exp = $ms.ToArray()
W32 $exp 12 ($expRva + $dllNameOff); W32 $exp 16 1; W32 $exp 20 $n; W32 $exp 24 $n
W32 $exp 28 ($expRva + $eat); W32 $exp 32 ($expRva + $npt); W32 $exp 36 ($expRva + $ord)
for ($i = 0; $i -lt $n; $i++) {
    W32 $exp ($eat + 4 * $i) $exports[$names[$i]]
    W32 $exp ($npt + 4 * $i) ($expRva + $nameOffs[$i])
    W16 $exp ($ord + 2 * $i) $i
}
$sections += , @('.edata', $expRva, $exp, 0x40000040)             # initialized data | read

# Headers: DOS stub, PE32+ x64 DLL, 16 data directories, section table.
$fileOff = 0x400; $placed = @()
foreach ($s in $sections) { $raw = Align $s[2].Length 0x200; $placed += , @($s[0], $s[1], $s[2], $s[3], $fileOff, $raw); $fileOff += $raw }
$img = New-Object byte[] $fileOff
W16 $img 0 0x5A4D; W32 $img 0x3C 0x40; W32 $img 0x40 0x4550
W16 $img 0x44 0x8664; W16 $img 0x46 $placed.Count; W16 $img 0x54 240; W16 $img 0x56 0x2022
$oh = 0x58
W16 $img $oh 0x20B
[BitConverter]::GetBytes([uint64]$layout.ImageBase).CopyTo($img, $oh + 24)
W32 $img ($oh + 32) 0x1000; W32 $img ($oh + 36) 0x200; W16 $img ($oh + 48) 6
W32 $img ($oh + 56) (Align ($expRva + $exp.Length) 0x1000); W32 $img ($oh + 60) 0x400
W16 $img ($oh + 68) 2; W32 $img ($oh + 108) 16
W32 $img ($oh + 112) $expRva; W32 $img ($oh + 116) $exp.Length
$sh = $oh + 240
foreach ($p in $placed) {
    [Text.Encoding]::ASCII.GetBytes($p[0]).CopyTo($img, $sh)
    W32 $img ($sh + 8) $p[2].Length; W32 $img ($sh + 12) $p[1]; W32 $img ($sh + 16) $p[5]; W32 $img ($sh + 20) $p[4]; W32 $img ($sh + 36) $p[3]
    [Array]::Copy($p[2], 0, $img, $p[4], $p[2].Length)
    $sh += 40
}
[IO.File]::WriteAllBytes($Out, $img)
$obf.GetEnumerator() | Sort-Object Value | ForEach-Object { '{0} = {1}' -f $_.Value, $_.Key } |
    Set-Content "$Out.names.txt" -Encoding UTF8
"wrote $Out with $n exports in $($placed.Count) sections"
