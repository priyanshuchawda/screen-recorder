Write-Host "=== GPU ==="
Get-CimInstance Win32_VideoController | Select-Object Name, AdapterRAM, DriverVersion, VideoProcessor, CurrentHorizontalResolution, CurrentVerticalResolution | Format-List

Write-Host "=== CPU ==="
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed | Format-List

Write-Host "=== RAM ==="
$os = Get-CimInstance Win32_OperatingSystem
$totalGB = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1)
$freeGB  = [math]::Round($os.FreePhysicalMemory / 1MB, 1)
Write-Host "Total: $totalGB GB"
Write-Host "Free:  $freeGB GB"

Write-Host "=== OS ==="
$os | Select-Object Caption, BuildNumber, OSArchitecture | Format-List

Write-Host "=== BATTERY ==="
Get-CimInstance Win32_Battery | Select-Object Name, EstimatedChargeRemaining, BatteryStatus | Format-List

Write-Host "=== POWER ==="
powercfg /getactivescheme

Write-Host "=== D3D FEATURE LEVELS ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    Write-Host ("GPU: " + $_.Name)
    Write-Host ("VRAM: " + [math]::Round($_.AdapterRAM / 1GB, 1) + " GB")
    Write-Host ("Driver: " + $_.DriverVersion)
}

Write-Host "=== DISPLAY ==="
Get-CimInstance Win32_VideoController | ForEach-Object {
    Write-Host ("Resolution: " + $_.CurrentHorizontalResolution + "x" + $_.CurrentVerticalResolution)
    Write-Host ("Refresh Rate: " + $_.CurrentRefreshRate + " Hz")
}

Write-Host "=== MEDIA FOUNDATION ==="
# Check if HW H264 encoder is present
$mftKey = "HKLM:\SOFTWARE\Classes\CLSID\{6CA50344-051A-4DED-9779-A43305165E35}"
if (Test-Path $mftKey) { Write-Host "Intel Quick Sync / HW MFT registry key found" } else { Write-Host "HW MFT registry key not found (not conclusive)" }
