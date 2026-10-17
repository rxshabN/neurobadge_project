<#
  Neuro-Badge soak logger. Records every serial line with a timestamp.
  Close the Arduino Serial Monitor first; it holds the port.

  powershell -ExecutionPolicy Bypass -File .\soak_logger.ps1 -Port COM5 -Minutes 30
#>
param(
  [Parameter(Mandatory = $true)][string]$Port,
  [int]$Minutes = 30
)

New-Item -ItemType Directory -Force -Path "logs" | Out-Null
$out = Join-Path "logs" ("soak_" + (Get-Date -Format "yyyyMMdd_HHmm") + ".txt")

$sp = New-Object System.IO.Ports.SerialPort($Port, 115200, 'None', 8, 'One')
$sp.ReadTimeout = 3000
$sp.DtrEnable = $false
$sp.RtsEnable = $false
$sp.Open()

$end = (Get-Date).AddMinutes($Minutes)
$writer = New-Object System.IO.StreamWriter($out, $false)
$writer.AutoFlush = $true
$lines = 0
Write-Host "Logging $Port to $out for $Minutes min. Ctrl+C stops early; the file keeps what was read."

try {
  while ((Get-Date) -lt $end) {
    try { $line = $sp.ReadLine().TrimEnd() } catch [System.TimeoutException] { continue }
    $writer.WriteLine((Get-Date -Format "HH:mm:ss.fff") + " " + $line)
    $lines++
    if ($line -match 'task_wdt|Rebooting|rst:') { Write-Host "RESET SEEN: $line" -ForegroundColor Red }
    if ($lines % 30 -eq 0) {
      $left = [int]($end - (Get-Date)).TotalMinutes
      Write-Host ("{0} lines, {1} min left. Last: {2}" -f $lines, $left, $line)
    }
  }
}
finally {
  $writer.Close()
  $sp.Close()
  Write-Host "Saved $lines lines to $out"
}
