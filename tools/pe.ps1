# Shared PE helpers for the analysis tools (dot-source this file).

function Read-PeLayout([byte[]]$Bytes) {
    $peOff = [BitConverter]::ToInt32($Bytes, 0x3C)
    if ([BitConverter]::ToUInt32($Bytes, $peOff) -ne 0x4550) { throw 'not a PE file' }
    $sectionCount = [BitConverter]::ToUInt16($Bytes, $peOff + 6)
    $optSize = [BitConverter]::ToUInt16($Bytes, $peOff + 20)
    $opt = $peOff + 24
    $sections = @()
    $sh = $opt + $optSize
    for ($i = 0; $i -lt $sectionCount; $i++) {
        $sh = $opt + $optSize + 40 * $i
        $sections += [pscustomobject]@{
            Name            = [Text.Encoding]::ASCII.GetString($Bytes, $sh, 8).TrimEnd([char]0)
            VirtualSize     = [BitConverter]::ToUInt32($Bytes, $sh + 8)
            VirtualAddress  = [BitConverter]::ToUInt32($Bytes, $sh + 12)
            RawSize         = [BitConverter]::ToUInt32($Bytes, $sh + 16)
            RawOffset       = [BitConverter]::ToUInt32($Bytes, $sh + 20)
            Characteristics = [BitConverter]::ToUInt32($Bytes, $sh + 36)
        }
    }
    [pscustomobject]@{
        ImageBase = [BitConverter]::ToUInt64($Bytes, $opt + 24)
        Sections  = $sections
    }
}

# File offset of an RVA, or -1 if it is not backed by file data.
function Get-FileOffset($Layout, [long]$Rva) {
    foreach ($s in $Layout.Sections) {
        if ($Rva -ge $s.VirtualAddress -and $Rva -lt $s.VirtualAddress + $s.RawSize) {
            return [long]($Rva - $s.VirtualAddress + $s.RawOffset)
        }
    }
    return -1
}
