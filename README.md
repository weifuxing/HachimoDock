<div align="center">
  <img src="assets/hachimodock-mark.png" width="112" alt="HachimoDock（哈基米机）logo" />
  <h1>HachimoDock（哈基米机）</h1>
  <p><strong>把 AI 助手、语音和日常小工具放到桌面上</strong></p>
  <p>
    一块会跟随任务变化的小屏，配上实体按键、摇杆和你喜欢的形象。
  </p>
  <p>
    <a href="https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB">
      <img alt="硬件复刻 · PCB · BOM · 装配教程" src="https://img.shields.io/badge/硬件复刻-PCB%20%7C%20BOM%20%7C%20装配教程-ff6700?style=for-the-badge&logo=kicad&logoColor=white" />
    </a>
  </p>
  <p>
    <a href="#当前版本与下载">下载</a>
    · <a href="#你可以用它做什么">功能与场景</a>
    · <a href="#快速开始">快速开始</a>
    · <a href="#硬件方案">硬件与屏幕</a>
    · <a href="#源码与开发">源码</a>
    · <a href="#常见问题">常见问题</a>
  </p>
  <p>
    <img alt="License: non-commercial source available" src="https://img.shields.io/badge/license-non--commercial_source--available-2a2620" />
    <img alt="Desktop" src="https://img.shields.io/badge/desktop-macOS%20%7C%20Windows-db3b2b" />
    <img alt="Device" src="https://img.shields.io/badge/device-ESP32--P4%20%7C%20480%C3%97640-e8a23a" />
  </p>
  <p>
    <a href="https://github.com/ai-maker-lab/HachimoDock/stargazers"><img alt="GitHub stars" src="https://img.shields.io/github/stars/ai-maker-lab/HachimoDock?style=social" /></a>
    <a href="https://github.com/YizhengWw"><img alt="Follow @YizhengWw" src="https://img.shields.io/github/followers/YizhengWw?label=Follow%20%40YizhengWw&style=social" /></a>
  </p>
</div>

## 本分支的硬件适配

本分支已合入官方 0.1.245 / R292 公开源码，保留 HD28091C40-V1 屏幕、CST328 触摸、自制屏幕转接板和外壳适配。Pico 使用官方板型配置，SW1/SW2 为 GPIO14/GPIO6。显示默认采用 2 路 DSI、每路 500 Mbps，v3 保留 20 MHz DPI 像素时钟。触摸默认每 15 ms 轮询一次，INT 可悬空。适配固件版本带有 `-hd28091` 后缀。

接线、板型编译目标、打印文件及实测范围见 [HD28091 硬件改动与固件适配说明](docs/hd28091-hardware-adaptation.md)。本分支适配固件按该说明自行编译，下方版本与下载表对应上游官方发布。

| 配套硬件 | 资料与文件 |
| --- | --- |
| HD28091C40-V1 屏幕，480×640 | [淘宝购买链接](https://item.taobao.com/item.htm?id=566355265504&mi_id=0000sGhHLYPMQHlue9llXrv9IkNEE6YnqL7G8TRNHJ512as&spm=tbpc.boughtlist.suborder_itemtitle.1.70d82e8dXaiCDY) |
| 自制屏幕转接板 | [立创开源硬件平台工程](https://oshwhub.com/fosen/project_tzekpmhv) · [项目内原理图、PCB 与 BOM](hardware/screen-adapter/README.md) |
| 方案一 R10 整套外壳 | [零件清单与打印说明](hardware/enclosure-hd28091/README.md) · [零件预览](hardware/enclosure-hd28091/HD28091-R10-parts.png) |

外壳采用 R10 外角和约 R4.5 面框边缘，摇杆凸台沿用抬高前的高度。整套共 8 件，包括前壳、后壳、三个按键帽、摇把帽、内件和盖板，每件打印 1 个。外壳目录集中提供独立 STL、各件 STEP、总装 STEP、FreeCAD 工程和前壳 3MF。前壳 CAD 为网格转实体，新增配套件的切片与实物装配验证待完成。

## 项目简介

HachimoDock（哈基米机）是一台与电脑协作的桌面小伙伴：让 AI Agent 的工作状态变成看得见的动画，用实体按键说出下一条需求，也可以切到游戏、计时器或信息组件。它由 **Windows/macOS 客户端和 ESP32-P4 设备**组成。设备负责显示、输入与音频，电脑连接 Agent 和所需服务。

你可以把它放在显示器旁跟随任务，用语音给 Agent 写草稿，或在工作间隙与宠物聊天。支持的 WiFi6 板型还能通过局域网连接电脑，或切换成蓝牙快捷键盘。无线使用时，设备需要单独供电，电脑协同功能需要客户端保持运行。

桌面应用的安装名称为 **HachimiDock**。开发文档中的 **Pet Manager** 指同一应用。

[English](README.en.md) · [下载最新版本](https://github.com/ai-maker-lab/HachimoDock/releases/latest) · [问题反馈](https://github.com/ai-maker-lab/HachimoDock/issues) · [安全反馈](SECURITY.md)

## 当前版本与下载

| 项目 | 版本 | 下载 |
| --- | --- | --- |
| Windows x64 | **0.1.245** | [下载安装包](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_Windows-x64-setup.exe) |
| macOS Apple silicon | **0.1.245** | [下载安装包](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_macOS-arm64.dmg) |
| 配套设备固件 | **0.7.107-p4-r292 / R292** | 客户端内置 OTA · [完整烧录包](https://github.com/ai-maker-lab/HachimoDock/releases/tag/v0.1.245) |

更新说明见 [0.1.245 发布页](https://github.com/ai-maker-lab/HachimoDock/releases/tag/v0.1.245)。客户端内置 WiFi6 与 Pico 的 v1、v3 四种 R292 OTA 固件；Wi-Fi 按键与对话状态同步需要同时更新客户端和固件。Wi-Fi 配对后可脱离电脑 USB 数据连接，设备仍需供电，电脑协同功能仍需管理端运行；Pico 固件不包含 Wi-Fi／蓝牙。下载后可核对 Release 页面显示的 SHA-256。

完整烧录包用于新设备或恢复出厂，会清除设置、形象和组件；请同时核对板型和芯片修订版，WiFi6 与 Pico、v1 与 v3 不可混刷。日常升级优先使用客户端内置 OTA。

当前预编译客户端支持 Apple silicon Mac 和 Windows x64；不提供 Intel Mac 或 Linux 安装包。macOS 包使用 Developer ID 签名并通过 Apple 公证；Windows 安装器未做 Authenticode 签名。发行包不内置服务 Key。公开源码与安装包的关系见下方“源码与发布对应关系”；[PC 构建](pc/README.md)和[固件构建](firmware/BUILD.md)文档以当前公开源码为准。

### 按板型和芯片修订版下载完整烧录包

**第一次烧录，建议直接用 ChatGPT、WorkBuddy 等 AI 工具协助。** 下载并解压下表中与设备匹配的完整包，把包内的 `FLASH.md` 交给工具，并告诉它你的电脑系统和板型。可以这样问：「请先帮我核对连接的设备和固件包是否匹配，再按这份说明带我完成首次烧录，并检查写入结果。」

| 板型 | 芯片修订版 | R292 完整烧录包 |
| --- | --- | --- |
| WiFi6 | v1 | [下载 WiFi6 v1](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_wifi6-v1_FULL-FACTORY.zip) |
| WiFi6 | v3 | [下载 WiFi6 v3](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_wifi6-v3_FULL-FACTORY.zip) |
| Pico | v1 | [下载 Pico v1](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_pico-v1_FULL-FACTORY.zip) |
| Pico | v3 | [下载 Pico v3](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_pico-v3_FULL-FACTORY.zip) |

## 你可以用它做什么

### 工作时，抬眼看看 Agent 进行到哪一步

设备用宠物动画和任务气泡显示思考、执行、等待、完成及错误状态。选择正在跟随的 Agent 或会话，就能在电脑之外保留一个简洁的工作状态窗口。

当前适配 **ChatGPT（Codex）、Claude、OpenClaw、MiMo Desktop / MiMoCode、WorkBuddy 和 DeepSeek Harness**。不同适配器提供的状态和操作能力不同；状态跟随不代表支持所有应用的语音输入或对话切换。

跟随 WorkBuddy 或 DeepSeek Harness 时，可以在设备上看到当前会话标题和任务状态。无论通过 USB 还是 Wi-Fi 连接，任务气泡都使用 MiSans Medium，标题 32px、正文 24px。

<p align="center"><img src="assets/agent-status.gif" alt="Agent 工作状态与设备宠物动画" /></p>

### 给 Agent 写语音草稿，或直接和宠物聊天

| 你想做的事 | 使用方式 | 需要什么 |
| --- | --- | --- |
| 把想法交给电脑上的 Agent | 默认按住 SW1 录音，松开后识别并追加到目标输入框；检查草稿，再按确认键发送 | USB 连接、已配置的识别服务，以及可用的目标输入框和系统权限 |
| 与设备上的宠物连续对话 | 在宠物页面默认长按 SW2 开始，再次长按或按返回键结束；设备显示聆听、思考和回复字幕 | 已配置的识别、对话及语音合成服务；USB，或已配对并授权的 WiFi6 连接 |

实时对话还可以配合本机待办、电脑状态等工具使用。连接米家账号后，可控制账号授权且开放相应能力的设备与场景；目前使用中国大陆服务，具体支持范围见[智能家居说明](pc/docs/smart-home.md)。这些能力需要电脑客户端运行，并按功能连接相应服务。

**语音草稿不会自动发送。** ChatGPT（Codex）、Claude、WorkBuddy 和 DeepSeek Harness 的桌面输入依赖系统辅助功能与可访问的输入框。WorkBuddy 和 DeepSeek 的目标对话需先在对应应用中打开，选择设备气泡不会自动切换它们的桌面对话。MiMoCode 的语音支持与平台相关，OpenClaw 使用自己的会话路由。目标不明确或输入框不可用时会停止操作，不会猜选历史会话或重复发送。

详见[客户端兼容说明](pc/README.md)、[WorkBuddy 使用说明](pc/docs/workbuddy.md)和[实时对话](pc/docs/realtime-chat.md)。确认语音发送也不等于批准 Agent 的工具权限请求。

### 少一根数据线：通过 Wi-Fi 放在桌面喜欢的位置

WiFi6 板型可在**同一局域网**内与电脑配对，支持 Agent 状态跟随、实时对话，以及已授权的无线组件安装和固件升级。拔掉电脑 USB 数据线后，设备仍需另行供电。使用电脑协同功能时，客户端需要保持运行，云服务需要联网。

首次配对应在可信的局域网中进行。要使用实时对话等无线控制，先通过 USB 连接同一设备，在客户端 Wi-Fi 配对设置中完成一次有线授权。**配对成功与获得无线控制权限是两步。** 更换电脑时，先在设备上解除旧配对。使用方法见 [Wi-Fi 配对与授权](pc/docs/lan-agent-pairing.md)。

USB 客户端在线时优先走 USB，接管时会结束正在进行的 Wi-Fi 实时对话。「电脑状态」组件支持 USB 和已配对的 Wi-Fi，显示数据时电脑客户端需要在线。城市电台的播放方式取决于板型和音源，支持的音源可由 WiFi6 设备直接播放。Pico 固件不提供 Wi-Fi 或蓝牙。

### 把实体按键变成电脑快捷键

在 WiFi6 设备的「蓝牙快捷键」中配置按键，开启蓝牙模式，再到电脑的蓝牙设置里配对 **HachimiDock**。三个实体按键支持分别设置单击、双击和长按，摇杆方向与中键也有对应配置，可用于复制、粘贴或你常用的软件快捷键。

快捷键可组合主键和修饰键，实际效果取决于电脑系统及当前应用。默认单击 1、2、3 键分别发送 **Ctrl+C、Ctrl+V、Ctrl+A**；Mac 用户可按习惯改为 Command 组合键。

蓝牙键盘连接后，实体按键和摇杆用于向电脑发送快捷键，原有设备按键功能暂停；断开蓝牙或关闭该模式后恢复。每次操作发送一组按键组合。

### 工作间隙，换个形象或打开一个小组件

内置西高地形象，也可导入自定义外观、状态视频和提示音，或配置生成服务制作素材。在组件中心预览、安装与移除小游戏和工具，例如番茄钟、五子棋、贪吃蛇、木鱼、电脑状态和城市电台。

- **设备本地组件**：安装后在设备上运行；是否需要联网或电脑协同，取决于组件自身功能。
- **WiFi6 联网组件**可在设备上直接读取 HTTPS JSON、订阅 MQTT。公开数据打开即用，私人数据在组件内授权一次，设备重启后会重新连接。
- **电脑状态**等使用电脑数据的组件通过 USB 或已配对的 Wi-Fi 从桌面客户端取数。
- **自己的组件**：在支持的 Agent 中使用 [PetUI Skill](pc/skills/petui/SKILL.md)描述游戏或工具，生成后在组件中心预览、校验并同步到设备。组件受设备能力和存储限制，不是任意网页或脚本。

缺少触控不影响一般组件安装，但触控玩法可能无法使用。城市可以手动设置或关闭自动定位；自动定位按出口 IP 估算城市，不是 GPS。待办保存在本机，目前不提供跨电脑同步或到点通知。

更多说明：[数据组件](pc/docs/widget-live-data.md) · [电脑状态与近期待办](pc/docs/local-live-widgets.md) · [音量与共享城市](pc/docs/shared-device-settings.md) · [城市电台](pc/docs/city-radio.md)。

## 快速开始

如果是还没烧录过的新板，请先按上方的[完整烧录包指南](#按板型和芯片修订版下载完整烧录包)完成首次烧录，再连接客户端。

1. **安装客户端并连接设备。** 选择对应系统的安装包，用支持数据传输的 USB 线连接设备；在客户端确认板型与固件版本。
2. **先体验状态跟随。** 打开支持的 Agent，在客户端选择跟随目标、形象和按键配置。已有设备需要升级时优先使用客户端 OTA。
3. **按需开启语音。** 在 API 配置中填写自己的服务凭据，并完成所需系统权限授权。语音输入前打开目标对话；实时对话还需要配置对话模型与声音。
4. **可选：配置无线或蓝牙。** WiFi6 用户可按上文完成局域网配对和 USB 授权，或将实体按键配置为蓝牙快捷键。

| 默认操作 | 行为 |
| --- | --- |
| SW1 短按 / 摇杆中键 | 确认或进入 |
| 按住 SW1、松开 | 录音、结束并写入语音草稿 |
| SW2 短按 | 切换宠物页面与组件中心；组件可绑定自己的动作 |
| SW2 长按 | 开始或结束实时对话 |
| SW3 短按 | 返回或取消 |
| SW3 长按 | 打开设备系统设置 |
| 摇杆方向 | 选择会话、组件或执行组件映射动作 |

这些是出厂默认值，用户配置优先。语音确认发送不等于批准 Agent 的工具权限请求。WorkBuddy 的操作边界见[专门说明](pc/docs/workbuddy.md)。

macOS 安装受阻时，先检查下载来源和校验值，再按系统“隐私与安全性”的提示处理。不要对未知应用解除隔离或关闭 Gatekeeper。语音输入所需权限位于“系统设置 → 隐私与安全性 → 辅助功能”。

### 新设备或恢复出厂

[v0.1.245 Release](https://github.com/ai-maker-lab/HachimoDock/releases/tag/v0.1.245) 提供 WiFi6 v3 和 Pico v3 原底板的完整烧录包。包内包含 Bootloader、应用固件、形象、组件及双平台烧录工具。请同时核对板型和实际芯片修订版；微雪 ESP32-P4-WIFI6-M 的 M 不代表芯片版本。同一芯片的固件由 Windows/macOS 共用。完整镜像会覆盖设备现有数据。

**v1 与 v3 烧录包不可互刷，具体版本可以咨询客服进行确认。** 包内脚本需要 Python 3.10+，自动安装 esptool **5.4.0**。屏幕连接 **DSI**，不是 CSI；请断电后插拔排线。详细版本和步骤见 [固件构建与烧录指南](firmware/BUILD.md)。

## 图集

<table>
  <tr>
    <td width="50%">
      <img src="https://image.lceda.cn/oshwhub/pullImage/6c47814b144549beb179528ea77cd75e.png" alt="HachimoDock（哈基米机）实拍图" />
      <br />
      <sub>桌面小屏实拍：把 Agent 状态放到工位视野里。</sub>
    </td>
    <td width="50%">
      <img src="https://image.lceda.cn/oshwhub/pullImage/0187d6df22f74724acda54e111c7c850.png" alt="HachimoDock（哈基米机）设备近景" />
      <br />
      <sub>新版三按键与四向摇杆机身，屏幕显示内置西高地形象。</sub>
    </td>
  </tr>
</table>

家居控制能力由[**Xiaomi MiLoCo**](https://github.com/XiaoMi/xiaomi-miloco) 提供，相关 MiLoCo 授权作品的版权归小米所有，适用 [Xiaomi Miloco License](https://github.com/ai-maker-lab/HachimoDock/blob/main/licenses/Xiaomi-Miloco-LICENSE.md)，版权标识、免责声明及许可证随相关实现一并保留。非商业使用仍须符合该许可证的用途范围；本项目不向第三方授予额外的商业使用、再许可或商标权限。完整说明见 [第三方声明](https://github.com/ai-maker-lab/HachimoDock/blob/main/THIRD_PARTY_NOTICES.md)。

## 硬件方案

### 原屏缺货与替代屏幕方案

原方案屏幕目前缺货，可参考社区作者 fosen 的[哈基米机屏幕转接板方案](https://oshwhub.com/fosen/project_tzekpmhv)，用于适配另一款 2.8 英寸触摸屏。项目页提供转接板设计、配套外壳模型，以及 HD28091C40-V1 结构图和屏幕初始化配置附件。

替换时请按该项目的屏幕型号、转接板、排线、外壳及初始化配置配套选择。目前尚未在这款替代屏上完成实机验证，不能仅凭同为 2.8 英寸判断可直接替换。复刻资料和更新以该社区项目页为准。

本仓库给出软件和固件配套；PCB、BOM、硬件装配和复刻资料请看：
[OSHWHub 硬件端页面](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB)。

| 模块 | 当前方案 |
|---|---|
| 主控 | ESP32-P4，RISC-V 双核，HP 最高 400MHz |
| 存储 | 32MB Flash，32MB PSRAM |
| 屏幕 | 2.8 英寸 MIPI-DSI；物理面板 480×640，应用逻辑画布 640×480，经旋转输出 |
| 输入 | 3 个实体按键、四向中按摇杆及配套触控屏；按键可配置 |
| 音频 | 板载麦克风语音采集，桌面端 ASR 与 Agent 输入路由 |
| 连接 | USB 数据连接；WiFi6 板型另支持局域网连接与蓝牙键盘，Pico 固件不提供无线功能 |
| 扩展 | PetUI 通用小组件/小游戏运行时，组件通过桌面端同步到设备 |

## 硬件端资料

硬件端不只是一张接线图，包含从主控、屏幕、PCB、外壳到装配的复刻路径：

| 内容 | 入口 |
|---|---|
| 硬件复刻总入口 | [OSHWHub 项目页](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB) |
| 桌面客户端与完整固件 | [GitHub Releases](https://github.com/ai-maker-lab/HachimoDock/releases/latest) |
| 完整性校验 | Release 页面显示 SHA-256，烧录包内另含分段校验清单 |

## 源码与开发

| 目录 | 内容 | 开发入口 |
| --- | --- | --- |
| `pc/` | Tauri + React 桌面客户端、Agent 适配与组件资源 | [PC 构建](pc/README.md) |
| `firmware/` | ESP32-P4 的显示、音频、输入与组件运行时 | [固件构建](firmware/BUILD.md)、[设备协议](firmware/protocol.md) |
| `integrations/` | Agent 集成与接口说明 | [DSH 集成](integrations/dsh-hachimodock/README.md) |

### 源码与发布对应关系

`main` 和 `v0.1.245` 标签提供 0.1.245 / R292 的公开源码。安装包和固件的 SHA-256 见 [发布元数据](latest-release.json)。

资源保存在普通 Git 中，克隆不依赖 Git LFS。构建以锁定依赖和目标平台配置为准；不同发布包的源码对应关系以各 Release 说明为准。自行编译请查阅下列构建文档，并按实际板型选择配置。

## 常见问题

**首次烧录成功后屏幕全白，怎么办？** 等烧录工具显示写入和校验均已成功，再拔下设备的 USB 线，重新插上并保持供电。屏幕可能需要约 **5 分钟**预热，随后应恢复正常显示。若仍是白屏，先核对完整包的板型和芯片修订版。

**电脑关机后还能用吗？** 已安装且不依赖 PC 的本地组件可以继续在有电的设备上运行。Agent 状态、语音识别与对话，以及电脑提供的数据需要客户端在线；设备不会自行运行电脑上的 Agent 或云端模型。

**USB、Wi-Fi 和蓝牙有什么区别？** USB 是初次设置和日常协同的数据连接。Wi-Fi 用于受支持板型与同一局域网电脑的无线协同，部分控制需先 USB 授权。蓝牙模式把实体按键变成电脑键盘输入，不传输 Agent 对话或代替 Wi-Fi 配对。

**需要自己准备 API Key 吗？** 安装包不内置服务 Key。使用在线识别、对话、语音合成或素材生成时，需在客户端配置支持的服务和自己的凭据；费用、可用地区和网络要求由相应服务决定。不使用这些功能时，可先体验状态跟随和本地组件。

**物理屏幕与组件画布为什么尺寸不同？** 面板为 480×640，组件使用 640×480 横向逻辑坐标，由固件旋转输出。更换屏幕要同时核对接口、初始化和触控坐标，不能只看尺寸或驱动芯片名称。

**升级会清空设备吗？** 日常升级优先使用客户端 OTA。新机初始化、恢复出厂或分区迁移需要完整烧录，会清除设备上的设置、形象和组件；请先保留所需资料，并核对板型及芯片修订版。

**如何反馈？** 使用 [GitHub Issues](https://github.com/ai-maker-lab/HachimoDock/issues)，说明客户端/固件版本、电脑系统、板型、连接方式及复现步骤。提交前请移除凭据、对话与个人数据。安全问题请按 [SECURITY.md](SECURITY.md) 私下反馈。

## 下载完整性

Release 页面显示各附件的 SHA-256；完整烧录包内部另含镜像和分段校验清单。下载后核对 SHA-256，避免混用不同版本、芯片的镜像及清单。校验值能检查文件一致性，不替代来源与签名核验。

## 致谢 / Acknowledgements

感谢以下项目与社区提供的代码、工具及基础能力：

- **[Xiaomi MiLoCo](https://github.com/XiaoMi/xiaomi-miloco)、[MiSans](https://hyperos.mi.com/font/zh/)**：智能家居协议适配参考与设备显示字体。
- **Espressif、Waveshare**：设备开发框架、板级驱动与语音处理能力。
- **Tauri、React、Node.js、FFmpeg、Lucide**：桌面应用、音视频处理及界面基础。
- **[Clawd on Desk](https://github.com/rullerzhou-afk/clawd-on-desk)**：部分 Agent 状态监控与 Hook 桥接代码。

同时感谢参与开发、测试、反馈和组件创作的贡献者。第三方版权与许可详见 [第三方声明](THIRD_PARTY_NOTICES.md)。

## 作者 / Author

贡献者 / Contributors：Wanzheng Liu、Zinuo Tian。

- 📧 邮箱：avalon.ty@gmail.com
- 💬 微信交流群：扫描下方二维码加入

<table>
  <tr>
    <th>微信群二维码</th>
    <th>微信好友二维码（群满备用）</th>
  </tr>
  <tr>
    <td align="center"><img src="assets/group-qrcode.png" width="180" alt="HachimoDock 微信交流群二维码" /></td>
    <td align="center"><img src="assets/wechat-contact-qrcode.png" width="180" alt="ai-maker 微信好友二维码，备注 HachimiDock 后邀请入群" /></td>
  </tr>
</table>

微信群已经满了或无法直接加入时，可以先扫描右侧二维码添加微信好友，备注 **“HachimiDock”**，再由管理员邀请你入群。

如果 HachimoDock 对你有帮助，欢迎给仓库点一个 ⭐ **Star**、点击 **Follow** 关注后续更新，也欢迎通过 Issue 提交可复现的问题与建议。

## License

Copyright (C) 2026 Yizheng Wang.

项目采用 [HachimoDock 项目许可证](LICENSE)。允许许可范围内的个人非商业使用、修改和组件创作；商业用途须另行取得书面授权。项目名称、标识及素材的相关权利仍由各权利人保留。

**重要声明：本项目仅限许可范围内的非商业用途。未经权利人书面授权，不得用于商业产品、收费服务或独立于本项目的 APP、Web 服务及其他软件、硬件产品。** 项目内非商业组件开发按许可证明确允许。

Current sources are provided under the [HachimoDock Project License](LICENSE) for permitted non-commercial purposes only. Commercial use and independent product development require prior written permission. Third-party licenses and licenses accompanying earlier releases remain applicable. See the [English overview](README.en.md).
