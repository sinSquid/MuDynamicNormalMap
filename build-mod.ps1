[CmdletBinding()]
param(
    [string]$OutputModDirectory = 'E:\iniRePather-Skyrim\Mod Organizer 2\mods\【人物美化】动态法线 Mu Dynamic NormalMap-2.1.13-自编译',
    [string]$RuntimeSourceDirectory = '',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$muOutput = [IO.Path]::GetFullPath($OutputModDirectory)
$muRequired = @(
    'SKSE\Plugins\MuDynamicNormalMap.ini',
    'Scripts\MuDynamicNormalMap.pex',
    'SKSE\Plugins\MuDynamicNormalMap\BeastRaces.ini',
    'SKSE\Plugins\MuDynamicNormalMap\ElderRaces.ini',
    'SKSE\Plugins\MuDynamicNormalMap\FemaleHuman.ini',
    'SKSE\Plugins\MuDynamicNormalMap\MaleHuman.ini'
)
$muShaders = @('UpdateNormalMap', 'MergeTexture', 'BleedTexture', 'GenerateMips')

# A plugin DLL alone is not an independently usable Mod. Refuse incomplete packages.
foreach ($muRelative in $muRequired) {
    if (Test-Path -LiteralPath (Join-Path $muOutput $muRelative) -PathType Leaf) { continue }
    if (!$RuntimeSourceDirectory -or !(Test-Path -LiteralPath (Join-Path $RuntimeSourceDirectory $muRelative) -PathType Leaf)) {
        throw "Missing runtime asset: $muRelative. Supply -RuntimeSourceDirectory with an existing complete Mu Mod."
    }
}
$muTextureRoot = Join-Path $muOutput 'Textures'
if (!(Test-Path -LiteralPath $muTextureRoot -PathType Container) -and
    (!$RuntimeSourceDirectory -or !(Test-Path -LiteralPath (Join-Path $RuntimeSourceDirectory 'Textures') -PathType Container))) {
    throw 'Missing companion textures. Supply -RuntimeSourceDirectory with a complete Mu Mod.'
}

if (!$SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-local.cmd')
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE); output was not updated." }
}
$muBinaryDirectory = Join-Path $PSScriptRoot 'contrib\Distribution\PluginReleaseNG'
$muArtifacts = @{}
foreach ($muName in @('MuDynamicNormalMap.dll', 'MuDynamicNormalMap.pdb')) {
    $muArtifacts["SKSE\Plugins\$muName"] = Join-Path $muBinaryDirectory $muName
}
foreach ($muShader in $muShaders) {
    $muArtifacts["SKSE\Plugins\MuDynamicNormalMap\Shader\$muShader.hlsl"] = Join-Path $PSScriptRoot "shader\$muShader.hlsl"
    $muArtifacts["SKSE\Plugins\MuDynamicNormalMap\Shader\Compiled\CS_$muShader.cso"] = Join-Path $PSScriptRoot "shader\Compiled\CS_$muShader.cso"
}
foreach ($muArtifact in $muArtifacts.Values) {
    if (!(Test-Path -LiteralPath $muArtifact -PathType Leaf)) { throw "Missing build artifact: $muArtifact" }
}

# Seed missing assets only. Preserve edited INI files and other existing runtime resources.
if ($RuntimeSourceDirectory) {
    $muRuntimeRoot = [IO.Path]::GetFullPath($RuntimeSourceDirectory)
    foreach ($muFolder in @('Scripts', 'Textures', 'SKSE\Plugins\MuDynamicNormalMap')) {
        $muSourceRoot = Join-Path $muRuntimeRoot $muFolder
        if (!(Test-Path -LiteralPath $muSourceRoot -PathType Container)) { continue }
        foreach ($muFile in Get-ChildItem -LiteralPath $muSourceRoot -Recurse -File) {
            $muRelative = $muFile.FullName.Substring($muRuntimeRoot.TrimEnd('\').Length + 1)
            if ($muRelative -match '(?i)(^|\\)DiskCache(\\|$)' -or $muFile.Extension -in @('.log', '.bak')) { continue }
            $muDestination = Join-Path $muOutput $muRelative
            if (!(Test-Path -LiteralPath $muDestination)) {
                New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($muDestination)) -Force | Out-Null
                Copy-Item -LiteralPath $muFile.FullName -Destination $muDestination
            }
        }
    }
    $muConfig = 'SKSE\Plugins\MuDynamicNormalMap.ini'
    if (!(Test-Path -LiteralPath (Join-Path $muOutput $muConfig))) {
        New-Item -ItemType Directory -Path (Join-Path $muOutput 'SKSE\Plugins') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $muRuntimeRoot $muConfig) -Destination (Join-Path $muOutput $muConfig)
    }
}

$muBackup = Join-Path ([IO.Path]::GetDirectoryName($muOutput)) ('..\backups\MuBuild-' + [Guid]::NewGuid().ToString('N'))
foreach ($muRelative in $muArtifacts.Keys) {
    $muSource = $muArtifacts[$muRelative]
    $muDestination = Join-Path $muOutput $muRelative
    $muDigest = (Get-FileHash -LiteralPath $muSource -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $muDestination -PathType Leaf) {
        if ((Get-FileHash -LiteralPath $muDestination -Algorithm SHA256).Hash -eq $muDigest) { continue }
        $muOld = Join-Path $muBackup $muRelative
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($muOld)) -Force | Out-Null
        Copy-Item -LiteralPath $muDestination -Destination $muOld
    }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($muDestination)) -Force | Out-Null
    Copy-Item -LiteralPath $muSource -Destination $muDestination -Force
    if ((Get-FileHash -LiteralPath $muDestination -Algorithm SHA256).Hash -ne $muDigest) { throw "Output hash mismatch: $muRelative" }
}
foreach ($muRelative in $muRequired + @($muArtifacts.Keys)) {
    if (!(Test-Path -LiteralPath (Join-Path $muOutput $muRelative) -PathType Leaf)) { throw "Incomplete output: $muRelative" }
}
Write-Host "Verified complete Mod output: $muOutput"
