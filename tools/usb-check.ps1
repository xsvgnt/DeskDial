# Identifies how the ESP32-S3 board is connected over USB.
# Run with the board plugged in:
#   powershell -ExecutionPolicy Bypass -File .\tools\usb-check.ps1

$ports = Get-PnpDevice -Class Ports -PresentOnly -ErrorAction SilentlyContinue
if (-not $ports) {
    Write-Host "No COM ports found. Is the board plugged in with a data cable (not charge-only)?"
    exit 1
}

foreach ($p in $ports) {
    $id = $p.InstanceId
    switch -Regex ($id) {
        'VID_303A' { $kind = 'NATIVE ESP32-S3 USB'
                     $verdict = 'Option 2 works (PC on/off detectable). Arduino: USB CDC On Boot = Enabled.' }
        'VID_1A86' { $kind = 'WCH CH340/CH343 USB-to-serial chip'
                     $verdict = 'Option 2 not possible, only the timeout. Arduino: USB CDC On Boot = Disabled.' }
        'VID_10C4' { $kind = 'Silicon Labs CP210x USB-to-serial chip'
                     $verdict = 'Option 2 not possible, only the timeout. Arduino: USB CDC On Boot = Disabled.' }
        'VID_0403' { $kind = 'FTDI USB-to-serial chip'
                     $verdict = 'Option 2 not possible, only the timeout. Arduino: USB CDC On Boot = Disabled.' }
        default    { $kind = 'Not the ESP32 board (other device)'; $verdict = '' }
    }
    $com = if ($p.FriendlyName -match '\((COM\d+)\)') { $Matches[1] } else { '?' }
    Write-Host ""
    Write-Host ("{0}  {1}" -f $com, $p.FriendlyName)
    Write-Host ("  {0}" -f $id)
    Write-Host ("  -> {0}" -f $kind) -ForegroundColor Cyan
    if ($verdict) { Write-Host ("  -> {0}" -f $verdict) -ForegroundColor Green }
}
Write-Host ""
