<#
.SYNOPSIS
  Lists RIP-relative references (x64 `[rip+disp32]` operands) to given RVAs, e.g. the global that holds
  an IL2CPP class pointer, and attributes each site to the containing method.

.DESCRIPTION
  IL2CPP code reaches a class's static fields through a global holding the Il2CppClass pointer
  (decompiled as DAT_183c9d098 etc.). Finding every function that loads that global answers "who
  uses this class's statics", which find-callers.ps1 cannot (it only follows call/jmp).

  Heuristic: for every byte position p in executable sections, the 4 bytes at p are taken as a
  disp32 and the reference is reported if p + 4 + disp32 equals a target. That is exact for operands
  where the displacement is the last field of the instruction (loads/stores/lea without immediate),
  which covers how IL2CPP accesses class globals; it can produce rare false positives on random
  bytes. Use -Range to limit the scan to the functions of interest.

.EXAMPLE
  .\find-refs.ps1 -Path "C:\...\GameAssembly.dll" -Rva 0x3c9d098 -MethodMap pick_methods.tsv -Range 15d000-164000
#>
param(
    [Parameter(Mandatory)] [string]$Path,
    [Parameter(Mandatory)] [long[]]$Rva,
    [string]$MethodMap,
    [string[]]$Range        # optional hex RVA ranges "start-end" to restrict the scanned code
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\pe.ps1"

Add-Type -TypeDefinition @"
using System; using System.Collections.Generic;
public static class RipScan {
    public static List<long[]> Scan(byte[] b, long fileOff, long size, long sectionRva, long[] targets, long lo, long hi) {
        var set = new HashSet<long>(targets); var res = new List<long[]>();
        for (long i = 0; i + 4 <= size; i++) {
            long site = sectionRva + i;
            if (site < lo || site >= hi) continue;
            long tgt = site + 4 + BitConverter.ToInt32(b, (int)(fileOff + i));
            if (set.Contains(tgt)) res.Add(new long[] { site, tgt });
        }
        return res;
    }
}
"@

$bytes = [IO.File]::ReadAllBytes($Path)
$layout = Read-PeLayout $bytes
$windows = New-Object 'System.Collections.Generic.List[long[]]'
if ($Range) {
    foreach ($r in $Range) { $p = $r.Split('-'); $windows.Add([long[]]@([Convert]::ToInt64($p[0], 16), [Convert]::ToInt64($p[1], 16))) }
} else { $windows.Add([long[]]@(0, [long]::MaxValue)) }

$hits = @()
foreach ($s in $layout.Sections | Where-Object { $_.Characteristics -band 0x20000000 }) {   # IMAGE_SCN_MEM_EXECUTE
    $size = [Math]::Min($s.VirtualSize, $s.RawSize)
    foreach ($w in $windows) { $hits += [RipScan]::Scan($bytes, $s.RawOffset, $size, $s.VirtualAddress, $Rva, $w[0], $w[1]) }
}

$methods = $null
if ($MethodMap) {
    $methods = Get-Content $MethodMap -Encoding UTF8 | ForEach-Object {
        $p = $_.Split("`t")
        [pscustomobject]@{ Rva = [Convert]::ToInt64($p[0], 16); Class = $p[1]; Method = $p[2] }
    } | Where-Object Rva -gt 0 | Sort-Object Rva
}

# One line per referencing method (sites of the same method are grouped).
$hits | ForEach-Object {
    $owner = if ($methods) { $methods | Where-Object Rva -le $_[0] | Select-Object -Last 1 }
    [pscustomobject]@{
        Target = '0x{0:x}' -f $_[1]
        Method = if ($owner) { '{0}::{1} (0x{2:x})' -f $owner.Class, $owner.Method, $owner.Rva } else { '?' }
        Site   = '0x{0:x}' -f $_[0]
    }
} | Group-Object Target, Method | ForEach-Object {
    '{0}  {1}  sites: {2}' -f $_.Group[0].Target, $_.Group[0].Method, (($_.Group | ForEach-Object Site) -join ',')
}
