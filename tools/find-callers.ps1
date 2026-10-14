<#
.SYNOPSIS
  Lists every `call rel32` / `jmp rel32` in a PE's executable sections that targets one of the given RVAs.

.DESCRIPTION
  This is how the real SongScan.StartScan was told apart from its obfuscator decoys: only the real one
  is called from other classes. Attribute each call site to a method with the address map that
  Encore writes when ENCORE_DUMP=1 (encore_methods.tsv): the containing method is the closest entry
  at or below the site.

  This is a plain byte scan, so a hit can in rare cases be data that looks like an instruction.
  Encore itself additionally filters sites through the x64 unwind tables.

.EXAMPLE
  .\find-callers.ps1 -Path "C:\...\Clone Hero\GameAssembly.dll" -Rva 0x1b4fd0,0x1b57d0,0x1b5910,0x1b5c10 `
      -MethodMap "C:\...\Clone Hero\encore_methods.tsv"
#>
param(
    [Parameter(Mandatory)] [string]$Path,
    [Parameter(Mandatory)] [long[]]$Rva,
    [string]$MethodMap
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\pe.ps1"

Add-Type -TypeDefinition @"
using System; using System.Collections.Generic;
public static class RelScan {
    // Returns {siteRva, targetRva, isCall} triples for rel32 call/jmp hitting a target.
    public static List<long[]> Scan(byte[] b, long fileOff, long size, long sectionRva, long[] targets) {
        var set = new HashSet<long>(targets); var res = new List<long[]>();
        for (long i = 0; i + 5 <= size; i++) {
            byte op = b[fileOff + i];
            if (op != 0xE8 && op != 0xE9) continue;
            long site = sectionRva + i;
            long tgt = site + 5 + BitConverter.ToInt32(b, (int)(fileOff + i + 1));
            if (set.Contains(tgt)) res.Add(new long[] { site, tgt, op == 0xE8 ? 1 : 0 });
        }
        return res;
    }
}
"@

$bytes = [IO.File]::ReadAllBytes($Path)
$layout = Read-PeLayout $bytes
$hits = @()
foreach ($s in $layout.Sections | Where-Object { $_.Characteristics -band 0x20000000 }) {   # IMAGE_SCN_MEM_EXECUTE
    $size = [Math]::Min($s.VirtualSize, $s.RawSize)
    $hits += [RelScan]::Scan($bytes, $s.RawOffset, $size, $s.VirtualAddress, $Rva)
}

$methods = $null
if ($MethodMap) {
    $methods = Get-Content $MethodMap -Encoding UTF8 | ForEach-Object {
        $p = $_.Split("`t")
        [pscustomobject]@{ Rva = [Convert]::ToInt64($p[0], 16); Class = $p[1]; Method = $p[2] }
    } | Where-Object Rva -gt 0 | Sort-Object Rva
}

foreach ($h in $hits) {
    $line = '{0} at 0x{1:x} -> 0x{2:x}' -f ($(if ($h[2]) { 'call' } else { 'jmp ' })), $h[0], $h[1]
    if ($methods) {
        $owner = $methods | Where-Object Rva -le $h[0] | Select-Object -Last 1
        if ($owner) { $line += '   in {0}::{1} (0x{2:x})' -f $owner.Class, $owner.Method, $owner.Rva }
    }
    $line
}
foreach ($t in $Rva) {
    '0x{0:x}: {1} caller(s)' -f $t, @($hits | Where-Object { $_[1] -eq $t }).Count
}
