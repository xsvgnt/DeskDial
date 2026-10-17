# Tells you how an ESP32-S3 board is connected over USB, which decides the
# USB settings to build with and whether the board can tell the PC is off.
# Run with the board plugged in:
#   powershell -ExecutionPolicy Bypass -File .\tools\usb-check.ps1
# SPDX-License-Identifier: MIT

$ports = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue
if (-not $ports) {
    Write-Host "No COM ports found. Is the board plugged in with a data cable (not charge-only)?"
    exit 1
}

$uart = 'USB-to-serial chip: no "PC is off" detection (the 5 s link timeout still applies). Build with ARDUINO_USB_CDC_ON_BOOT=0.'
foreach ($p in $ports) {
    $id = $p.InstanceId
    switch -Regex ($id) {
        'VID_303A' { $kind = 'Native ESP32-S3 USB (USB Serial/JTAG)'
                     $verdict = 'Supported as is: "PC is off" detection works. Build with ARDUINO_USB_CDC_ON_BOOT=1 (the default).' }
        'VID_1A86' { $kind = 'WCH CH340/CH343 USB-to-serial chip'; $verdict = $uart }
        'VID_10C4' { $kind = 'Silicon Labs CP210x USB-to-serial chip'; $verdict = $uart }
        'VID_0403' { $kind = 'FTDI USB-to-serial chip'; $verdict = $uart }
        default    { $kind = 'Not an ESP32 board (other device)'; $verdict = '' }
    }
    $com = if ($p.FriendlyName -match '\((COM\d+)\)') { $Matches[1] } else { '?' }
    Write-Host ""
    Write-Host ("{0}  {1}" -f $com, $p.FriendlyName)
    Write-Host ("  {0}" -f $id)
    Write-Host ("  -> {0}" -f $kind) -ForegroundColor Cyan
    if ($verdict) { Write-Host ("  -> {0}" -f $verdict) -ForegroundColor Green }
}
Write-Host ""
