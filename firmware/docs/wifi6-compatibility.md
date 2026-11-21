# Wi‑Fi 6 固件兼容与 v1.3 启动回归

## 主控修订与镜像

| 项目 | Wi‑Fi 6 v1 | Wi‑Fi 6 v3 |
| --- | --- | --- |
| ESP32-P4 修订 | 0.1–1.99，含 v1.3 | 3.0–3.99，含 v3.2 |
| ESP-IDF / CPU | 5.5.1 / 360 MHz | 5.5.4 / 400 MHz |
| H.264 1.5.0 库目录 | `sw/libs/esp32p4_less_v3/` | `sw/libs/esp32p4/` |
| 链接器 | v1 编译链通过 `-B` 指定兼容的 `ld` | v3 工具链默认 `ld` |
| OTA 应用资源 | `firmware.bin` | `firmware-v3.bin` |

两套发布目标共享应用功能、分区布局、ESP-Hosted 2.12.11、esp_wifi_remote 1.6.3 和 SDIO 参数。v1 的部分冷启动和控制代码单独做体积优化，音频 PCM、DSP 和传输热路径保留各自的编译配置。选包使用实际板型、P4 修订和镜像头。Pico 有独立板型和镜像，板型不明时先确认再升级。

## 反复重启的两条已知路径

- **早期 P4 启动复位。** ESP-IDF 5.5.1 缺少 esp_h264 1.5.0 使用的修订选择条件。早期 P4 需要显式链接 `esp32p4_less_v3` 的 tinyH264 和 OpenH264，并用兼容链接器处理厂商汇编的硬件循环重定位。
- **欢迎动画首帧异常。** RTC RAM 即使地址对齐也不支持 P4 的 128 位 SIMD 访问。两套 Wi‑Fi 6 镜像在项目内构建 tinyH264，三种分配入口的所有候选堆都要求 `MALLOC_CAP_SIMD`。两套镜像均采用串行解码和水平亮度滤波的标量实现。

升级 SDK、重新导入发布分支或调整 H.264 依赖时，检查以下项目。

1. 用正式的 Wi‑Fi 6 v1 构建入口生成镜像。日志和最终链接结果要能追溯到两份 pre-v3 库，tinyH264 经项目级 SIMD 分配替换后实际进入链接。兼容 linker 目录只提供 `ld`，v1 继续用原 SDK、编译器和汇编器。v3 使用原有 v3 库目录。对应实现见 [选库](../tools/h264_pre_v3.cmake)、[SIMD 库接入](../tools/h264_simd.cmake) 和 [PIO 链接器](../scripts/platformio_add_p4_toolchain.py)。
2. 在 P4 v1.3 上验收最终待发镜像。至少覆盖冷启动、重新上电、OTA 后首次启动、默认宠物首帧和连续播放，以及带欢迎动画的自定义宠物反复切换。核对运行镜像身份、启动计数和 panic。开机停在 Bootloader 加载成功附近并每隔数秒复位时，保留串口日志和对应 ELF，再判断属于哪条已知路径。
3. 运行 [H.264 选库、链接器及 SIMD 主机测试](../tests/p4_h264_simd_alloc_test.py) 与 [解码回归](../tests/p4_h264_replay_test.py)，同时复核 v1 镜像仍有至少 128 KiB OTA 余量。主机测试与链接核对各自留存，实机记录绑定最终镜像 SHA-256。

## 普通版和 DB 版

微雪普通版使用 C6，支持 2.4 GHz Wi‑Fi 6 和 BLE。DB 版使用 C5，支持 2.4 GHz、5 GHz Wi‑Fi 6 和 BLE。两种无线芯片均通过 SDIO 连接 P4。DB 的 SDIO CLK/CMD/D0–D3 和 RESET 与当前主机配置对应。按 P4 修订选 Wi‑Fi 6 v1 或 v3 即可使用同一套 P4 构建流程。 [普通版资料](https://docs.waveshare.com/ESP32-P4-WIFI6) · [DB 版资料](https://docs.waveshare.com/ESP32-P4-WIFI6-DB)

主机保留 C6 的 SDIO 编译配置，运行时根据 ESP-Hosted INIT 接受 C6（`0x0d`）和 C5（`0x17`）。C5 才会收到自动双频设置。该设置成功后，旧路由器兼容模式只改 2.4 GHz 的协议配置。配网诊断提供 `radioChipId`、`radioDualBand`、`radioBandModeError` 和无线副芯片固件版本。主机测试覆盖握手和开双频的分支。 [Hosted 主机测试](../tests/hosted_radio_patch_test.py)

DB 实机验收分别记录 C5 的 2.4 GHz 和 5 GHz 扫描、连接、断电回连、设备独立联网及 BLE HID 配对。P4 的完整烧录和 OTA 只写 P4。C5/C6 自身需要与主机兼容的 ESP-Hosted 从机固件，独立于 P4 镜像维护。 [DB 烧录说明](https://docs.waveshare.com/ESP32-P4-WIFI6-DB/Firmware-Flashing)
