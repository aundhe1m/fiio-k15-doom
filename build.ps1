<#
.SYNOPSIS
    DOOM Firmware Builder for the FiiO K15 (Windows / PowerShell; Linux and macOS: build.sh).

.DESCRIPTION
    Takes the official FiiO K15 v159 update you downloaded yourself and writes
    out\K15_update_<date>_<time>_v<N>.zip (UTC build time, default N=160): the same firmware
    plus DOOM and a DOOM boot animation. Nothing from FiiO and no WAD is stored in this
    repository; the builder patches your copy.

    Needs only Docker Desktop (Linux containers, the default). Everything else runs in the doomfw-builder image (docker\Dockerfile), built on the
    first run (~10 minutes), which also needs internet for the pinned engine sources.

.EXAMPLE
    .\build.ps1 $HOME\Downloads\K15_update_20261012_v159.zip

.EXAMPLE
    .\build.ps1 K15_update_20261012_v159.zip -Animation hellfire

.EXAMPLE
    .\build.ps1 -EngineOnly
#>
# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding(DefaultParameterSetName = 'Firmware')]
param(
    # The official K15_update_YYYYMMDD_v159.zip.
    [Parameter(ParameterSetName = 'Firmware', Mandatory = $true, Position = 0)]
    [string]$Zip,

    # Date in the output name, YYYYMMDD (default: today, UTC).
    [Parameter(ParameterSetName = 'Firmware')]
    [string]$Date,

    # Time in the output name, HHMM (default: now, UTC).
    [Parameter(ParameterSetName = 'Firmware')]
    [string]$Time,

    # Package version, must be > 159 (default 160).
    [Parameter(ParameterSetName = 'Firmware')]
    [int]$Version,

    # Boot animation.
    [Parameter(ParameterSetName = 'Firmware')]
    [ValidateSet('melt', 'hellfire', 'demons', 'terminal', 'stock')]
    [string]$Animation,

    # Dev: only build build\out\doomk15 and k15-mqsend.
    [Parameter(ParameterSetName = 'Engine', Mandatory = $true)]
    [switch]$EngineOnly
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$Here = $PSScriptRoot
$Image = 'doomfw-builder'

# Native commands that write to stderr must not trip $ErrorActionPreference = 'Stop'
# (Windows PowerShell 5.1 turns redirected stderr lines into errors).
function Invoke-Quiet {
    param([string]$Exe, [string[]]$Arguments)
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>$null
        return [pscustomobject]@{ Code = $LASTEXITCODE; Output = $out }
    } finally {
        $ErrorActionPreference = $saved
    }
}

function Invoke-Checked {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw 'docker not found: install Docker Desktop (https://www.docker.com/products/docker-desktop/)'
}
if ((Invoke-Quiet docker @('info')).Code -ne 0) {
    throw "the Docker daemon isn't running: start Docker Desktop and try again"
}

$containerArgs = @()
$mounts = @('--mount', "type=bind,source=$Here,target=/w")
if ($EngineOnly) {
    $containerArgs += '--engine-only'
} else {
    $zipItem = Get-Item -LiteralPath $Zip
    $mounts += @('--mount', "type=bind,source=$($zipItem.DirectoryName),target=/in,readonly")
    $containerArgs += "/in/$($zipItem.Name)"
    if ($PSBoundParameters.ContainsKey('Date')) { $containerArgs += @('--date', $Date) }
    if ($PSBoundParameters.ContainsKey('Time')) { $containerArgs += @('--time', $Time) }
    if ($PSBoundParameters.ContainsKey('Version')) { $containerArgs += @('--version', "$Version") }
    if ($PSBoundParameters.ContainsKey('Animation')) { $containerArgs += @('--animation', $Animation) }
}

# Rebuild the image when docker\Dockerfile's doomfw.builder.rev label changed.
$rev = (Select-String -LiteralPath (Join-Path $Here 'docker/Dockerfile') `
        -Pattern '^LABEL doomfw\.builder\.rev="(.*)"').Matches[0].Groups[1].Value
$have = $null
$inspect = Invoke-Quiet docker @('image', 'inspect', '--format', '{{json .Config.Labels}}', $Image)
if ($inspect.Code -eq 0 -and $inspect.Output) {
    $labels = ($inspect.Output | Out-String) | ConvertFrom-Json
    if ($labels -and $labels.PSObject.Properties['doomfw.builder.rev']) {
        $have = $labels.'doomfw.builder.rev'
    }
}
if ($have -ne $rev) {
    Write-Host "### building the $Image image (rev $rev; the first time takes ~10 minutes)"
    Invoke-Checked docker @('build', '-t', $Image, (Join-Path $Here 'docker'))
}

# The version comes from the VERSION file; $env:DOOMFW_VERSION, if set, overrides it.
$envArgs = @()
if ($env:DOOMFW_VERSION) { $envArgs += @('-e', "DOOMFW_VERSION=$env:DOOMFW_VERSION") }
# Docker Desktop on Windows needs no ownership fix; pwsh on Linux/macOS does.
if ((Get-Variable IsWindows -ErrorAction SilentlyContinue) -and -not $IsWindows) {
    $envArgs += @('-e', "HOST_UID=$(id -u)", '-e', "HOST_GID=$(id -g)")
}

Invoke-Checked docker (@('run', '--rm') + $envArgs + $mounts +
    @($Image, 'sh', '/w/tools/build-all.sh') + $containerArgs)
