# Dev loop: close all FL instances, rebuild flim, relaunch leader "A" and member "B", both
# auto-connected (skips the Connect button). Server must be running: default ws://localhost:8080,
# or pass -Url / -Password (e.g. -Url wss://my-flim.fly.dev -Password ...).
#   -Minimized   start both FLs minimized (they don't take over the screen)
#   -NoLocalB    B ignores local copies of samples (FLIM_SAMPLE_NOLOCAL): tests sample transfer on one PC
param([string]$Url = "localhost:8080", [string]$Password = "", [switch]$Minimized, [switch]$NoLocalB)
Stop-Process -Name FL64 -Force -ErrorAction SilentlyContinue
Start-Sleep 2
$r = cmd /c "`"$PSScriptRoot\build.bat`" 2>&1"
if ($LASTEXITCODE -ne 0) { $r; exit 1 }
Remove-Item "$env:TEMP\flim_engine.log" -ErrorAction SilentlyContinue
$exe = "$PSScriptRoot\out\flim.exe"
$style = if ($Minimized) { "Minimized" } else { "Normal" }
$env:FLIM_AUTOCONNECT = $Url; $env:FLIM_PASSWORD = $Password
# each its own shared folder (one PC: the real one would be the same folder for both)
$env:FLIM_SHARED_DIR = "$env:TEMP\flim_shared_A"
$env:FLIM_NAME = "A"; Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle $style
Start-Sleep 12
if ($NoLocalB) { $env:FLIM_SAMPLE_NOLOCAL = "1" }
$env:FLIM_SHARED_DIR = "$env:TEMP\flim_shared_B"
$env:FLIM_NAME = "B"; Start-Process $exe -WorkingDirectory (Split-Path $exe) -WindowStyle $style
"build ok, launched A and B -> $Url"
