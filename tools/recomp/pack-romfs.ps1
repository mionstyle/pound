# Packs RomFS folders into zip parts of at most ~340 MB each (the transfer limit is 400 MB).
#
#   powershell -ExecutionPolicy Bypass -File pack-romfs.ps1 -Romfs <romfs dir> -Out <dir> -Name <prefix> <folder> [<folder> ...]
#
# Example:
#   pack-romfs.ps1 -Romfs C:\dump\updates\458752\romfs -Out C:\dump -Name lv Phive\Shape Voxel\Const

param(
    [Parameter(Mandatory = $true)][string]$Romfs,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Folders
)

$limit = 340MB
Set-Location $Romfs
$root  = (Get-Location).Path
$part  = 0
$size  = 0
$files = New-Object System.Collections.Generic.List[string]

function Flush-Part {
    if ($files.Count -eq 0) { return }
    $script:part++
    $list = Join-Path $Out ("lista-{0}{1}.txt" -f $Name, $script:part)
    $zip  = Join-Path $Out ("romfs-{0}{1}.zip" -f $Name, $script:part)
    $files | Set-Content $list -Encoding ascii
    if (Test-Path $zip) { Remove-Item $zip }
    tar -a -cf $zip -T $list
    Write-Host ("feito {0}{1}  ({2} ficheiros)" -f $Name, $script:part, $files.Count)
    $files.Clear()
    $script:size = 0
}

foreach ($f in $Folders) {
    Get-ChildItem -Path $f -Recurse -File | ForEach-Object {
        if ($size + $_.Length -gt $limit) { Flush-Part }
        $files.Add($_.FullName.Substring($root.Length + 1).Replace('\', '/'))
        $script:size += $_.Length
    }
}
Flush-Part
Write-Host ("pronto: {0} partes" -f $part)
