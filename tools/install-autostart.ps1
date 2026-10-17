# Starts DeskDial at every logon.
#
# The hardware module needs administrator rights for temperatures and power,
# so by default DeskDial runs from Task Scheduler with the highest privileges
# (no UAC prompt each time). A media-only build doesn't need them: pass
# -NoElevation to run it with normal rights.
#
# From an elevated PowerShell ("Run as administrator"), in the repository folder:
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Port COM3
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -NoElevation
#   powershell -ExecutionPolicy Bypass -File .\tools\install-autostart.ps1 -Remove
#
# The published program is copied to %LOCALAPPDATA%\Programs\DeskDial, so
# rebuilding never fights with the running copy. Run the script again after
# each rebuild to update the installed copy.
# SPDX-License-Identifier: MIT

param(
    [string]$Source = (Join-Path $PSScriptRoot '..\host\bin\Release\net8.0-windows10.0.19041.0\win-x64\publish'),
    [string]$Port = '',
    [switch]$NoElevation,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
$TaskName = 'DeskDial'
$Target = Join-Path $env:LOCALAPPDATA 'Programs\DeskDial'

$me = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]$me).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host 'Please run this from an elevated PowerShell (right-click > Run as administrator).' -ForegroundColor Yellow
    exit 1
}

# Stop a running copy before replacing or removing it
Stop-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
Get-Process DeskDial -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

if ($Remove) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "Autostart removed. The program files are still in $Target."
    exit 0
}

if (-not (Test-Path (Join-Path $Source 'DeskDial.exe'))) {
    Write-Host "DeskDial.exe not found in $Source" -ForegroundColor Yellow
    Write-Host 'Build it first:  cd host ;  dotnet publish -c Release'
    exit 1
}

New-Item -ItemType Directory -Force -Path $Target | Out-Null
Copy-Item -Path (Join-Path $Source '*') -Destination $Target -Recurse -Force
$exe = Join-Path $Target 'DeskDial.exe'

$actionArgs = @{ Execute = $exe; WorkingDirectory = $Target }
if ($Port) { $actionArgs.Argument = "--port $Port" }
$action = New-ScheduledTaskAction @actionArgs
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $me.Name
$runLevel = if ($NoElevation) { 'Limited' } else { 'Highest' }
$principal = New-ScheduledTaskPrincipal -UserId $me.Name -LogonType Interactive -RunLevel $runLevel
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew `
    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal `
    -Settings $settings -Description 'DeskDial: feeds the round display over USB (now playing, hardware stats).' -Force | Out-Null
Start-ScheduledTask -TaskName $TaskName

Write-Host "Installed to $Target and started." -ForegroundColor Green
if ($NoElevation) {
    Write-Host 'It will start automatically at every logon, with normal rights.'
} else {
    Write-Host 'It will start automatically at every logon, with administrator rights.'
}
