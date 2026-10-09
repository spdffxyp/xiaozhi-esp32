<#
.SYNOPSIS
  抓取小魔豆 B220 的串口启动日志（COM25 = 接到 ESP32 UART0 的 TTL 转接板）。

.DESCRIPTION
  本机 TTL 只接了 TX/RX/GND，没有 DTR/RTS，所以不能靠 esptool 复位。
  复位方式二选一：
    * 让 flash-b220.ps1 用 --after watchdog_reset 触发（推荐，脚本末尾会自动做）
    * 或者你手动拔插一次设备电源
  抓取本身只读串口，不发送任何数据。

.EXAMPLE
  .\capture-serial.ps1                      # 抓 30 秒到 logs\boot-<时间>.log
  .\capture-serial.ps1 -Seconds 90          # 抓久一点（看配网/握手）
  .\capture-serial.ps1 -Watch               # 持续监听，Ctrl+C 退出
#>
param(
    [string]$Port    = 'COM25',
    [int]   $Baud    = 115200,
    [int]   $Seconds = 30,
    [string]$OutFile,
    [switch]$Watch
)

$ErrorActionPreference = 'Stop'

if (-not $OutFile) {
    $dir = '<workspace>\logs'
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    $OutFile = Join-Path $dir ("boot-{0:yyyyMMdd-HHmmss}.log" -f (Get-Date))
}

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.DtrEnable   = $false
$sp.RtsEnable   = $false

try {
    $sp.Open()
} catch {
    throw "打不开 $Port（$($_.Exception.Message)）。若有别的串口工具占着它，请先关掉。"
}

Write-Host "监听 $Port @ $Baud $(if ($Watch) {'（持续，Ctrl+C 退出）'} else {"（$Seconds 秒）"}) -> $OutFile" -ForegroundColor Cyan

$sw = [Diagnostics.Stopwatch]::StartNew()
$writer = New-Object System.IO.StreamWriter($OutFile, $false, [System.Text.Encoding]::UTF8)
try {
    while ($Watch -or $sw.Elapsed.TotalSeconds -lt $Seconds) {
        try {
            $line = $sp.ReadLine()
            if ($null -ne $line -and $line.Length -gt 0) {
                $line = $line.TrimEnd()
                Write-Host $line
                $writer.WriteLine($line)
                $writer.Flush()
            }
        } catch [TimeoutException] {
            # 没数据就继续等
        }
    }
} finally {
    $writer.Close()
    if ($sp.IsOpen) { $sp.Close() }
}

Write-Host "`n已保存: $OutFile  （$(if (Test-Path $OutFile) { (Get-Item $OutFile).Length } else { 0 }) 字节）" -ForegroundColor Green
