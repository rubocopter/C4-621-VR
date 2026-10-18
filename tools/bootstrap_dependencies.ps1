# Installs exact upstream dependencies outside the tracked source tree.
# Does not modify the game or launch any probes.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$reference = Join-Path $root 'reference'
New-Item -ItemType Directory -Path $reference -Force | Out-Null

function Invoke-GitChecked {
    param([string[]]$GitArgs)
    & git @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed ($LASTEXITCODE)" }
}
function Ensure-PinnedCheckout {
    param([string]$Directory, [string]$Url, [string]$Tag, [string]$Commit)
    if (Test-Path -LiteralPath $Directory) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory '.git'))) {
            throw "Unmanaged dependency directory exists: $Directory. Back it up or manage it manually."
        }
    } else {
        Invoke-GitChecked -GitArgs @('clone', '--depth', '1', '--branch', $Tag, $Url, $Directory)
    }
    $actual = (& git -C $Directory rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Commit) {
        throw "Unexpected dependency revision in $Directory (expected $Commit; got $actual). No checkout was changed."
    }
}
Ensure-PinnedCheckout -Directory (Join-Path $reference 'minhook') `
    -Url 'https://github.com/TsudaKageyu/minhook.git' -Tag 'v1.3.4' `
    -Commit 'c3fcafdc10146beb5919319d0683e44e3c30d537'
Ensure-PinnedCheckout -Directory (Join-Path $reference 'openxr-sdk') `
    -Url 'https://github.com/KhronosGroup/OpenXR-SDK.git' -Tag 'release-1.1.63' `
    -Commit 'f2448a8797c85814aa892efc1ab8707900fbcc78'
$headersSource = Join-Path $reference 'openxr-sdk/include/openxr'
$headersDestination = Join-Path $reference 'openxr'
if (-not (Test-Path -LiteralPath $headersDestination)) {
    Copy-Item -LiteralPath $headersSource -Destination $headersDestination -Recurse
} else {
    # Existing developer copies may have been used for earlier experiments.
    # Never overwrite them without an explicit local decision.
    Write-Warning 'reference/openxr already exists; leaving existing local headers unchanged.'
}
foreach ($header in @('openxr.h', 'openxr_platform.h', 'openxr_platform_defines.h')) {
    if (-not (Test-Path -LiteralPath (Join-Path $headersDestination $header))) {
        throw "Missing OpenXR header $header in $headersDestination"
    }
}
Write-Host 'Dependencies available under reference/. Nothing was installed into the game.'
