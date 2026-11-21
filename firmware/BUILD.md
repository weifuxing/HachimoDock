# 固件构建与烧录 / Firmware build and flashing

当前源码版本 **0.7.107-p4-r292 / R292**，配套桌面端 **0.1.245**。核对发布镜像时同时检查构建身份和 SHA-256。设备功能见 [固件说明](README.md)，字段契约见 [protocol.md](protocol.md)。

HD28091 屏幕与 Rev A 转接板使用本分支构建的 `-hd28091` 固件，四个发布目标均保留该后缀。默认显示参数为双通道 DSI、每通道 500Mbps，v3 DPI 时钟为 20MHz，CST328 每 15ms 轮询。接线和已有实测结果见 [硬件适配说明](../docs/hd28091-hardware-adaptation.md)。

## 选择构建目标

| 板型 / 芯片 | 环境 | 芯片修订范围 | 桌面 OTA 文件 |
| --- | --- | --- | --- |
| WiFi6 v1 | `esp32_p4_wifi6_system_settings_v1` | 0.1–1.99 | `firmware.bin` |
| WiFi6 v3 | `esp32_p4_wifi6_system_settings_v3` | 3.0–3.99 | `firmware-v3.bin` |
| Pico v1 原底板 | `esp32_p4_pico_v1` | 0.1–1.99 | `firmware-pico-v1.bin` |
| Pico v3 原底板 | `esp32_p4_pico_v3` | 3.0–3.99（含 v3.2） | `firmware-pico-v3.bin` |

四个目标的串口运行参数均为 4 Mbaud，同一板型和芯片的应用镜像由 macOS 与 Windows 共用。WiFi6 包含受支持的 Wi-Fi／蓝牙功能，Pico 不包含这两个协议栈。通用 `esp32_p4_evboard` 及诊断环境不能替代上述四板配置。

WiFi6 目标覆盖普通版 C6 和 DB 版 C5。构建与验收细节见 [Wi-Fi 6 兼容性](docs/wifi6-compatibility.md)。

编译器、平台和 SDK 以 [platformio.ini](platformio.ini)、各目标 SDK 配置与依赖锁文件为准。v1 保持 360 MHz，v3 使用 400 MHz；不要混用 sdkconfig、芯片门槛或分区表。微雪 ESP32-P4-WIFI6-M 的 M 表示焊接排针，不能据此判断芯片修订版。**WiFi6 与 Pico、v1 与 v3 不可混刷**，无法确认时应先核对硬件；不要降低版本门槛或强制写入绕过检查。

## 获取与构建

只想使用设备，可从 [发布页](https://github.com/ai-maker-lab/HachimoDock/releases/latest) 下载与板型及芯片匹配的完整烧录包。包内工具要求 Python **3.10+**，使用 **esptool 5.4.0**。首次安装或恢复出厂使用完整烧录，日常升级优先使用客户端 OTA。

自行构建需要 Git、Python 3.10+、PlatformIO Core **6.1.19**、Node.js 22、pytest 和 Pillow。Core 6.1.18 不支持当前 v3 平台。资源使用普通 Git，不依赖 Git LFS。建议使用独立 Python 环境。

固件的 [WAMR 组件](components/hachi_wamr/CMakeLists.txt) 需要 `WAMR_ROOT` 指向相容的 WebAssembly Micro Runtime 源码根目录，其中应有 `build-scripts/runtime_lib.cmake`；编译选项见 [wamr-options.cmake](components/hachi_wamr/wamr-options.cmake)，许可见 [第三方声明](../THIRD_PARTY_NOTICES.md)。工具链、SDK 和 WAMR 必须由构建者准备，公开源码不包含开发者机器的依赖缓存或凭据。更换依赖版本后需要重新验证，不能据此保证与已发布镜像逐字节一致。

从仓库根目录执行：

```sh
python -m pip install platformio==6.1.19 pytest Pillow
python firmware/tools/build_builtin_bundle.py --check
# 完整主机测试还需要下文所述的 SDK 编译上下文。
python -m pytest firmware/tests
cd firmware
python -m platformio run -e esp32_p4_wifi6_system_settings_v1
python -m platformio run -e esp32_p4_wifi6_system_settings_v3
python -m platformio run -e esp32_p4_pico_v1
python -m platformio run -e esp32_p4_pico_v3
```

首次构建会下载相应平台、ESP-IDF 与锁定组件。v1/v3 使用不同的平台版本，保留各自 SDK 上下文与编译数据库；不要将一个目标的生成配置复制到另一个目标。Windows 建议使用纯 ASCII 短路径和 PowerShell。缺少依赖或预算检查失败应修正实际输入，不能用空文件跳过检查。这些命令仅编译，不烧录设备。

## 配套桌面资源

每个环境的应用输出为 `firmware/.pio/build/<环境名>/firmware.bin`。将四份经过核验的应用镜像按上表分别放入 `pc/src-tauri/firmware/esp32-p4/`，再准备桌面资源。四份镜像必须来自同一份已审核源码，板型、芯片修订范围、版本、构建标识和 SHA-256 均需核对；不要把完整 factory 镜像作为 OTA 输入，也不要沿用旧版本文件。

使用发布版 OTA 时，在仓库根目录运行 `python pc/scripts/stage-release-firmware.py`，脚本会下载四个完整烧录包，提取对应的应用镜像，并核对压缩包及镜像的 SHA-256、板型和芯片修订范围。出厂形象与 **27 个组件**由 [factory-config.json](factory-config.json) 指定。三款新增 WASM 组件的完整模块随出厂存储镜像和桌面客户端提供，OTA 镜像保留其目录信息。

## 完整出厂镜像

以下示例只构建 WiFi6 v3；其他三种目标应分别替换环境名和输出目录：

```sh
# firmware 目录
python -m platformio run -e esp32_p4_wifi6_system_settings_v3 -t factory
# 回到仓库根目录
cd ..
node pc/scripts/package-factory-release.mjs \
  --build-dir firmware/.pio/build/esp32_p4_wifi6_system_settings_v3 \
  --output /path/to/empty-output-directory
```

完整包包含 Bootloader、分区表、应用、OTA 元数据、默认形象与组件、校验清单和双平台烧录入口。仅有 `firmware.bin` 不是完整烧录包。分别核验四个目标的文件、芯片范围、分段布局和 SHA-256；保持桌面安装包与对应固件资源一致。生成镜像和 ZIP 不等于已完成实机烧录验收。

## 升级、分区与接线

日常客户端 OTA 仅更新应用分区，保留用户数据，不修改分区表。完整出厂烧录**会清除设备设置、自定义形象与组件**；需要保留数据时先备份。不要仅覆盖分区表，或回写不匹配的 SPIFFS 镜像。

当前 [partitions.csv](partitions.csv) 的 32 MiB 布局为双 OTA 各 3 MiB、storage 8.875 MiB、appearance0/1 各 8.5 MiB。旧设备普通 OTA 不会获得新的分区容量。应用镜像须保留至少 128 KiB OTA 余量；分区容量不能代替堆、DMA、栈和帧耗时检查。

屏幕接 **DSI**，不是 CSI，插拔排线前断电。原生 USB 与 USB-UART 的选择取决于板卡切换电路，见 [USB 连接方式](README.md#usb-连接方式)。

## 验证与发布边界

主机测试验证模型与协议，目标编译验证配置、链接资源和静态预算。输入、触控、显示、连续传输、OTA/恢复以及录放音、AEC 和真实听感仍需分别进行硬件验证。未运行、跳过或缺硬件的项目不表示通过。

修改传输代码时应区分明确拒绝与结果未知。超时不允许自动重放变更、清除旧事务或把重连当作提交成功；先通过只读状态与身份核对确定后续操作。

## 完整主机检查的 SDK 与 WASM 输入

CI 使用 Python 3.12、Node.js 22、PlatformIO Core 6.1.19，以及 LLVM/LLD **20.1.8**。`CLANG` 指定该版本的 clang，配套 wasm-ld 位于其工具目录。生成的 WASM 与技能镜像按字节检查，不应更换编译器后重写产物绕过验证。

完整测试会读取锁定 SDK 的源码、目标编译器和各环境的编译数据库。[tools/prepare_ci_sdk.py](tools/prepare_ci_sdk.py) 在 GitHub Actions 中准备 v1/v3 PlatformIO core，通过 `compiledb` 生成上下文并将路径写入 `GITHUB_ENV`；它不连接或烧录设备。本机测试同样需要相应输入，将 `PET_P4_V1_COMPILE_COMMANDS`、`PET_P4_V3_COMPILE_COMMANDS` 指向真实编译数据库，并提供测试要求的 SDK 路径。不要使用手写数据库或空 SDK 替代。
