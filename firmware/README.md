# HachimoDock ESP32-P4 固件

当前源码版本 **0.7.107-p4-r292 / R292**，配套桌面端 **0.1.245**。完整烧录包见 [发布页](https://github.com/ai-maker-lab/HachimoDock/releases/latest)。

本目录是 HachimoDock 当前使用的 ESP32-P4 固件源码，与 [PC 客户端](../pc/) 配合使用：PC 负责 Agent 状态监听、语音识别服务对接、素材预处理与传输；设备负责 USB 通信、受支持板型的 Wi-Fi／蓝牙、按键与摇杆输入、屏幕渲染、形象播放及组件运行。

为设备安装固件，请下载 [完整烧录包](https://github.com/ai-maker-lab/HachimoDock/releases/latest)，解压后按中文指南操作。已能连接 Pet Manager 的设备，可直接使用客户端内的“固件升级”。需要自行编译时，请参考 [构建与烧录指南](BUILD.md)。

## 目标硬件

| 项目 | 配置 |
| --- | --- |
| 主控 | ESP32-P4，RISC-V 双核；高性能核心 v1 为 360 MHz，v3 为 400 MHz；低功耗核心为 40 MHz |
| 片上内存 | 768 KB L2MEM、32 KB 低功耗 SRAM、8 KB TCM |
| PSRAM | 32 MB 封装内堆叠内存 |
| Flash | 32 MB QSPI NOR Flash |
| 显示屏 | HD28091C40-V1，480 × 640 ST7701S MIPI 屏幕；固件使用 640 × 480 横向逻辑画布，再旋转输出 |
| 输入 | SW1 / SW2 / SW3、摇杆；触控支持 CST328、CST3530、GT911 与按芯片 ID 识别的 FT6336U / CST826，是否可用以实机 `touchReady` 为准 |
| USB | Type-C，可通过板上切换电路连接 CH343 USB-UART 或 ESP32-P4 原生 USB HS OTG |

板型、引脚与分区配置以 [boards/](boards/)、[sdkconfig.defaults](sdkconfig.defaults) 和 [partitions.csv](partitions.csv) 为准。不同硬件版本烧录前需核对配置。

### HD28091C40-V1 屏幕适配

当前工作树采用 Rev A 转接板，触摸 J3 第 3 脚默认悬空。LCD 和触摸各自通过 RC 上电复位，初始化前等待至少 1 秒，LCD 软件复位后再等待 120 ms。触摸通过 GPIO7/SDA、GPIO8/SCL 通信，默认每 15 ms 轮询一次。CST328 使用 `0x1A` 和 480×640 坐标。CST3530 使用 `0x58`，将 240×320 触摸坐标缩放并旋转到 640×480 横向界面。[Rev A 转接板](../hardware/screen-adapter/README.md)

轮询路径保留 150 ms 鲜度检查，有效抬起、多点或通信错误结束当前手势。CST328 可选将 TP_INT 飞线接到 GPIO22，并在构建标志追加 `-DBSP_LCD_TOUCH_INT=22`。此时 ISR 通知触摸任务执行 I²C 读帧，静止按住的 700 ms 长按由任务计时触发，通信失败时最多追加两次读取重试。寄存器协议依据 Hynitron 数据手册，读后确认及消费标识处理参考公开驱动。[CST328 数据手册](https://www.cdtech-display.com/wp-content/uploads/2024/07/CST328-DataSheet-V2.2.pdf) [微雪 CST328 驱动](https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/display/touch/esp_lcd_touch_cst328/esp_lcd_touch_cst328.c) [Linux Hynitron 驱动](https://github.com/torvalds/linux/blob/master/drivers/input/touchscreen/hynitron_cstxxx.c)

初始化表来自用户提供的 `BOE2.8IPS(ZV028V0Q-N10-1QP0 )_ADS(480X640)_FW(Customer)-45Duty-190802.ini.txt`，包含厂方电压、Gamma、GIP 参数及 `11 → 等待120ms → 36 00 → 35 00 → 29` 的结束顺序。外部 VBP/VFP 分别设置为 16/14 行，满足文件开头的约束。

根据转接板两对数据线配置双通道 DSI，初始速率为每通道 500 Mbps。水平总长按厂方 `C2` 的 `RTNI=0x14` 换算为 832 像素周期，HBP/HSYNC/HFP 分别为 20/4/328。v1 像素时钟采用 25 MHz，v3 当前使用可整除的 20 MHz 做降频对比，计算刷新率约为 44.6/35.7 Hz；v3 的上一版为 24 MHz、约 42.8 Hz。厂方文件没有完整主机时序和模组内部 LANSEL 配置，上述链路参数仍需实机验证。计算依据见 ST7701S 手册第 9、39、271 页。[ST7701S 数据手册](https://focuslcds.com/wp-content/uploads/Drivers/ST7701S.pdf)

Rev A 背光 EN 接电源，亮度固定。固件在 LCD 初始化后通过 DCS 关闭画面，在首帧绘制完成后开启画面，上电至软件接管期间可能可见亮屏。`backlightStatus` 记录该显示门控命令的执行结果。实机验收需覆盖冷启动、MCU 重启、彩条颜色与边界，以及触摸四角、拖动和抬起。`diagnostics/query` 的 `touchHealth` 提供 `probe0x1a`、`probe0x58` 和已就绪的控制器名称。

中断固件的 `touchHealth.interruptGpio` 应为 `22`，启动日志包含 `mode=interrupt int_gpio=22`。飞线完成后需实测轻触、静止长按、连续拖动、抬起和冷启动期间的触摸，核对目标屏的完整中断报文时序。

ESP32-P4-WIFI6-M 的 M 表示焊接排针，不代表芯片版本。v1 与 v3 使用不同固件镜像，同一种芯片的镜像由 Windows/macOS 共用；v3 的构建与实机验收要求见 [BUILD.md](BUILD.md)。

## 已支持的功能

- **USB 通信与设备识别**：USB-UART / USB Serial/JTAG 的 JSON Lines 握手，以及 `pet-usb-native-v1` 原生 USB 协议。设备身份由基础 MAC 派生，上报实际能力、固件版本、构建标识及协议版本。
- **会话气泡与形象**：接收 Agent 状态、进度和语音文本，按当前选中气泡的状态播放形象；会话结束后气泡保留 60 秒。
- **动画与音效**：播放预处理后的 H.264 Annex-B 动画及受限 PCM WAV 提示音，保留旧 MJPEG 素材兼容路径。
- **声明式游戏和工具**：支持组件状态持久化、移动与碰撞规则、PNG 精灵、动画和分层绘制；不执行组件自带的任意 JavaScript。
- **组件安装与恢复**：分块校验，文件与目录采用独立 A/B 代际提交，完整校验后生效；启动时选择有效版本，必要时回退。
- **可配置输入**：按键、长按与摇杆动作可由 PC 配置并保存至 NVS，跨重启保留；组件退出始终遵循全局返回设置。
- **设备麦克风输入**：通过 USB 传输 16 kHz、单声道 S16LE 音频；有 ES7210 时使用其采集，否则走 ES8311 路径。识别和 Agent 输入由 PC 完成。
- **A/B 固件升级**：分块确认、SHA-256 校验、ESP-IDF 项目标识校验、启动健康确认和回滚。回执超时不等于写入失败；未知结果只读核对，不自动重放写入。
- **传输异常恢复**：可查询断线后残留的 OTA 事务，明确确认后再处理；传输期间显示轻量传输画面，未完成的素材不会替换有效形象。
- **诊断与截图**：查询重启原因、堆内存、PSRAM、SPIFFS、输入及音频状态，并通过协议获取调试截图。

### 当前功能边界

- WiFi6 固件支持同局域网配对、Agent 跟随及实时对话按键和状态传输，USB 在线时优先使用 USB；设备仍需供电，PC 协同功能仍需管理端运行。Pico 固件不包含 Wi-Fi／蓝牙栈。
- 设备不直接播放 MP4；PC 先生成设备可用的 H.264 素材包。
- 设备不运行完整 HTML / JavaScript 页面，也不接受组件自带的任意网络读取器。
- ASR / TTS 不在设备本地执行，需要 PC 端及用户配置的服务。
- 原生 USB 可复用已缓存的形象包；包含 H.264 / MJPEG 的首次完整素材下发仍使用 USB-UART。

## 页面与默认操作

ESP32-P4-Pico 使用官方 `esp32_p4_pico_v1`（v1）或 `esp32_p4_pico_v3`（v3）构建，旧的 `esp32_p4_pico` 保留为 v1 别名。SW1/SW2 分别对应 Pico 的 GPIO14/GPIO6，WIFI6 对应 GPIO50/GPIO49。SW3 为 GPIO5，编码器中键/B/A 为 GPIO4/3/2，摇杆 X/Y 为 GPIO21/20。触摸默认轮询，可选 INT 引脚为 GPIO22。握手中的 `capabilities.controls.sw1/sw2` 上报所选板型的实际引脚。[Pico 引脚图](https://docs.waveshare.com/ESP32-P4-Pico#pinout-definition) [WIFI6 引脚图](https://docs.waveshare.com/ESP32-P4-WIFI6#pinout-definition)

宠物主界面 `main` 和组件中心 `components` 是同级页面；组件运行页 `app` 是组件中心的子页面。宠物页面不显示顶部页面指示圆点。

| 操作 | 默认行为 |
| --- | --- |
| SW1 短按 | 确认 / 进入 |
| 按住 SW1、松开 | 录音、结束并写入 Agent 草稿；确认后才发送 |
| SW2 短按 | 切换宠物主界面与组件中心；组件运行中若绑定了 SW2，则执行组件动作 |
| SW2 长按 | 开始 / 结束实时对话 |
| SW3 短按 | 返回 / 取消；组件内按全局返回动作退出 |
| SW3 长按 | 打开系统设置 |
| 摇杆中键短按 | 确认 |
| 摇杆中键长按 | 不绑定 |
| 摇杆向上 / 向下 | 上一个 / 下一个；主界面选择会话气泡，组件中心选择组件 |
| 摇杆方向（组件内） | 按当前组件的动作映射执行 |

以上均为默认值，用户可在 PC 端修改。组件包只定义游戏或工具动作，不应另行定义固定退出键，也不能覆盖全局退出配置。输入经过软件消抖、长按识别及有界事件队列处理，避免阻塞渲染与传输。

## 默认组件

出厂配置包含 **27 个组件**，设备组件目录最多支持 40 个可见组件。顺序及初始选择以 [factory-config.json](factory-config.json) 为准。当前初始选择为城市电台，设备开机仍进入宠物主界面。

| 顺序 | 名称 | ID |
| --- | --- | --- |
| 1 | 城市电台 | `city-radio` |
| 2 | 墨线五子棋 | `inkline-gomoku` |
| 3 | 静心木鱼 | `wooden-fish` |
| 4 | 自选股行情 | `stock-watchlist` |
| 5 | 模拟琴键 | `wasm-piano` |
| 6 | 消星星 | `cozy-pop-stars` |
| 7 | 中国象棋 | `xiangqi` |
| 8 | 像素贪吃蛇 | `snake-turn` |
| 9 | 2046 | `wasm-2046` |
| 10 | 云端跳一跳 | `cloud-hop` |
| 11 | 今天吃点什么 | `today-meal-menu` |
| 12 | 成语笺 | `chengyu-jian` |
| 13 | 鲜果一刀 | `fresh-fruit-slice` |
| 14 | 每天一本书 | `book-pocket` |
| 15 | 像素方块 | `block-combo` |
| 16 | 番茄钟 | `tomato-clock` |
| 17 | 像素飞鸟 | `flappy-bird` |
| 18 | 每日新知 | `hachimi-learn` |
| 19 | 自然声景 | `hachimi-sounds` |
| 20 | 双键接球 | `two-key-pong` |
| 21 | 电脑状态 | `computer-status` |
| 22 | 喝水提醒 | `drink-reminder` |
| 23 | 灵感胶囊 | `hachimi-notes` |
| 24 | Token 仪表盘 | `token-usage` |
| 25 | 纠结扭蛋机 | `decision-capsule` |
| 26 | 答案之书 | `answer-book` |
| 27 | 盼头列车 | `hope-train` |

普通 OTA 更新应用内的内置组件资源，尊重已持久化的用户卸载记录；不会为了恢复出厂列表而删除用户组件或自动补回用户主动移除项。完整烧录则重新初始化出厂资源。依赖 PC 数据或媒体的组件需要客户端保持运行，内容源与可用性以对应组件配置为准。

## 构建固件

安装 Python 3.10+、PlatformIO Core 6.1.19、Node.js 22 和素材工具依赖，按 [BUILD.md](BUILD.md) 准备锁定 SDK、WAMR 与资源。公开目标区分板型和芯片修订版：

| 板型 / 芯片 | 构建环境 | 串口运行参数 |
| --- | --- | --- |
| WiFi6 v1 | `esp32_p4_wifi6_system_settings_v1` | 4 Mbaud |
| WiFi6 v3 | `esp32_p4_wifi6_system_settings_v3` | 4 Mbaud |
| Pico v1 | `esp32_p4_pico_v1` | 4 Mbaud |
| Pico v3 | `esp32_p4_pico_v3` | 4 Mbaud |

例如在仓库根目录进入 `firmware` 后执行 `python -m platformio run -e esp32_p4_wifi6_system_settings_v3`。四个目标的完整命令和桌面 OTA 文件映射见 [构建指南](BUILD.md)。同一板型和芯片的应用镜像由 Windows/macOS 共用，WiFi6/Pico、v1/v3 不可混刷。

## 烧录与升级

### 日常升级

已完成出厂初始化的设备，优先使用 Pet Manager 内的固件升级。普通 OTA 只写应用分区，保留形象、用户组件和配置；新固件启动后执行内置组件迁移。

串口救援应按 [构建与烧录指南](BUILD.md) 选择实际板型与芯片；先核对设备分区布局，并在写入后核验版本和镜像。不要将旧工具的通用 v1 默认值当作四板通用目标。

### Windows 通用开发板与旧分区维护

以下 PowerShell 命令适用于 `esp32_p4_evboard` 通用开发板。从 `firmware` 目录运行，先核对板型、芯片、分区表和端口。WiFi6 或 Pico 成品板请选对应的完整烧录包或客户端 OTA。

```powershell
.\tools\p4.ps1 build
.\tools\p4.ps1 flash -Port COM5
```

如需将兼容的早期通用开发板恢复至出厂状态，先备份设备数据，再使用 `.\tools\p4.ps1 factory-flash -Port COM5 -FactoryReset`。仍使用旧版单应用分区和 16 MiB SPIFFS 的通用开发板可用 `.\tools\migrate_to_ab.ps1 -Port COM5 -FactoryReset` 迁移。工具会先备份旧 SPIFFS，再执行完整烧录。旧文件系统不会原样写回较小的新分区。

### 首次烧录 / 恢复出厂

首次安装应使用**完整烧录包**，不能只刷应用分区的 `firmware.bin`。完整包包含引导程序、分区表、固件、OTA 元数据、西高地形象、内置组件、校验清单及烧录工具。

生成出厂镜像：

```sh
cd firmware
# 示例仅用于 WiFi6 v3；其他板型使用上表对应环境。
python -m platformio run -e esp32_p4_wifi6_system_settings_v3 -t factory
```

默认输出为 `.pio/build/<所选环境>/pet-manager-p4-factory.bin`，附带记录 SHA-256 和分段布局的 JSON 清单。生成可分发 ZIP 的步骤见 [构建与烧录指南](BUILD.md)。

**出厂烧录会覆盖设备原有配置、形象和组件，请先备份。** 使用与板型、芯片修订版相符的公开完整包及其说明；不要运行未指定目标的通用烧录命令。旧分区迁移需单独核验布局和备份，普通 OTA 不会修改分区表。

## Flash 分区

实际配置以 [partitions.csv](partitions.csv) 为准：

| 分区 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | `0x009000` | 24 KiB | 设备与输入配置 |
| `phy_init` | `0x00F000` | 4 KiB | PHY 初始化数据 |
| `ota_0` | `0x010000` | 3 MiB | A/B 应用镜像 |
| `ota_1` | `0x310000` | 3 MiB | A/B 应用镜像 |
| `otadata` | `0x610000` | 8 KiB | A/B 启动选择元数据 |
| `storage` | `0x620000` | 8.875 MiB | SPIFFS 清单、音效、组件及元数据 |
| `appearance0` | `0xF00000` | 8.5 MiB | 受保护的西高地内置动画 |
| `appearance1` | `0x1780000` | 8.5 MiB | 可替换的自定义动画 |

旧20 MiB形象布局（storage大小`0x6E0000`、形象槽位于`0xC00000`/`0x1600000`）与旧18 MiB布局都需要显式 USB 重新部署分区。需要保全数据时先备份并迁移；用户明确允许清空的测试机可直接完整初始化。不能仅覆盖分区表或把旧 SPIFFS 镜像当成新文件系统。普通 OTA 只更新应用，不更新分区表，也不会自动迁移。新布局保持组件空间比原始布局多 2 MiB，双 OTA 各增加 512 KiB，形象合计 17 MiB；默认西高地所有 16 段原始视频均保留，不重编码。

正常启动不会自动格式化 SPIFFS。挂载失败时保留数据并记录错误，避免暂时性故障造成素材和组件被静默删除。

## USB 连接方式

对于采用相应 USB 切换电路的 Waveshare ESP32-P4-WIFI6 板：

- 跳线断开：Type-C 连接 CH343 USB-UART，用于烧录、串口日志、调试及完整形象下发。
- 跳线短接：Type-C 连接 ESP32-P4 原生 USB HS OTG，用于 TinyUSB 自定义批量传输。

切换的是数据通路，不改变供电输入；实际操作请核对所用板卡说明。原生接口通过 Microsoft OS 2.0 描述符匹配系统 WinUSB，无需单独安装 INF 或 Zadig。

Windows/macOS 共用 4 Mbaud 固件。macOS 保留 8 KiB 逻辑分块及分段节流；Windows 保留原有分片与发送间隔，可从较大的 raw 分块开始，按当前协商结果选择分块；超时或结果未知时不自动重启事务。PC 仍可探测连接旧 2 Mbaud 固件。运行波特率与烧录波特率分别设置，实际吞吐以日志统计为准。

原生 USB 打开目标前使用随机挑战值核对设备身份和协议版本；身份不唯一、回复过期或不支持身份检查时拒绝写入，可改用 USB-UART。

## 通信协议

完整字段与交互约定见 [protocol.md](protocol.md)。协议标识保留英文，以便与代码对应：

| 消息 | 用途 |
| --- | --- |
| `hello` | 设备身份、版本与能力握手 |
| `state/<agent>` | Agent 生命周期状态 |
| `session/current` | 当前选中会话及气泡队列快照 |
| `speech/text` | 气泡标题、正文与状态 |
| `control/screen-page` | 页面控制 |
| `stats/update` | Token 等受限统计数据 |
| `input/config` | 校验并持久化全局按键映射 |
| `widget/begin` / `widget/chunk` / `widget/commit` | 分块安装组件 |
| `widget/list` / `widget/delete` | 查询或删除组件 |
| `miniapp/event` / `miniapp/query` | 组件动作及运行状态查询 |
| `asset/*` | 形象素材传输、校验与提交 |
| `firmware/*` | 固件升级、状态查询、中止与提交 |
| `debug/screenshot` | 分块返回调试截图 |
| `diagnostics/query` | 查询重启、资源及外设状态 |
| `system/reset-inputs` | 重置输入配置，不删除素材 |
| `system/reboot` | 确认后延迟重启 |

`hello` 和 `diagnostics/status` 上报 `buildId`、`gitSha`、`buildDirty` 及 `protocolSchema`，用于区分同版本号的不同构建；缺失字段视为未知。

未知或不支持的消息返回明确 NACK；请求包含 `requestId` 时，回复回传该标识。PC 应按设备上报的能力启用功能，不应仅凭版本号猜测。

## 形象素材格式

PC 在形象创建、导入或视频替换后预处理素材；同步时只读取并校验结果，不再临时转码：

```text
p4/
├── manifest.json
├── families/sha256-<digest>.h264
└── audio/<family>.wav
```

- 动画采用 `p4-h264-v1`、H.264 Annex-B 和 Baseline 兼容参数。
- 画布为 640 × 480，按原比例缩放并补黑边，不拉伸图像。
- 采样上限为 15 FPS，每个动作最多 225 帧，按原视频时长分配帧。
- 相同源视频复用同一个哈希路径，避免重复下发。
- 提示音为 16 kHz、单声道、16 位 PCM WAV，每条最大 1 MiB。
- 设备采用受限 PSRAM 缓存和逐帧解码，减少每帧 Flash 读取；清单只在加载素材时解析。
- 动画存入专用形象分区，清单和音效存入 SPIFFS；数据校验后才提交索引和就绪标记。

## 测试与设备自检

在仓库根目录执行主机测试：

```sh
python -m pytest firmware/tests -q
python firmware/tests/protocol_contract_test.py
python firmware/tools/build_builtin_bundle.py --check
```

Windows 也可使用 `.\tools\p4.ps1 test`，再运行 `build`。源代码测试不会自动烧录设备；物理设备测试及清空操作需手动执行。

烧录后建议检查：

- 版本、构建标识、分区布局与预期一致。
- 日志出现 `loaded P4 asset manifest`、`P4 asset cache loaded`，动画持续正常播放。
- H.264 路径出现 `P4 H264 decoder=software-dual output=I420`；旧 MJPEG 包可能显示硬件 JPEG 解码日志。
- `debug/lcd` 的初始化、背光和渲染状态正常；截图不全黑，连续截图校验值随画面变化。
- 按键、摇杆、触摸及音频采集正常，页面切换及组件退出符合全局配置。
- 中断传输后可重新连接、查询状态并恢复更新，原有效素材保持可用。

## 字体与许可

设备界面使用 MiSans Medium，来自[小米澎湃 OS 官方字体网站](https://hyperos.mi.com/font/zh/)，遵循 MiSans 字体知识产权许可协议。仓库不分发原始 OTF 文件，包含为设备生成的 16 px、1 bpp 中文字形数据。

项目自有源码采用根目录 [HachimoDock 项目许可证](../LICENSE)，仅限许可范围内的非商业用途；第三方驱动、字体与依赖继续遵循各自许可证，见 [第三方声明](../THIRD_PARTY_NOTICES.md)。

媒体切图升级：0.7.79-p4 提供 widgetSurfaceImages=p4-surface-images-v1，支持有界调色板压缩图片与播放状态驱动旋转。图片由组件包提供，不内置电台专用美术。

Wi-Fi 6 firmware uses one P4 image per supported P4 silicon revision. The Hosted handshake recognizes the standard ESP32-C6 and the DB board’s ESP32-C5. The C5 must already carry compatible ESP-Hosted coprocessor firmware. The normal P4 OTA does not flash it. Validate the same P4 image on both boards before advertising DB support.
