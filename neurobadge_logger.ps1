<#
  Neuro-Badge :: serial-to-CSV dataset logger
  -------------------------------------------
  Replaces edge-impulse-data-forwarder. The CLI needs a native serial binding
  that has no prebuilt binary for Node 22 and will not compile without a Visual
  Studio toolchain node-gyp recognises. This script talks to the COM port with
  System.IO.Ports, which ships with .NET, and writes CSV files that Edge Impulse
  ingests directly through Data acquisition -> Upload data.

  Run the datacollect sketch on the board first, and close the Arduino Serial
  Monitor — it holds the port open.

  USAGE

    # 1. find the port
    powershell -ExecutionPolicy Bypass -File .\neurobadge_logger.ps1 -ListPorts

    # 2. watch the stream and confirm the values move when you breathe on it
    powershell -ExecutionPolicy Bypass -File .\neurobadge_logger.ps1 -Port COM5 -Preview

    # 3. record a class: five 60-second samples
    powershell -ExecutionPolicy Bypass -File .\neurobadge_logger.ps1 -Port COM5 -Label normal_air
    powershell -ExecutionPolicy Bypass -File .\neurobadge_logger.ps1 -Port COM5 -Label poor_ventilation
    powershell -ExecutionPolicy Bypass -File .\neurobadge_logger.ps1 -Port COM5 -Label high_voc

  Files land in .\dataset\<label>.<n>.csv
#>

param(
  [string]$Port,
  [string]$Label,
  [int]$Seconds = 60,
  [int]$Count   = 5,
  [string]$OutDir = "dataset",
  [switch]$ListPorts,
  [switch]$Preview
)

function Open-Badge([string]$p) {
  $sp = New-Object System.IO.Ports.SerialPort($p, 115200, 'None', 8, 'One')
  $sp.ReadTimeout = 3000
  # Both low: on a CP2102 board DTR and RTS drive the auto-reset circuit, and
  # leaving them asserted bounces the ESP32 every time the port opens.
  $sp.DtrEnable = $false
  $sp.RtsEnable = $false
  $sp.Open()
  Write-Host "Opened $p at 115200. Letting the board settle for 3 s..."
  Start-Sleep -Seconds 3
  $sp.DiscardInBuffer()
  return $sp
}

# Three comma-separated numbers and nothing else. Anything the sketch or the
# bootloader prints on the way up is discarded rather than corrupting a sample.
$LINE = '^\s*\d+\s*,\s*-?\d+(\.\d+)?\s*,\s*-?\d+(\.\d+)?\s*$'

if ($ListPorts) {
  $ports = [System.IO.Ports.SerialPort]::GetPortNames()
  if ($ports.Count -eq 0) { Write-Host "No serial ports found. Is the board plugged in?" }
  else { Write-Host "Available ports:"; $ports | ForEach-Object { Write-Host "  $_" } }
  exit 0
}

if (-not $Port) { Write-Host "ERROR: -Port is required. Run with -ListPorts to find it." ; exit 1 }

# ----------------------------------------------------------------- preview
if ($Preview) {
  $sp = Open-Badge $Port
  Write-Host ""
  Write-Host "Streaming for 30 s.  Columns: gas_raw, temperature, humidity"
  Write-Host "Breathe on the sensor and confirm gas_raw and humidity both rise."
  Write-Host ""
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  while ($sw.Elapsed.TotalSeconds -lt 30) {
    try { $line = $sp.ReadLine().Trim() } catch { continue }
    if ($line -match $LINE) { Write-Host "  $line" }
  }
  $sp.Close()
  exit 0
}

# ----------------------------------------------------------------- record
if (-not $Label) { Write-Host "ERROR: -Label is required (normal_air, poor_ventilation or high_voc)." ; exit 1 }

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$sp = Open-Badge $Port

for ($n = 1; $n -le $Count; $n++) {
  Write-Host ""
  Write-Host "=== $Label : sample $n of $Count  ($Seconds s) ==="
  Read-Host "Position the badge, then press Enter to start"

  $rows = New-Object System.Collections.Generic.List[string]
  $sp.DiscardInBuffer()
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $nextTick = 5

  while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
    try { $line = $sp.ReadLine().Trim() } catch { continue }
    if ($line -notmatch $LINE) { continue }

    $parts = $line -split ','
    $ts = [int]$sw.Elapsed.TotalMilliseconds
    $rows.Add(("{0},{1},{2},{3}" -f $ts, $parts[0].Trim(), $parts[1].Trim(), $parts[2].Trim()))

    if ($sw.Elapsed.TotalSeconds -ge $nextTick) {
      Write-Host ("  t={0,3}s  gas={1,-6} T={2,-6} RH={3,-6}  ({4} rows)" -f `
                  [int]$sw.Elapsed.TotalSeconds, $parts[0].Trim(), $parts[1].Trim(),
                  $parts[2].Trim(), $rows.Count)
      $nextTick += 5
    }
  }

  if ($rows.Count -lt 10) {
    Write-Host "  WARNING: only $($rows.Count) rows captured. Check the sketch is running"
    Write-Host "           and that the Serial Monitor is closed. Sample NOT written."
    continue
  }

  $file = Join-Path $OutDir "$Label.$n.csv"
  "timestamp,gas_raw,temperature,humidity" | Set-Content -Path $file -Encoding ASCII
  $rows | Add-Content -Path $file -Encoding ASCII
  Write-Host ("  wrote {0}   ({1} rows)" -f $file, $rows.Count)
}

$sp.Close()
Write-Host ""
Write-Host "Done. Upload the files in .\$OutDir to Edge Impulse:"
Write-Host "  Data acquisition -> Upload data -> select files -> label from filename."
