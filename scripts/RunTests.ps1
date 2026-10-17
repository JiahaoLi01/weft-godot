<#
.SYNOPSIS
Runs the extension's tests (tests/test_runner.gd) in Godot on each graphics driver, with the API's validation layers.

.DESCRIPTION
The test project (tests/) and the addon with its built DLL (scripts/BuildWeft.ps1 first) are copied into a folder of
their own (-ProjectDir, by default %TEMP%/weft-godot-tests), and Godot runs the tests there in a small window, once a
driver (--rendering-driver vulkan, d3d12) and render thread mode (--render-thread safe, the default, where the main
thread renders; separate, Godot's experimental render thread) with --gpu-validation: the Vulkan SDK's validation layer (with its
synchronization checks), the Direct3D 12 debug layer (Windows' Graphics Tools). A run fails when a test check fails, when Godot prints an error (a validation
layer's among them), or when it takes more than -Timeout seconds (the test process is ended then). Logs are in
build/tests/<driver>-<mode>.log.

.EXAMPLE
powershell -File scripts/RunTests.ps1 -Godot "D:/SteamLibrary/steamapps/common/Godot Engine/godot.windows.opt.tools.64.exe"
#>
param(
    [string]$Godot = $(if ($env:GODOT) { $env:GODOT } else { "D:/SteamLibrary/steamapps/common/Godot Engine/godot.windows.opt.tools.64.exe" }),
    [string[]]$Drivers = @("vulkan", "d3d12"),
    [ValidateSet("safe", "separate")]
    [string[]]$RenderThreads = @("safe", "separate"),
    [string]$ProjectDir = (Join-Path $env:TEMP "weft-godot-tests"),
    [switch]$NoValidation,
    [int]$Timeout = 180
)
$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path $Godot)) {
    throw "no Godot at $Godot (-Godot or GODOT)"
}
$dll = Join-Path $repository "addons/weft/bin/weft_godot.windows.x86_64.dll"
if (-not (Test-Path $dll)) {
    throw "no ${dll}: run scripts/BuildWeft.ps1 first"
}

# The project: the tests, the addon, and the list of extensions Godot loads (the editor would write it on import).
if (Test-Path $ProjectDir) {
    Remove-Item -Recurse -Force $ProjectDir
}
New-Item -ItemType Directory -Force (Join-Path $ProjectDir "addons/weft") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $ProjectDir ".godot") | Out-Null
Copy-Item (Join-Path $repository "tests/*") $ProjectDir -Recurse -Force
Copy-Item (Join-Path $repository "addons/weft/*") (Join-Path $ProjectDir "addons/weft") -Recurse -Force
[System.IO.File]::WriteAllText((Join-Path $ProjectDir ".godot/extension_list.cfg"), "res://addons/weft/weft.gdextension`n",
    (New-Object System.Text.UTF8Encoding $false))

# The Vulkan validation layer's synchronization checks too: the hazards between weft's submissions and Godot's frames.
if (-not $NoValidation) {
    $env:VK_KHRONOS_VALIDATION_VALIDATE_SYNC = "true"
}
$logs = Join-Path $repository "build/tests"
New-Item -ItemType Directory -Force $logs | Out-Null
$failed = @()
$runs = foreach ($driver in $Drivers) { foreach ($thread in $RenderThreads) { [pscustomobject]@{Driver = $driver; Thread = $thread} } }
foreach ($run in $runs) {
    $driver = $run.Driver
    $name = "$driver-$($run.Thread)"
    $log = Join-Path $logs "$name.log"
    $arguments = @("--path", "`"$ProjectDir`"", "--rendering-driver", $driver, "--render-thread", $run.Thread, "--windowed",
        "--resolution", "320x240", "--log-file", "`"$log`"", "-s", "res://test_runner.gd")
    if (-not $NoValidation) {
        $arguments = @("--gpu-validation") + $arguments
    }
    Write-Host "weft-godot: testing on $driver, render thread $($run.Thread)"
    $process = Start-Process -FilePath $Godot -ArgumentList $arguments -PassThru -WindowStyle Normal `
        -RedirectStandardOutput (Join-Path $logs "$name.stdout.txt") -RedirectStandardError (Join-Path $logs "$name.stderr.txt")
    # Windows PowerShell keeps the exit code only for a process whose handle was taken while it ran.
    $null = $process.Handle
    if (-not $process.WaitForExit($Timeout * 1000)) {
        # Only the test process this script started.
        Stop-Process -Id $process.Id -Force
        Write-Host "  timed out after $Timeout s"
        $failed += $name
        continue
    }
    $text = if (Test-Path $log) { Get-Content $log -Raw -Encoding UTF8 } else { "" }
    $checks = @([regex]::Matches($text, "(?m)^FAIL .*$") | ForEach-Object { $_.Value })
    # Every error Godot prints: the validation layers' (as "ERROR: Vulkan Debug Report" or "ERROR: <category> - Message Id
    # Number"), weft's, the engine's own, and the test script's.
    $errors = @([regex]::Matches($text, "(?m)^(SCRIPT )?ERROR: .*$") | ForEach-Object { $_.Value })
    $summary = [regex]::Match($text, "weft-godot tests: \d+ failures").Value
    foreach ($line in $checks + $errors) {
        Write-Host "  $line"
    }
    if ($process.ExitCode -ne 0 -or $checks.Count -or $errors.Count -or -not $summary) {
        Write-Host "  failed (exit $($process.ExitCode)) $summary; log: $log"
        $failed += $name
    } else {
        Write-Host "  passed: $summary"
    }
}
if ($failed.Count) {
    throw "weft-godot tests failed on: $($failed -join ', ')"
}
Write-Host "weft-godot: the tests passed on $(($runs | ForEach-Object { "$($_.Driver)-$($_.Thread)" }) -join ', ')"
