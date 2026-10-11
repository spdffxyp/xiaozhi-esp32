# B220 构建与烧写指南（供 agent 使用）

本文件只讲**怎么编译、怎么烧写、怎么验证**。协议与硬件调试结论见
[B220-LED-AND-BASE-UART.md](B220-LED-AND-BASE-UART.md)。

> **本文件里的命令都是实际跑通过的**（2026-10-11，ESP-IDF v6.1）。
> 命令、路径、默认值、偏移都来自本机实测与脚本源码，不是推测。
> 若发现与实际不符，**以脚本和构建输出为准，并回来修正本文件**。

板级专属规则见仓库根的 `AGENTS.md`；本文件是它的补充，**冲突时以 `AGENTS.md` 为准**。

---

## 1. 环境（固定路径，不要猜）

| 项 | 值 |
| --- | --- |
| 工作区根 | `<workspace>` |
| 固件工程 | `<repo>` |
| ESP-IDF | `C:\esp\v6.1\esp-idf`（v6.1） |
| IDF 工具目录 | `C:\Espressif\tools` |
| Python 环境 | `C:\Espressif\tools\python\v6.1\venv` |
| 工具链 | `C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204` |
| IDF 环境脚本 | `C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1` |

**每个新 shell 都要先载入环境**，否则 `idf.py` 与工具链都不在 PATH：

```powershell
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
```

（`source` 是 Linux 写法；这里是 PowerShell，用 `.` 点源。）

---

## 2. ★ 沙箱权限：这一步最容易卡住

**`idf.py` / `ninja` 需要创建命名管道，在受限沙箱里必然失败**
（表现为 `WinError 5`、`ninja failed`、或莫名其妙的子进程错误）。

所以**构建与烧写必须提权**（`danger-full-access`）。若被拒绝，
**停下来向用户说明并请求批准，不要绕路**（例如改用别的构建方式或手工拼命令）。

判断依据：报错里出现 `Access is denied` / `WinError 5` / 管道相关字样，
且**同一命令**在提权后成功 —— 那就是沙箱问题，不是代码问题。

---

## 3. 编译

```powershell
cd <workspace>
. 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
cd xiaozhi-esp32-b220
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi
```

| 参数 | 说明 |
| --- | --- |
| `xiaomodou-b220`（位置参数） | **板目录名**（`main/boards/<这个名字>/`） |
| `--name` | **变体名**，来自该板 `config.json` 的 `builds[].name`；写入 `sdkconfig` |
| `--wake-word` | 唤醒词，本项目为 `nihaoxiaozhi` |

**枚举正确取值**（不要凭记忆写）：

```powershell
python scripts/build.py --list-boards
```

**建议把输出重定向到日志再筛选**，因为 IDF 输出很长：

```powershell
python scripts/build.py xiaomodou-b220 --name xiaomodou-b220 --wake-word nihaoxiaozhi *> build.log
"exit=$LASTEXITCODE"
Select-String -Path build.log -Pattern 'Build completed|error:|warning:|FAILED'
```

**判定成功的唯一标准**：出现 `Build completed successfully!` **且没有 `error:` / `warning:`**。
本项目把警告视为必须处理的问题（历史上靠"零警告"抓出过真 bug）。

> **构建成功 ≠ 硬件验证。** 任何"已修复"的结论都必须区分
> **"编译通过"** 与 **"真机现象确认"**。

---

## 4. 产物

| 文件 | 用途 |
| --- | --- |
| `build/xiaozhi.bin` | **应用镜像**（约 2.5 MB） |
| `build/xiaozhi.elf` | 带符号，供 `nm` / `objdump` 反汇编验证 |
| `build/bootloader/bootloader.bin` | 引导 |
| `build/partition_table/partition-table.bin` | 分区表 |
| `build/generated_assets.bin` | assets（含唤醒词模型） |

**改动生效的标志**是 `build/xiaozhi.bin` 的**时间戳变化**。可用它确认
"这次编译真的重新链接了"：

```powershell
Get-Item build\xiaozhi.bin | Select-Object Length,LastWriteTime
```

---

## 5. 验证编译结果（不依赖硬件）

**两种方法，用途不同。选错会得出错误结论。**

### 5.1 符号/反汇编 —— 判断"代码是否真的被编进去、调用顺序对不对"

```powershell
$nm = 'C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32-elf-nm.exe'
$od = 'C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin\xtensa-esp32-elf-objdump.exe'

# 找符号（-C 展开 C++ 名字）
& $nm -C build\xiaozhi.elf | Select-String -Pattern 'TcpLog|b220_base'

# 反汇编（注意：函数可能被内联，符号名不一定存在）
& $od -d --start-address=0x400f4e98 --stop-address=0x400f51f0 build\xiaozhi.elf |
    Select-String -Pattern 'call8|l32r'
```

要点：

- **看调用顺序**：例如"静态初始化里不得出现 socket"这类约束，
  只能靠反汇编证明（本项目真出过这个 bug）。
- **间接调用**：Xtensa 常用 `l32r a8, <地址>` + `callx8 a8`，
  所以**按函数名 grep `call8` 可能一条都找不到**，要同时看 `callx8`。
- **被内联/被优化掉的函数不会有独立符号**（本项目出现过两次误判）。

### 5.2 镜像内字符串 —— 只用于"这版固件包含某功能"的粗检

```powershell
$t = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes('build\xiaozhi.bin'))
$t.Contains('self.base.move')
```

**注意两个坑**：

- `build/xiaozhi.bin` 是**压缩镜像**，很多字符串查不到；
- 运行期拼接的格式串（如 `"DIRS %u/%u"` 里的片段）**不一定会原样出现**。
- **绝不能**用 ELF 里的字符串搜日志文本 —— ESP-IDF 会把日志格式串去重进表，
  搜索结果与"代码是否调用"无关。

---

## 6. 烧写

```powershell
cd <workspace>
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& '.\flash-b220.ps1' -NoBoot
```

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `-Port` | `COM25` | 接到 ESP32 UART0 的 TTL 转接板 |
| `-Baud` | `921600` | 烧写波特率 |
| `-BuildDir` | `...\xiaozhi-esp32-b220\build` | 产物目录 |
| `-NoBoot` | — | **跳过写完后的 `verify_flash` 校验**（串口质量差导致校验失败时才用） |
| `-FullErase` | — | 全片擦除 |

**分区与偏移**（`partitions/v2/16m.csv`，16 MB flash）：

```
nvs 0x9000 | otadata 0xd000 | phy_init 0xf000
ota_0 0x20000 (app) | ota_1 0x410000 (app) | assets 0x800000 (8M)
bootloader → 0x1000 | partition-table → 0x8000
```

**本机没有自动复位能力**：TTL 转接线**只接了 TX/RX/GND，没有 DTR/RTS**，
所以任何依赖 DTR/RTS 的复位（`hard_reset` / `soft_reset`）在这套线材上都不会真的生效。
脚本因此用 `--before no_reset`。所以：

- **手动让设备进下载模式**：**上电时按住 boot 键**，或**插 micro-USB**；
- 烧写完成后**手动断电重上**才会跑新固件；
- 不要为了"让复位生效"反复改脚本 —— 这是线材限制，不是脚本 bug。

**校验**：脚本写完调用 `verify_flash`（而不是把 2.4 MB 读回本地算 MD5 ——
那种做法在 921600 下曾被串口抖动拖垮）。看到校验通过即成功。
若校验因串口质量失败：先降速重试（`-Baud 460800`），再考虑 `-NoBoot`
（写入过程本身已有 esptool 的逐块 MD5 校验）。

---

## 7. 串口日志

```powershell
& '.\capture-serial.ps1' -Seconds 60            # 默认 COM25 / 115200
```

| 参数 | 默认 |
| --- | --- |
| `-Port` | `COM25` |
| `-Baud` | `115200` |
| `-Seconds` | `30` |
| `-OutFile` | 自动落到 `logs\boot-<时间戳>.log` |
| `-Watch` | 持续输出 |

---

## 8. ★ 本项目特有的陷阱

### 8.1 控制台被关闭 ⇒ **串口默认没有日志**

`config.json` 里有 `CONFIG_ESP_CONSOLE_NONE=y`（UART0 交给底座），
所以**应用日志默认不出现在串口上**：

- 日志走 **TCP 3333**（`ncat <设备IP> 3333`，同时也是一个命令通道）；
- 串口上仍能看到**启动早期**（ROM/引导）的输出，但**看不到应用日志**；
- 因此 **boot 日志里查不到运动/灯光帧的回显** —— 要查那些必须看 TCP 日志。

> 若需要串口日志做排查，把 `CONFIG_ESP_CONSOLE_NONE=y` 临时去掉再编译，
> **但要明白这会把 UART0 从底座手里收回**。

### 8.2 `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=3584`

不是随便调的：日志钩子要在**调用方栈**上格式化，WiFi 事件任务（默认 2304）会栈溢出。
**调小它会复现开机崩溃**（`stack overflow in task sys_evt`）。

### 8.3 板级文件与 `main.cc`

- 板级代码全部在 `main/boards/xiaomodou-b220/`；
- **`main.cc` 不应被修改**；
- 板子特有行为不要放进 core 模块（见 `AGENTS.md`）。

### 8.4 不要手动编辑生成物

`build/`、`sdkconfig`、`sdkconfig.old`、`managed_components/`、`components/`、
`main/assets/lang_config.h`、生成的 mmap 头文件 —— 都由构建生成，**改了会被覆盖**。

要改配置，改 **`main/boards/xiaomodou-b220/config.json` 的 `sdkconfig_append`**。

---

## 9. 常见错误对照表

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| `WinError 5` / `ninja failed` / 命名管道错误 | 沙箱限制 | **提权**（`danger-full-access`），被拒就停下来问用户 |
| `idf.py: command not found` | 没载入 IDF 环境 | 点源 `Microsoft.v6.1.PowerShell_profile.ps1` |
| `uart_driver_install failed (ESP_ERR_INVALID_STATE)` | 控制台仍占着 UART0 | 正常 —— 代码会退回 ROM 发送器；要可读 RX 就关控制台 |
| `uart_write_bytes: uart driver error` | 控制台驱动的 TX 环长度为 0 | 已由 ROM 发送器路径解决；不要改回 `uart_write_bytes` |
| `stack overflow in task sys_evt` | 日志钩子在调用方栈上占用过大 | 见 8.2，不要动栈大小以外的参数 |
| 编译通过但串口没日志 | 见 8.1 | 用 TCP 3333 |
| 烧写后设备行为没变 | 产物没重新链接 / 没进下载模式 | 核对 `xiaozhi.bin` 时间戳；手动上电 |
| `error: expected expression before '{' token` | 在 **C** 文件里用了 C++ 的花括号赋值 | 改用逐字段赋值（`s.x = 1;`） |
| `'X' undeclared` / 常量顺序错 | 常量定义在使用之后 | 把常量提到首次使用之前 |
| `declared 'static' but never defined` | 有前向声明但定义被删/漏 | 删掉残留声明或补上定义 |

---

## 10. 交付前自检清单

1. 构建：**零警告**，`Build completed successfully!`；
2. `build/xiaozhi.bin` **时间戳已更新**；
3. 关键改动有**符号/反汇编或镜像字符串**证据；
4. commit message **区分**"编译验证"与"真机验证"；
5. 若动了协议或帧格式，**同步更新**
   [B220-LED-AND-BASE-UART.md](B220-LED-AND-BASE-UART.md)；
6. **未经用户明确要求不要 commit**；不要 amend 已推送的提交。
