# Starts NowPlayingBridge at logon with administrator rights (needed for
# temperatures, fans and power), without a UAC prompt each time.
#
# From an elevated PowerShell ("Run as administrator"), in the project folder:
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Port COM3
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Remove
#
# The published program is copied to %LOCALAPPDATA%\Programs\NowPlayingBridge,
# so rebuilding in the project folder never fights with the running copy.
# Run the script again after each rebuild to update the installed copy.

param(
    [string]$Source = (Join-Path $PSScriptRoot '..\pc\NowPlayingBridge\bin\Release\net8.0-windows10.0.19041.0\win-x64\publish'),
    [string]$Port = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$TaskName = 'NowPlayingBridge'
$Target = Join-Path $env:LOCALAPPDATA 'Programs\NowPlayingBridge'

$me = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$me).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Please run this from an elevated PowerShell (right-click > Run as administrator).' -ForegroundColor Yellow
    exit 1
}

# Stop a running copy before replacing or removing it
Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
Get-Process NowPlayingBridge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Autostart removed. The program files are still in $Target."
    exit 0
}

if (-not (Test-Path (Join-Path $Source 'NowPlayingBridge.exe'))) {
    Write-Host "NowPlayingBridge.exe not found in $Source" -ForegroundColor Yellow
    Write-Host 'Build it first:  cd pc\NowPlayingBridge ;  dotnet publish -c Release'
    exit 1
}

New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $Target -Recurse -Force
$exe = Join-Path $Target 'NowPlayingBridge.exe'

$actionArgs = @{ Execute = $exe; WorkingDirectory = $Target }
if ($Port) { $actionArgs.Argument = "--port $Port" }
$action = New-ScheduledTaskAction @actionArgs
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $me.Name
$principal = New-ScheduledTaskPrincipal -UserId $me.Name -LogonType Interactive -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'Feeds the ESP32 now-playing / hardware display over USB.' -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName

Write-Host "Installed to $Target and started." -ForegroundColor Green
Write-Host 'It will start automatically at every logon, with administrator rights.'
Write-Host 'If you added a shortcut to the Startup folder earlier, delete it.'
