<#
.SYNOPSIS
  把小智（xiaozhi-esp32）B220 固件刷进小魔豆 B220。

.DESCRIPTION
  刷机口 = 接到 ESP32 UART0 的 3.3V TTL 转接板（默认 COM25）。
  因为 TTL 未接 DTR/RTS，所以用 --before no_reset：先手动让设备进下载模式
  （按住启动键上电 / 或复位时拉低 GPIO0），看到 "waiting for download" 再跑本脚本。

  分区布局（partitions/v2/16m.csv）：
      bootloader 0x1000 | 分区表 0x8000 | otadata 0xd000
      app(ota_0) 0x20000 | app(ota_1) 0x410000 | assets 0x800000 (8MB)

  唤醒词模型（esp-sr WakeNet9 "你好小智" = wn9_nihaoxiaozhi_tts）：
      本板没有 model 分区，模型由 scripts/build_default_assets.py 打进
      build\generated_assets.bin 的 srmodels.bin，随 assets 写到 0x800000 ——
      所以刷机清单仍是 5 项，偏移一个都没变，assets 也不用整片重刷。
      （若将来分区表里出现 model 分区，本脚本会自动把 build\srmodels\srmodels.bin
        按分区表偏移一起写；文件不存在就直接报错并说明怎么办。）

  首次从原厂固件切到小智时建议加 -FullErase：原厂分区布局与 assets 内容都不同，
  整片擦除可以避免 assets 分区里残留的 8MB 原厂音频数据干扰挂载。

  回滚（任何时候都能回到原厂）：
      & $py -m esptool --port COM25 --before no_reset --after no_reset `
          write_flash 0x0 <workspace>\backup\esp32-16MB-full.bin
  备份 MD5 应为 A6A27D2B4B7A8DE36636E9EF635BEA40。

.EXAMPLE
  .\flash-b220.ps1 -FullErase        # 首次刷写（整片擦除后写入）
  .\flash-b220.ps1                   # 后续迭代（保留 NVS/WiFi 凭据）
#>
param(
    [string]$Port     = 'COM25',
    [int]   $Baud     = 921600,
    [switch]$FullErase,
    [switch]$NoBoot,
    [string]$BuildDir = '<repo>\build'
)

$ErrorActionPreference = 'Stop'
$py = '<idf-python>'
if (-not (Test-Path $py)) { throw "esptool 的 python 环境不存在: $py" }

$files = @(
    @{ off = '0x1000';   path = Join-Path $BuildDir 'bootloader\bootloader.bin' },
    @{ off = '0x8000';   path = Join-Path $BuildDir 'partition_table\partition-table.bin' },
    @{ off = '0xd000';   path = Join-Path $BuildDir 'ota_data_initial.bin' },
    @{ off = '0x20000';  path = Join-Path $BuildDir 'xiaozhi.bin' },
    @{ off = '0x800000'; path = Join-Path $BuildDir 'generated_assets.bin' }
)

# =============================================================================
# 唤醒词模型落在哪里？先认分区表，再认产物。
#
# 现状（partitions/v2/16m.csv）：没有 model 分区，esp-sr 的模型在
# build\generated_assets.bin 里（assets 中的 srmodels.bin），随上面第 5 项写到
# 0x800000 —— 不新增分区、不动任何偏移，只重写 assets 这一个文件。
# 保险：如果分区表里以后真加了 model 分区，只要 build\srmodels\srmodels.bin
# 存在（esp-sr 的 srmodels_bin 目标生成）就自动按该分区偏移一起写。
# =============================================================================
$repoRoot  = '<repo>'
$boardCfg  = Join-Path $repoRoot 'main\boards\xiaomodou-b220\config.json'
$modelBin  = Join-Path $BuildDir 'srmodels\srmodels.bin'
$assetsBin = Join-Path $BuildDir 'generated_assets.bin'

function Get-ModelPartitionOffset {
    param([string]$PartitionCsv)
    if (-not (Test-Path $PartitionCsv)) { return $null }
    $next = [int64]0
    foreach ($line in (Get-Content $PartitionCsv)) {
        $line = ($line -split '#')[0].Trim()
        if (-not $line) { continue }
        $f = @($line.Split(',') | ForEach-Object { $_.Trim() })
        if ($f.Count -lt 5 -or $f[0] -eq 'Name') { continue }
        if     ($f[4] -match '^0x([0-9a-fA-F]+)$') { $size = [int64][Convert]::ToInt64($Matches[1], 16) }
        elseif ($f[4] -match '^([0-9]+)[Mm]$')     { $size = [int64]$Matches[1] * 1MB }
        elseif ($f[4] -match '^([0-9]+)[Kk]$')     { $size = [int64]$Matches[1] * 1KB }
        else                                       { $size = [int64]$f[4] }
        if ($f[3] -match '^0x[0-9a-fA-F]+$') { $off = [int64][Convert]::ToInt64($f[3].Substring(2), 16) } else { $off = $next }
        if ($f[0] -eq 'model') { return $off }
        $next = $off + $size
    }
    return $null
}

$csvRel = $null
if (Test-Path $boardCfg) {
    $cfgText = Get-Content $boardCfg -Raw
    if ($cfgText -match 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME=\\"([^"\\]+)\\"') { $csvRel = $Matches[1] }
}
$modelOffset = $null
if ($csvRel) {
    $modelOffset = Get-ModelPartitionOffset (Join-Path $repoRoot ($csvRel -replace '/', '\'))
}
if ($null -ne $modelOffset) {
    if (Test-Path $modelBin) {
        $files += @{ off = ('0x{0:x}' -f $modelOffset); path = $modelBin }
    } else {
        throw (("分区表 {0} 里声明了 model 分区，但没有 {1} —— 该文件由 esp-sr 的 srmodels_bin 目标在构建时生成；" +
                '请先在 xiaozhi-esp32-b220 里重新构建（idf.py build 或 python scripts\build.py xiaomodou-b220），再刷机。') -f $csvRel, $modelBin)
    }
}
foreach ($f in $files) {
    if (-not (Test-Path $f.path)) { throw "缺少构建产物: $($f.path) —— 先在 xiaozhi-esp32-b220 里执行 python scripts/build.py xiaomodou-b220" }
    Write-Host ("  {0,-9} {1,9:N0} B  {2}" -f $f.off, (Get-Item $f.path).Length, (Split-Path $f.path -Leaf))
}

# --- 唤醒词模型自检：assets 里必须真的有 srmodels.bin，否则刷完唤不醒 --------
if ($null -eq $modelOffset) {
    Write-Host ("  分区表 {0} 无 model 分区 → 唤醒词模型随 generated_assets.bin 写到 0x800000（偏移全未变）" -f $csvRel) -ForegroundColor DarkGray
}
if (Test-Path $assetsBin) {
    $assetsOk = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($assetsBin)).Contains('srmodels')
    if ($assetsOk) {
        Write-Host "  assets 自检: 含 srmodels（wn9_nihaoxiaozhi_tts 唤醒词模型）" -ForegroundColor Green
    } else {
        Write-Host "  !! assets 自检: 里面没有 srmodels —— 设备能正常显示，但**语音唤醒不会工作**。" -ForegroundColor Yellow
        Write-Host "     处理: 确认 main\boards\xiaomodou-b220\config.json 含 CONFIG_USE_ESP_WAKE_WORD=y，" -ForegroundColor Yellow
        Write-Host "     然后重新构建 assets 再刷（构建时看 build_default_assets.py 是否打印 'wakenet models: ...'）：" -ForegroundColor Yellow
        Write-Host "       python scripts\build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi" -ForegroundColor Yellow
    }
}

$common = @('-m', 'esptool', '--port', $Port, '--before', 'no_reset', '--after', 'no_reset', '--baud', "$Baud")

Write-Host "`n=== flash_id ===" -ForegroundColor Cyan
& $py @common flash_id

if ($FullErase) {
    Write-Host "`n=== erase_flash (整片擦除, 约 1-2 分钟) ===" -ForegroundColor Yellow
    & $py @common erase_flash
}

Write-Host "`n=== write_flash ===" -ForegroundColor Cyan
$writeArgs = @($common + 'write_flash')
foreach ($f in $files) { $writeArgs += $f.off; $writeArgs += $f.path }
& $py @writeArgs
if ($LASTEXITCODE -ne 0) { throw "write_flash 失败（exit $LASTEXITCODE）" }

Write-Host "`n=== 校验：读回 app 镜像并与本地比对 ===" -ForegroundColor Cyan
$tmp = Join-Path $env:TEMP 'b220-verify-app.bin'
$appSize = (Get-Item (Join-Path $BuildDir 'xiaozhi.bin')).Length
& $py @common read_flash 0x20000 ('0x{0:x}' -f $appSize) $tmp
$h1 = (Get-FileHash (Join-Path $BuildDir 'xiaozhi.bin') -Algorithm MD5).Hash
$h2 = (Get-FileHash $tmp -Algorithm MD5).Hash
Write-Host ("  built  {0}" -f $h1)
Write-Host ("  on-chip {0}" -f $h2)
if ($h1 -ne $h2) { throw "读回校验失败：片上内容与构建产物不一致！" }
Write-Host "`n✅ 刷写并校验通过。" -ForegroundColor Green

if (-not $NoBoot) {
    # 我们的 TTL 只接了 TX/RX/GND，没有 DTR/RTS，所以不能用 hard_reset。
    # esptool 的 watchdog_reset 走 RTC 看门狗，不需要 DTR/RTS。
    Write-Host "`n=== 触发复位（watchdog_reset）让新固件启动 ===" -ForegroundColor Cyan
    $bootArgs = @('-m', 'esptool', '--port', $Port, '--before', 'no_reset', '--after', 'watchdog_reset', 'read_mac')
    & $py @bootArgs
    Write-Host "`n设备已复位，现在可以监听 $Port @115200 抓启动日志。" -ForegroundColor Green
}
