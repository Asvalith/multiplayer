param(
    [string]$ProjectRoot = (Split-Path -Parent $PSScriptRoot),
    [switch]$PlanOnly,
    [switch]$FullMage
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$contentRoot = [IO.Path]::GetFullPath((Join-Path $ProjectRoot 'Content'))

# 只导入本次使用的动画及水/毒特效；保留原始包路径，不覆盖任何已有资产。
function Import-SelectedEntries([string]$ArchivePath, [scriptblock]$SelectEntry, [string]$Prefix) {
    $archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
    $count = 0
    $selected = 0
    try {
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.EndsWith('/') -or -not (& $SelectEntry $entry.FullName)) { continue }
            ++$selected
            $relative = $entry.FullName.Substring($Prefix.Length)
            $destination = [IO.Path]::GetFullPath((Join-Path $contentRoot $relative))
            if (-not $destination.StartsWith($contentRoot + [IO.Path]::DirectorySeparatorChar)) {
                throw "Archive entry is outside Content: $($entry.FullName)"
            }
            if (Test-Path -LiteralPath $destination) {
                if ((Get-Item -LiteralPath $destination).Length -ne $entry.Length) {
                    throw "Existing asset differs; refusing overwrite: $destination"
                }
                continue
            }
            if ($PlanOnly) { ++$count; continue }
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
            ++$count
        }
    } finally { $archive.Dispose() }
    if ($PlanOnly) {
        Write-Output "Selected $selected files; would import $count new files from $([IO.Path]::GetFileName($ArchivePath))"
    } else {
        Write-Output "Selected $selected files; imported $count new files from $([IO.Path]::GetFileName($ArchivePath))"
    }
}

$mage = Join-Path $contentRoot 'Mage Animation Pack/Mage Animation Pack 5.4/Mage.zip'
# 完整补齐原 Mage 包供编辑器挑选和连线；保留原包路径及已有文件，不修改 Combat 表现资产。
# 此模式不连带重新导入特效，也不调用动画生成、重定向或角色替换工具。
if ($FullMage) {
    Import-SelectedEntries $mage {
        param($name)
        $name.StartsWith('Mage/') -and $name -match '\.(uasset|umap|uexp|ubulk|uptnl)$'
    } ''
    return
}

Import-SelectedEntries $mage {
    param($name)
    $name.StartsWith('Mage/Demo/Mannequins/') -or
    $name -match '^Mage/Animations/Sequence2/02_Attack/(06_Attack/AS_Attack_01_02_Seq|07_Skill_Attack/AS_Skill_01_01_(Start|Loop|End)_Seq|09_Buff/AS_Buff_Seq)\.uasset$' -or
    $name -match '^Mage/Animations/Sequence2/08_Hit/(01_Hit/AS_Hit_(F|Death)_Seq|14_Block/AS_Block_Loop_Seq)\.uasset$'
} ''

$effects = @(Get-ChildItem -LiteralPath (Join-Path $contentRoot 'nigara') -Filter '*.zip' -File)
if ($effects.Count -ne 1) { throw 'Expected exactly one source Niagara archive.' }
# 清单来自七个已选系统的递归依赖，只导入这些包及同包附属文件，避免重跑时恢复未用特效。
$effectEntries = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($package in (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'CombatEffectImports.json') -Raw | ConvertFrom-Json)) {
    $effectEntries.Add('Content/' + $package.Substring('/Game/'.Length)) | Out-Null
}
Import-SelectedEntries $effects[0].FullName {
    param($name)
    $name -match '^(?<package>.+)\.(uasset|uexp|ubulk|uptnl)$' -and
    $effectEntries.Contains($Matches.package)
} 'Content/'
