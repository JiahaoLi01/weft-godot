<#
.SYNOPSIS
Builds weft and the extension's native code into addons/weft/bin/weft_godot.windows.x86_64.dll.

.DESCRIPTION
Weft (https://github.com/JiahaoLi01/weft) is built from a checkout of its own (by default the folder `weft` next to
this repository, or WEFT_SOURCE_DIR) with CMake and Visual Studio 2022, together with godot-cpp (downloaded by CMake,
pinned by its commit and checksum; Python 3 generates its bindings); the Vulkan SDK's DXC is needed for weft's Vulkan
backend, which Godot draws with by default. The DLL (and its .pdb) are written into the addon, and the addon copied into
the demo project; neither is committed. Run it again after the weft checkout changes, with the Godot editors that loaded
the old DLL closed (an editor keeps it until it quits).

.EXAMPLE
powershell -File scripts/BuildWeft.ps1 -WeftSource F:/Projects/weft
#>
param(
    [string]$WeftSource = $env:WEFT_SOURCE_DIR,
    [ValidateSet("Release", "RelWithDebInfo", "Debug")]
    [string]$Config = "Release"
)
# Native tools write warnings to stderr; their exit codes say whether they failed.
$ErrorActionPreference = "Continue"
$repository = Split-Path -Parent $PSScriptRoot
if (-not $WeftSource) {
    $WeftSource = Join-Path (Split-Path -Parent $repository) "weft"
}
$WeftSource = (Resolve-Path $WeftSource -ErrorAction Stop).Path
if (-not (Test-Path (Join-Path $WeftSource "include/weft/weft.h"))) {
    throw "$WeftSource is not a weft checkout"
}
$build = Join-Path $repository "build/native"
Write-Host "weft-godot: building $WeftSource ($Config) in $build"
cmake -S (Join-Path $repository "native") -B $build -G "Visual Studio 17 2022" -A x64 "-DWEFT_SOURCE_DIR=$WeftSource"
if ($LASTEXITCODE) { throw "cmake configure failed" }
cmake --build $build --config $Config --target weft_godot --parallel
if ($LASTEXITCODE) { throw "cmake build failed" }

$bin = Join-Path $repository "addons/weft/bin"
New-Item -ItemType Directory -Force $bin | Out-Null
foreach ($file in @("weft_godot.windows.x86_64.dll", "weft_godot.windows.x86_64.pdb")) {
    $path = Join-Path $build "$Config/$file"
    if (Test-Path $path) {
        Copy-Item $path $bin -Force
    }
}
# The demo project gets a copy of the addon (not committed).
$demoAddon = Join-Path $repository "demo/addons/weft"
New-Item -ItemType Directory -Force $demoAddon | Out-Null
Copy-Item (Join-Path $repository "addons/weft/*") $demoAddon -Recurse -Force
Write-Host "weft-godot: $bin/weft_godot.windows.x86_64.dll (and a copy of the addon in demo/addons/weft)"
