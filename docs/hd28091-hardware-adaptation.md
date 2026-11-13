# HD28091 硬件改动与固件适配

本分支基于 HachimoDock 公开 R181 源码，适配 HD28091C40-V1 屏幕及自制转接板，并提供微雪 ESP32-P4-WIFI6 和 ESP32-P4-Pico 的外接按键配置。本文记录截至 2026-11-13 的实现与联调结果。

上游 0.1.217 发布包搭配 R206 固件。其发布清单的 `sourcePublished` 为 `false`，本分支构建版本由 `firmware/CMakeLists.txt` 和 `firmware/platformio.ini` 中的 R181 基线生成。[上游发布清单](https://github.com/ai-maker-lab/HachimoDock/blob/main/latest-release.json)

## 屏幕与转接板

显示屏采用 HD28091C40-V1，物理分辨率 480×640，界面使用 640×480 横向逻辑坐标并旋转输出。屏侧为 40Pin LCD 排线和独立 6Pin 触摸排线。[屏幕淘宝购买链接](https://item.taobao.com/item.htm?id=566355265504&mi_id=0000sGhHLYPMQHlue9llXrv9IkNEE6YnqL7G8TRNHJ512as&spm=tbpc.boughtlist.suborder_itemtitle.1.70d82e8dXaiCDY)

转接板工程及检查记录在 [hardware/screen-adapter](../hardware/screen-adapter/README.md)，包括嘉立创EDA原生工程、原理图、PCB、BOM、网表及预览。

转接板的在线工程位于立创开源硬件平台。[转接板工程](https://oshwhub.com/fosen/project_tzekpmhv)

| 项目 | 当前实现 |
| --- | --- |
| 主板连接 | 22Pin DSI 排线，按同编号引脚导通连接 |
| LCD 供电 | 主板 3.3V 经 ME6211C30M5G-N 生成 3.0V |
| 背光驱动 | SY7200AABC 恒流升压，R1＝5.6Ω，按 0.2V 反馈计算约 35.7mA |
| 背光使能 | EN 接输入电源，固定开启 |
| LCD 复位 | R2＝10kΩ、C6＝1µF 的 RC 电路，配 BAT54WS 掉电放电 |
| 触摸复位 | 独立 R3＝10kΩ、C8＝1µF 的 RC 电路，配 BAT54WS |

背光电流的计算值取决于实装电阻和反馈电压，额定电流匹配及均匀性需要结合屏厂规格与实测。RC 时间常数约 10ms，有效复位脉宽取决于电源上升过程和输入门限。

GPIO26、GPIO27 在原 BSP 中分别定义为背光和 LCD 复位控制引脚。微雪主板原理图将这两个 GPIO 引出到 40Pin 排针，屏侧控制路径应结合具体转接板核对。本转接板使用上述固定背光和 RC 复位电路。[微雪主板原理图](https://files.waveshare.com/wiki/ESP32-P4-WIFI6/ESP32-P4-WIFI6-datasheet.pdf)

## LCD 固件参数

本地 BSP 使用屏厂提供的 `BOE2.8IPS(ZV028V0Q-N10-1QP0 )_ADS(480X640)_FW(Customer)-45Duty-190802.ini.txt` 初始化参数，完整命令表位于 `firmware/components/esp32_p4_wifi6_touch_lcd_4_3/esp32_p4_wifi6_touch_lcd_4_3.c`。初始化前等待 1 秒，软件复位后等待 120ms，Sleep Out 后等待 120ms。

| 参数 | 设置 |
| --- | --- |
| DSI 数据通道 | 2 路，每通道 500Mbps，按转接板 D0/D1 接线配置 |
| 像素格式 | RGB565，RGB 顺序 |
| 水平有效像素 / HBP / HSYNC / HFP | 480 / 20 / 4 / 328，总计 832 像素周期 |
| 垂直有效行 / VBP / VSYNC / VFP | 640 / 16 / 4 / 14，总计 674 行 |
| v1 DPI 像素时钟 | 25MHz，计算刷新率约 44.6Hz |
| v3 DPI 像素时钟 | 20MHz，计算刷新率约 35.7Hz，作为暗角问题的降频对比设置 |

水平总长根据初始化表 `C2` 的 `RTNI=0x14`，按 `512 + 16×RTNI` 得到 832；外部 VBP/VFP 满足文件所列的至少 16/14 行约束。v3 的上一版对比参数为 24MHz，约 42.8Hz。主机时序组合与模组内部通道配置仍需目标屏实测确认。

固件通过 DCS Display Off/On 控制初始化后的画面显示，并在首帧提交后显示画面。背光从上电起持续工作，相关电路见转接板资料。

## CST328 触摸

| 触摸接口 J3 | 连接 |
| --- | --- |
| 1 GND | GND |
| 2 RESET | 转接板 TP_RST RC 电路 |
| 3 INT | 默认悬空，使用 15ms 轮询。可选飞线至主板 GPIO22 并启用中断配置 |
| 4 SDA | J1.2，主板 GPIO7 |
| 5 SCL | J1.3，主板 GPIO8 |
| 6 VDD | 3.3V |

驱动使用 7 位地址 `0x1A` 和 16 位寄存器，校验信息模式 `0xCACA` 协议标识及 480×640 坐标范围。单点报文、抬起、多点取消、读后 ACK 和错误恢复分别处理，坐标转换为横向界面坐标。

默认将 `BSP_LCD_TOUCH_INT` 设为 `GPIO_NUM_NC`，触摸任务每 15ms 通过 I²C 查询，适用于 INT 未连接的硬件。通信失败时取消手势，并在后续轮询中继续读取。

使用中断时，将 TP_INT 飞线接 GPIO22，并在所选 PlatformIO 环境的 `build_flags` 中追加 `-DBSP_LCD_TOUCH_INT=22`。GPIO22 使用上拉输入和下降沿中断，ISR 通知触摸任务，I²C 读取在任务中执行。启动时同步一次，700ms 静止长按由任务计时。通信失败时取消手势并追加最多两次重试，随后尝试 ACK 清理。

诊断字段 `touchHealth.interruptGpio` 在成功初始化后，轮询模式为 `-1`，上述中断配置为 `22`。结合 `stage=ready` 确认初始化完成，`probe0x1a` 用于定位触摸地址应答情况。

## WIFI6 与 Pico 按键

以下映射适用于外接按键板沿用两块主板的相同排针位置。

| 功能 | ESP32-P4-WIFI6 | ESP32-P4-Pico |
| --- | --- | --- |
| SW1 | GPIO50 | GPIO14 |
| SW2 | GPIO49 | GPIO6 |
| SW3 | GPIO5 | GPIO5 |
| 编码器中键 / B / A | GPIO4 / 3 / 2 | GPIO4 / 3 / 2 |
| 摇杆 X / Y | GPIO21 / 20 | GPIO21 / 20 |
| CST328 INT 可选飞线 | GPIO22，默认悬空轮询 | GPIO22，默认悬空轮询 |

`PET_P4_BOARD_PICO=1` 选择 Pico 配置。输入采样与握手中的 `capabilities.controls.sw1/sw2` 共用 `pet_p4_input.h` 的定义。Pico 构建版本为 `0.7.103-p4-r181-pico`。[Pico 引脚图](https://docs.waveshare.com/ESP32-P4-Pico#pinout-definition) [WIFI6 引脚图](https://docs.waveshare.com/ESP32-P4-WIFI6#pinout-definition)

| 板型 | 芯片 v1 构建目标 | 芯片 v3 构建目标 |
| --- | --- | --- |
| WIFI6 | `esp32_p4_evboard` | `esp32_p4_evboard_v3` |
| Pico | `esp32_p4_pico` | `esp32_p4_pico_v3` |

依赖与芯片修订范围见 [构建说明](../firmware/BUILD.md)。在 `firmware` 目录构建 Pico v3 应用及完整出厂镜像可执行以下命令。

```sh
python -m platformio run -e esp32_p4_pico_v3
python -m platformio run -e esp32_p4_pico_v3 -t factory
```

完整镜像包含启动程序、分区表、应用、OTA 元数据、默认形象和 29 个组件。整板初始化会覆盖设备现有程序、配置与资源。已有本项目固件的设备可依据实际备用分区容量，通过 A/B 应用升级保留数据。

## 外壳与打印工程

整套外壳文件与打印说明集中在 [hardware/enclosure-hd28091](../hardware/enclosure-hd28091/README.md)。当前保留方案一 R10，包含 R10 外角和约 R4.5 面框边缘，摇杆凸台采用抬高前的位置。前壳打印入口为 [screen-up STL](../hardware/enclosure-hd28091/HD28091-option1-R10-screen-up.stl) 和 [screen-up 3MF](../hardware/enclosure-hd28091/HD28091-option1-R10-screen-up.3mf)，文件与选定版本保持一致。后壳、三个按键帽及摇把的三个零件已从原总装中独立导出，整套共 8 件，每件打印 1 个。同目录提供各件 STEP、使用当前 R10 前壳的总装 STEP、FreeCAD 工程、零件预览和来源核验清单。前壳 CAD 为网格转实体，配套件的切片与实物装配验证待完成。

## 验证记录与待测项

2026-11-07 的 v3.2 联调已完成 WIFI6/Pico 对应固件的构建、串口 A/B 升级及新板完整初始化；设备通过镜像校验和启动健康检查。20MHz 对比固件通过运行中 ELF 摘要核对。Pico 版本上报 SW1/SW2/SW3＝GPIO14/6/5。

2026-11-13 在 Pico v3.2 设备 `p4-80f1b2d7ba1a` 上写入默认 15ms 轮询固件，串口 A/B 升级通过镜像 SHA-256 校验，新应用从 `ota_1` 启动并达到 `imageState=valid`。连续查询仍为 `touchReady=false`、`stage=probe`、`probe0x1a=ESP_ERR_NOT_FOUND`，采样数和触点数均为 0，音频地址 `0x18` 应答正常。

主机回归覆盖触摸寄存器、IRQ 唤醒与错误恢复、固定背光显示门控及诊断分类；协议检查覆盖板型引脚与握手信息的一致性。

当前待实测事项包括屏幕四角发暗的纯色对比、LCD 供电与背光电流、RC 复位波形、实体按键触发及 v1 板型。触摸需要继续核对独立 6Pin 排线、J3 接线、供电及复位，待地址探测和初始化成功后验证实物触摸响应。
