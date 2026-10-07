<#
.SYNOPSIS
  Flash the CubeIDE build to an Arduino Nicla Vision over USB using the
  Arduino DFU bootloader (no SWD / ST-Link needed).

.DESCRIPTION
  1. If the board is running our firmware, does the "1200 baud touch" on its
     virtual COM port so it reboots into the Arduino bootloader.
     (Or double-tap the reset button yourself: the green LED fades in/out.)
  2. Writes the CM4 image to 0x08100000 and the CM7 image to 0x08040000
     with dfu-util, then leaves DFU so the new firmware starts.

.EXAMPLE
  .\tools\upload.ps1                      # Debug build, both cores, auto-detect port
  .\tools\upload.ps1 -Config Release
  .\tools\upload.ps1 -Core CM7 -Port COM7
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',

    [ValidateSet('Both', 'CM7', 'CM4')]
    [string]$Core = 'Both',

    # COM port of the running firmware. Auto-detected when omitted.
    [string]$Port,

    # How long to wait for the bootloader to show up
    [int]$WaitSeconds = 15,

    # Path to dfu-util.exe. Taken from PATH or the Arduino IDE install when omitted.
    [string]$DfuUtil
)

$ErrorActionPreference = 'Stop'

$Root        = Split-Path -Parent $PSScriptRoot
# Project name = name of the .ioc file next to the tools folder (e.g. MY_NICLA.ioc
# -> MY_NICLA_CM7 / MY_NICLA_CM4), so this script can be copied unchanged
$ioc = Get-ChildItem -Path $Root -Filter *.ioc | Select-Object -First 1
if (-not $ioc) { throw "No .ioc file found in $Root - put the tools folder in the project root." }
$ProjectName = $ioc.BaseName
$DfuId       = '2341:035f'   # Nicla Vision bootloader VID:PID
$Cm7Address  = '0x08040000'  # Bootloader owns 0x08000000-0x0803FFFF
$Cm4Address  = '0x08100000'

# VID/PID of the running board: our CubeIDE CDC (ST VCP) or an Arduino sketch
$AppUsbIds = @('VID_0483&PID_5740', 'VID_2341&PID_025F')

function Find-DfuUtil {
    if ($DfuUtil) { return $DfuUtil }
    $cmd = Get-Command dfu-util -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $arduinoTools = Join-Path $env:LOCALAPPDATA 'Arduino15\packages\arduino\tools\dfu-util'
    if (Test-Path $arduinoTools) {
        $exe = Get-ChildItem $arduinoTools -Recurse -Filter dfu-util.exe |
            Sort-Object { $_.Directory.Name } -Descending | Select-Object -First 1
        if ($exe) { return $exe.FullName }
    }
    throw "dfu-util not found. Install the 'Arduino Mbed OS Nicla Boards' core in the Arduino IDE, add dfu-util to PATH, or pass -DfuUtil <path>."
}

function Test-DfuPresent([string]$exe) {
    $out = & $exe -l 2>&1 | Out-String
    return $out -match [regex]::Escape("[$DfuId]")
}

function Find-AppPort {
    $ports = Get-CimInstance Win32_PnPEntity -Filter "PNPClass = 'Ports'" -ErrorAction SilentlyContinue
    foreach ($p in $ports) {
        foreach ($id in $AppUsbIds) {
            if ($p.PNPDeviceID -like "USB\$id*" -and $p.Name -match '\((COM\d+)\)') {
                return $Matches[1]
            }
        }
    }
    return $null
}

function Invoke-1200bpsTouch([string]$com) {
    Write-Host "1200 baud touch on $com ..."
    $sp = New-Object System.IO.Ports.SerialPort $com, 1200
    $sp.DtrEnable = $false
    try {
        $sp.Open()
        Start-Sleep -Milliseconds 100
        $sp.Close()
    } catch {
        # The board resets while the port is open, so errors here are expected
    }
}

function Get-Image([string]$coreName) {
    $bin = Join-Path $Root "$coreName\$Config\${ProjectName}_$coreName.bin"
    if (-not (Test-Path $bin)) {
        throw "Missing $bin - build the $coreName project ($Config) in STM32CubeIDE first."
    }
    return $bin
}

$dfu = Find-DfuUtil
Write-Host "dfu-util: $dfu"

$images = @()
if ($Core -in 'Both', 'CM4') { $images += , @('CM4', (Get-Image 'CM4'), $Cm4Address) }
if ($Core -in 'Both', 'CM7') { $images += , @('CM7', (Get-Image 'CM7'), $Cm7Address) }

if (-not (Test-DfuPresent $dfu)) {
    if (-not $Port) { $Port = Find-AppPort }
    if ($Port) {
        Invoke-1200bpsTouch $Port
    } else {
        Write-Host 'No running board found. Double-tap the reset button to enter the bootloader...'
    }

    $deadline = (Get-Date).AddSeconds($WaitSeconds)
    while (-not (Test-DfuPresent $dfu)) {
        if ((Get-Date) -gt $deadline) {
            throw "Bootloader ($DfuId) did not show up. Double-tap reset (green LED pulses) and run again."
        }
        Start-Sleep -Milliseconds 500
    }
}
Write-Host 'Bootloader found.'

for ($i = 0; $i -lt $images.Count; $i++) {
    $name, $bin, $addr = $images[$i]
    $last = ($i -eq $images.Count - 1)
    $target = if ($last) { "${addr}:leave" } else { $addr }
    Write-Host "`n== Flashing $name : $bin -> $addr"
    & $dfu --device $DfuId -a 0 --dfuse-address=$target -D $bin
    # dfu-util returns 74 when the board resets on :leave before it can read the status
    if ($LASTEXITCODE -ne 0 -and -not ($last -and $LASTEXITCODE -eq 74)) {
        throw "dfu-util failed for $name (exit code $LASTEXITCODE)"
    }
}

Write-Host "`nDone - firmware is starting."
