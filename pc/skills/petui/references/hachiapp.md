# .hachiapp 创作与验证

`.hachiapp` 是单一交付格式。包内的 `component.json` 确定稳定组件 ID，`runtime/widget.json`、`buttons.json`、`negative-screen.json`、`share.json` 和素材沿用 PetUI 契约。`hachiapp.json` 由打包器写入。若使用 WASM，唯一允许的模块路径为 `runtime/app.wasm`，设备声明的 SHA-256 必须匹配实际内容。

## 设备运行清单

`runtime/widget.json` 需要 `schema_version: 1`、非空的 `states`、指向已声明状态的
`initial_state` 和 `dashboard` 对象。即使玩法全部在 WASM 中，设备也会先解析这份清单。
PetUI 打包及客户端下发会为省略的 `vars`、`transitions`、`tick` 补上 `{}`、`[]`、`[]`。
显式写入这些字段时，使用正确的 JSON 类型。

## 封面

每个新交付的组件都应带 `pc/cover.png`，尺寸为 640×480，文件不超过 256 KiB。用组件的真实、有代表性的画面，隐去个人数据，并在 Pet Manager 的「我的组件」确认卡片显示完整图像。WASM 游戏可从模拟器截取有代表性的对局帧。这个文件只供桌面预览。旧包仍可用原有的声明式预览。

## 社区发布

在「组件中心 → 发布作品」选择组件后，客户端会调用社区的组件包预检。组件 ID、版本、设备运行时、素材和按键等真实问题会在提交前提示。备用画面的装饰字段可以省略；打包器会用组件名称补齐空白的分享标题。社区仅在点击「提交审核」后创建审核申请。

## 声明式组件

按 PetUI 主流程完成素材和适用的玩法自测，再打包、校验和发布：

```sh
petui="$HOME/.claw-pet/authoring/petui"
"$petui" pack /path/to/component /path/to/output.hachiapp
"$petui" validate /path/to/output.hachiapp
"$petui" publish /path/to/output.hachiapp
```

## WASM 组件

先按 PetUI 主流程[预览并确认 UI](../SKILL.md#按需预览与验收)，再使用以下实现步骤。

1. 使用本技能的 `sdk/hachi.h`。用 C 实现 `hachi_init`、`hachi_event` 和 `hachi_frame`，检查模块、绘图和执行预算。网络与解码仍需受支持的 Pet Manager 服务。
2. 用 Pet Manager 自带的工具运行 `"$HOME/.claw-pet/authoring/petui" build /path/to/game.c /path/to/component /path/to/game.hachiapp`。Windows 使用用户目录下的 `.claw-pet/authoring/petui.cmd`。也可在「创建组件 → 构建并试玩」里选取含 `app.c` 和 `package/` 的项目文件夹。组件目录含清单、按键和封面。更新时保持原 ID 与存档槽语义。
3. 运行 `petui validate <组件.hachiapp>`，用组件试玩验证实际输入、画面与玩法。
4. 运行 `petui publish <组件.hachiapp>`，确认「我的组件」出现该版本。客户端的「构建并试玩」会自动完成本地导入。
5. 上板前确认实际设备声明 `widgetWasm=hachi-wasm-v1`，由用户点击「保存并同步」。更新时保持组件 ID 和存档槽语义。

### 网页原型、组件试玩与设备画面

- 网页原型用于定视觉目标。交付前以打包后的同一个 `.hachiapp` 运行试玩并生成封面，核对标题、棋盘布局、图标及主色。4:3 屏幕可以重新排版，渐变、圆角和图标等视觉元素应在素材阶段预渲染，避免在 WASM 中每帧逐像素生成。
- 设备对每次 WASM 调用执行 160,000 指令上限。桌面浏览器的试玩使用浏览器 WASM 引擎，不能证明通过这项预算。分阶段或预处理缩放，复用固定尺寸的 RGB565 或调色板精灵。
- 实机若只显示 `negative-screen.json` 的兜底页，检查设备的 `pet_wasm` 日志中模块启动和 `trapped` 记录，并记录运行包版本和 SHA-256。仅同步成功或桌面试玩成功不足以确认真机可运行。

### 原生画布与文字层

新 WASM 组件使用 640×480。源文件在引入 `hachi.h` 前声明 `HACHI_NATIVE_CANVAS`，
在 `hachi_init` 调用 `hachi_canvas640()`。坐标、尺寸与触摸事件都使用 640×480
屏幕像素。原生画布每帧从黑色开始，`hachi_frame` 应绘制完整一帧。

```c
#define HACHI_NATIVE_CANVAS 1
#include "hachi.h"

void hachi_init(void) { hachi_canvas640(); }
void hachi_event(int type, int x, int y) { (void)type; (void)x; (void)y; }
void hachi_frame(int elapsed_ms) {
    (void)elapsed_ms;
    static const char title[] = "你好，PetUI";
    hachi_rect(0, 0, HACHI_WIDTH, HACHI_HEIGHT, 0x20242b);
    hachi_text(28, 24, 26, 0xffebc8, title, sizeof(title) - 1);
}
```

`hachi_text(x, y, px, rgb, utf8, bytes)` 在 `hachi_frame` 内调用，左上角定位，颜色为
`0xRRGGBB`。每帧至多 40 行，单行 1–127 UTF-8 字节，字号 12–48，字符数乘字号平方
的总和最多 240000。宿主在图形上方按提交顺序合成文字，超过屏幕的部分裁切。
设备使用内置字形和气泡共用的透明度混色，字库缺字时显示问号。Pet Manager 的 WASM
试玩使用相同字模、布局指标、RGB565 混色，以及本次同步的实体按键映射。固定标签
和动态数字都可用同一个接口。

打包器从模块导入自动写入 `runtime/widget.json` 的 `hachiCanvas: "640x480-v1"`。
同步前分别核验 `widgetWasm=hachi-wasm-v1` 和 `widgetWasmCanvas=640x480-v1`。
不声明原生画布的旧模块仍使用 320×240 像素和旧触摸坐标。

### WASM 执行预算验收

固件的 WAMR 对 `init`、每个事件和每帧分别计量，当前每次调用限 160,000 条
指令。新固件最多接收 192 KiB 的模块，线性内存允许固定两页（128 KiB）
或四页（256 KiB），不可增长。编译默认保留两页，静态数据和栈放不下时会重试四页。
同步前核对设备的 `widgetWasmMaxBytes` 和 `widgetWasmMemoryPages`，缺少字段的旧固件
按 64 KiB、两页处理。单个 `hachi_sprite` 资源仍限 64 KiB。

用 `petui sprite source.png sprite.h` 生成精灵。转换保留原始宽高，最多量化为
254 个 RGB565 色值，部分透明像素按 128 阈值处理。它用固件同源解码器核验
调色板、压缩索引、像素数量和资源长度，并生成可直接包含的 C 头文件。输出改用 .bin 扩展名时保留原始字节。超过
单资源限制时，可将素材切成多个精灵。

把复杂的静态素材预编码成宿主能直接绘制的格式，可以减少每帧执行量。

先检查初始化与首帧，再回放选曲、首次打开画面、密集输入和结算等峰值
场景。用原生 640×480 绘制。把昂贵的一次性工作限制在多个帧预算内；静态资源
优先用宿主已有接口。模块字节数、线性内存、绘图配额和执行预算分别检查。

在产品仓库使用目标固件的同一 WAMR 源码构建检查器：

```sh
WAMR_ROOT=/path/to/wamr cmake -S firmware/tools/wasm-host-check -B /tmp/petui-wasm-check
cmake --build /tmp/petui-wasm-check --config Release
```

客户端自带同源检查器。用 `petui verify-wasm game.hachiapp --trace game.trace` 检查最终包。
打包、导入和同步前检查初始化和连续 120 帧。构建时还会执行项目的交互回放。
项目根目录中的 `game.trace` 由构建自动读取，也可用 `--trace` 指定文件。
使用可选的本机 Python 构建脚本时，也可以用 `--checker` 和 `--trace` 在构建时验收。
回放文本每行写一个 `frame <毫秒>`、`event <类型> <参数一> <参数二>` 或
`text <应出现的完整文案>`。事件应
由当前包的实体映射转换，并记录关键页面和反馈。缺少同源检查器时，PetUI 会停止 WASM 构建和打包。

## WASM 宿主能力

组件导入以下任一接口时，打包器自动写入 `hachiHost: "hachi-host-v1"`。
同步前核验设备声明 `widgetWasmHost=hachi-host-v1`。WASM 画布及基础运行时能力仍独立检查。

| SDK | 状态 | 输出 |
| --- | --- | --- |
| `hachi_clock_read(HACHI_CLOCK_LOCAL, &s)` | 等待校时 / 可用 | `time_ms` 为 UTC Unix 毫秒，`value` 为校时传入的 UTC 偏移分钟，`age_ms` 为距最后校时的毫秒 |
| `hachi_clock_read(HACHI_CLOCK_MONOTONIC, &s)` | 可用 | `time_ms` 为设备启动后的单调毫秒，其余为零 |
| `hachi_uv_read(&s)` | 不可用 / 可用 / 已过期 | `time_ms` 为样本 UTC Unix 毫秒，`value` 为 UV 指数 ×100，`age_ms` 为样本年龄 |
| `hachi_random_u32()` | 无状态 | 宿主生成的无符号 32 位随机数，可在 `hachi_init` 给游戏随机算法设种子 |

读快照仅能在 `hachi_frame` 内调用。状态值为 `HACHI_UNAVAILABLE=0`、
`HACHI_READY=1`、`HACHI_STALE=2`。不可用时输出清零，UV 0.00 是有效数据。

显示本地时间时，用 UTC Unix 毫秒加上 `(int64_t)s.value * 60000`，再转换
为当地日历、星期和时分秒。首次连接前显示等待校时。持续供电时可断开电脑，
校准值仍会用单调时间推进。重启后重新等待校时。离线跨时区或夏令时须重新连接。
需要计算时长时用单调时间的差值。墙钟沿用电脑校时，毫秒字段保留发送时的亚秒部分，
实际相位还取决于 USB 传输和绘制延迟。

UV 读取声明正在使用该数据。持续展示 UV 的组件至少每 6 秒读取一次。
Pet Manager 使用已授权的共享城市，按城市缓存 Open-Meteo 样本。
超过 30 分钟标记为过期，超过 60 分钟不可用。位置关闭、数据源未配置或没有
有效样本时显示「UV 暂不可用」。数据来自 Open-Meteo 的天气模型，组件页面应标注来源。
用于开发或非商业测试时，可在 Pet Manager 进程环境中设置
`HACHIMO_UV_OPEN_METEO_PUBLIC=1`。有商业 API 权限的部署设置
`HACHIMO_UV_OPEN_METEO_KEY`，通过 Open-Meteo 的客户接口取数。
密钥只由客户端进程读取。组件不携带密钥、网络地址或城市坐标。

浏览器实验室用本机时钟和随机数预览，UV 默认为不可用。测试可以注入不同
时间、UV 数据和状态，实机校时与精度仍需在设备端验收。

## WASM 离线语音

用 `hachi_sample(cue)` 播放一次组件自带音频。可在按键或触屏事件中调用，也可在 `hachi_frame` 中因碰撞、得分或计时结束等状态变化调用一次。
`cue` 从 0 开始，`-1` 停止。每次事件或帧回调最多提交一次短音效和一次背景音乐请求。同一帧每个声道采用最后一项请求。
`hachi_music(cue)` 循环播放背景音乐，`hachi_music(-1)` 停止音乐。短音效会覆盖上一段短音效，可和背景音乐同时播放。
离开组件或设备接管音频时停止。语音使用设备音量，浏览器试玩需先打开声音。

包内固定资源 `assets/speech.hab` 最多 512 KiB，包含 1–64 段，每段不超过 8 秒。
格式是 `HAB1`、小端 u32 片段数、每段一对小端 u32 绝对偏移与字节数，随后按序
紧接各段 16 kHz 单声道 IMA 数据。每 166 字节块包含 320 个样本：小端 i16 首样本、
u8 步长索引（0–88）、一个零字节、小端 u16 样本数和 160 字节按低半字节优先的
IMA 编码。最后一个字节的高半字节为零。

打包器自动绑定 `hachiAudio: "hachi-audio-v2"` 与语音资源 SHA-256。同步前设备必须
宣告 `widgetWasmAudio=hachi-audio-v2` 及新版资源传输能力。语音资源不可与其他组件
音频、媒体或 PC 宿主服务混用。验收应覆盖进场、不同卡片、换牌和退出，并用目标设备
确认实际声音和语音对话的抢占行为。

## 历史包

在 Pet Manager 使用同一个「导入组件」入口选择旧 `.clawpkg`。导入沿用组件 ID 和原运行时声明，设备按原 ID 加载已有存档。


### 设备同步验收

Pet Manager 会按目标设备的握手能力检查下发资源。组件预览和本地包校验通过后，仍要在目标板上同步并确认设备清单出现新版本。旧固件的场景精灵图集是整份发送的，大小取决于解码后每像素三字节以及 USB 封包开销。需要较大的图集时，用支持 `p4-widget-files-v2` 的固件同步。

### 验收实体键和触屏

WASM 的动作名由设备 ABI 定义。`buttons.json.action` 使用 `lab.left`、`lab.right`、
`lab.up`、`lab.down`、`lab.a`、`lab.b`，显示文字写在 `label`。
用 `input button.sw1.short_press` 或 `input joystick.up` 从包里的映射开始回放。
新编译的小游戏逐个验证实体动作的结果。在输入前渲染一帧并用 `observe` 记录状态，
输入后再渲染一帧，然后检查 `appeared <刚出现的完整文案>`、
`pixel-changed <x> <y> <预期 RGB565 十六进制值>` 或 `slot-changed <槽位> <预期值>`。
旧回放中的数字键码必须对应一个实体绑定。固件派发实体动作事件（`event 4`）。

如果游戏提供触屏，给 `component.json` 增加 `"wasmTouch": true`，通过
`hachi_event` 处理画布触点。在 `game.trace` 中先用 `frame 0` 和 `text <首屏实际文案>`
检查首屏，再用 `observe`、`event 1 <x> <y>`、`event 3 <x> <y>`、`frame 0`
和上面的具体结果断言验证点按。静态画面可用 `unchanged` 检查非法触点。
拼图分别验证合法相邻图块、非法图块、撤回和通关。用最终包的实际首屏或关键画面制作封面。

回放开头可用 `seed-save <槽位> <值>` 模拟从已有存档启动。时钟默认未校时。
回放中用 `clock <UTC 毫秒> <时区偏移分钟>` 注入校时数据，用
`uv <状态 0/1/2> <UV 指数×100> <样本年龄毫秒>` 覆盖不可用、有效和过期样本。
桌面试玩可切换到未校时状态。WASM 音效为单声部，同次调用提交多个音符时播放最后一个。
