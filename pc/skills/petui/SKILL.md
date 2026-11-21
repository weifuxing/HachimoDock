---
name: petui
description: Generate, validate, and publish ESP32-P4 desktop-pet games, tools, PC-fed live-data widgets such as stock watchlists, and bounded PNG sprite assets as .hachiapp packages. Use for new components or repairs to existing Pet Manager components; live data requires an implemented PC data source.
---

# petui

把自然语言需求实现为 ESP32-P4 组件。交付格式统一为 `.hachiapp`，导入 Pet Manager 后用「保存并同步」安装和更新。

## 创作工具

进入「组件中心」会自动维护 PetUI 技能和命令入口。使用用户目录下的 `.claw-pet/authoring/petui`（Windows 使用同目录的 `petui.cmd`）构建、校验、打包和发布。路径含空格时加引号。构建可使用本机 Python，无可用 Python 时由 Rust 调用随包编译器。校验、打包与本地发布均使用 Rust。按工具版本缓存编译、运行自检；`petui doctor` 可查看诊断。

有设备诊断文件时，用 `petui data-link <runtime/widget.json> <diagnostics.json>` 离线核对该数据源的 USB 和 Wi-Fi 接收计数与时效。按 [实时数据验收](references/live-data.md#离线核对接收证据) 选定板卡和启动范围。

## 设备独立运行

- 设备可以直接取得的数据、执行的动作和维护的状态，由设备负责。把首次配置、正常运行、断网恢复和重启恢复做成设备端的完整流程。
- 仅当数据或动作必须来自电脑本机时，使用 PC 提供器及共用的 USB／配对 Wi-Fi 连接。组件说明明确写出依赖的是哪种电脑数据或应用。
- 公开 HTTPS、带 Bearer Token 的 HTTPS JSON 和只读 MQTT TLS 使用设备 Wi-Fi 数据宿主。私有数据源需协商 `widgetPrivateFeed=p4-private-feed-v1`，组件内提示连接服务账号。匿名 MQTT 声明 `connection.type="mqtt-public"`，须协商 `widgetPublicMqtt=p4-public-mqtt-v1`，打开即订阅。参见 [设备联网约定](references/wifi-data.md#设备私有数据源)。

## 统一组件包

- 先读 [hachiapp.md](references/hachiapp.md)。根据需求在包内使用声明式运行时或有界 WASM，向用户交付同一种文件格式。
- 声明式和 WASM 组件都要校验、自测并通过 `petui publish <组件.hachiapp>` 发布到正式本地组件库。生成包后须确认已出现在「我的组件」。打包前保留原组件 ID。
- 新交付必须包含 `pc/cover.png`。用实际组件的代表画面，按 [hachiapp.md](references/hachiapp.md) 验收 Pet Manager 列表卡片。
- 先判断能否用有界 WASM 表达，再以声明式限制否决需求。需要维护棋盘数组、记录撤销历史或自定义移动规则的小游戏（例如 5×5 滑块拼图），走 WASM 路径。设备能力尚未读取时可以先离线编译和自测，等握手确认后再同步。
- 需要 WASM 时按 [hachiapp.md](references/hachiapp.md) 的 SDK 流程走。实际设备须宣告 `widgetWasm=hachi-wasm-v1` 才能同步 WASM；该能力与 `widgetProgram` 无关。
- 新 WASM 组件使用 640×480 原生画布，设备须宣告 `widgetWasmCanvas=640x480-v1`。交付前用实际组件检查首屏和主操作。按需要覆盖交互、结束等关键状态，若已有确认稿则对照检查。在素材、字号或画布需要调整时，先向用户说明；不能静默缩小素材再放大。
- 从第一张图开始，用 `petui sprite source.png sprite.h` 生成 WASM 精灵。该命令接受 RGB、RGBA 和索引色 PNG，用固件同源解码器检查实际产物。在 app.c 中包含生成的头文件，使用其中的宽高和字节数组调用 `hachi_sprite`。
- 打包时必须执行最终 WASM 的启动和连续帧检查。带交互的组件在项目根目录提供 `game.trace`，让 `构建并试玩` 自动回放；使用命令行时传 `--trace game.trace`。
- 实体键的 `buttons.json.action` 必须使用固件的 `lab.*` 动作，用户可见文字写在 `label`。在回放中按设备事件验收全部绑定。承诺可点按的 WASM 游戏声明 `"wasmTouch": true`，验收真实首屏、触点和画面响应。见 [实体键和触屏](references/hachiapp.md#验收实体键和触屏)。
- WASM 验收要通过固件同源 WAMR 的执行预算，覆盖首帧、首次解包、页面切换和密集输入。检查器、回放格式和命令见 [hachiapp.md](references/hachiapp.md#wasm-执行预算验收)。需要验收实体按键、触屏或屏幕实际表现时，用目标设备核对。
- 需要设备本地日期、时分秒、单调计时、随机数或当地 UV 时，使用 [WASM 宿主能力](references/hachiapp.md#wasm-宿主能力)。先核验 `widgetWasmHost=hachi-host-v1`，然后用 SDK 的 `hachi_clock_read`、`hachi_random_u32`、`hachi_uv_read`；紫外线必须处理缺测和过期。
- 需要按键或游戏事件触发离线语音时，按 [WASM 离线语音](references/hachiapp.md#wasm-离线语音) 使用 `hachi_sample`、`hachi_music` 和最多 512 KiB 的包内音频资源。同步前核验真实设备报告 `widgetWasmAudio=hachi-audio-v2`，用设备扬声器验证实际播报。
- 历史 `.clawpkg` 可从组件中心的同一个导入入口读取，兼容原 ID 和设备存档。

## 扩展能力与设备协商

创作扩展能力包时，先读取设备当前能力 JSON，再选择声明。`p4-quota-v3`、
`p4-graphics-v2`、格子/文本表、程序 v2、音序/采样/视频、应用壳和文本输入
分别协商；不能由版本号、包内声明、离线默认能力表或另一个能力推断支持。
编译实现、离线测试、设备验收是三个不同状态，只有真实设备报告的能力才可用于同步。
新增能力仍在统一验收期间，不得将候选目标的编译结果写成已在普通固件开放。

- 格子状态与包内文本表：读取 [cells-texts.md](references/cells-texts.md)，分别核验 `widgetCells`、`widgetTexts`。
- 新图形预算：读取 [graphics-v2.md](references/graphics-v2.md)，不能全局提高旧包预算。
- 粒子与关键帧背景：读取 [particles.md](references/particles.md)、[keyframe-background.md](references/keyframe-background.md)，分别核验能力；静态背景不要求关键帧能力。
- 多刚体与旋转精灵：读取 [bodies-preview.md](references/bodies-preview.md)，核验 `widgetBodies`；不把单角色 motion 的能力当成多刚体能力。
- 多页应用、文本表单、集合、长文、通知和 PC HTTP 数据：读取 [d-host-and-surfaces.md](references/d-host-and-surfaces.md)，逐项核验能力与宿主配置；旧表单限制不用于描述显式 v3 表单。
- 多服务组合、本地文件导入、动态封面和播放进度：读取 [service-bridge.md](references/service-bridge.md)，核验服务桥的精确协商与包配对。新建本地播放器使用 `kind: "local-media"`，旧 `media.source: "audio.player"` 已停用。没有服务桥能力时不得伪造能力下发。
- 合成旋律、短采样、组件视频、大图分片、跨组件事件与持久闹钟：读取 [component-media-events.md](references/component-media-events.md)，逐项核验独立能力；固定 sfx 的六个提示音不是这组扩展的上限，也不代表允许任意媒体/程序混用。
- 各能力的精确字段与拒绝边界同时见 `contracts/` 中对应向量。向量不是可发布模板。
- 账号与支付不属于本轮功能。PC 网络请求由宿主按用户授予的域名权限执行。支持 `widgetWifiFeed` 或 `widgetWifiAudio` 的 WiFi6 设备可直接读取组件声明的公开 HTTPS 数据或音频，见 [设备联网约定](references/wifi-data.md)。

## 工作流

### 按需预览与验收

一句话描述用途或玩法即可开始出稿。根据 [界面设计指南](references/ui-design.md) 以 640×480 设计，用户给出的风格和参考图优先。简单与复杂组件都要先展示 UI 并获得用户确认，再开始实现。已有确认稿时按该稿继续，用户提出修改时更新预览再确认。

- 简单组件和小改动（如单页计数、换文案或配色）只需展示首屏和主操作，确认后实现。交付前看实际首屏和主操作，有交互时检查一次输入后的反馈。
- 多状态游戏、动画角色和复杂工具用首屏加最能说明核心体验的状态来确认 UI。按实际玩法选择要展示的状态，例如一次动作反馈、结算或数据缺失。实现后用运行画面核对确认稿。
- 按组件实际使用的能力扩大验收范围。依赖实体键、触屏精度、设备音频或性能的体验，在有目标设备且用户已允许同步时做实机验收。设备不可用时先完成本地试玩，并说明剩余实机项目。按包格式和运行模式执行必需的校验、自测与回放。

1. 完整读取 [references/contract.md](references/contract.md)。契约是实现与校验的唯一依据。
2. 判断需求属于 `game` 还是 `tool`：
   - 需要设备端通用算法/事件程序时，完整读取 [references/component-program.md](references/component-program.md) 和 [references/program-preview.md](references/program-preview.md)。依据实际 `widgetProgram`/`widgetPrograms` 选择 v1/v2，并同时遵守设备报告的 cells/code/fuel/quantum 上限；缺失能力时拒绝同步。公开候选仍只有一个 PROGRAM 租约、总燃料 4096，不因 v2 私有上限更高而扩大。发布与设备执行验收分别报告，同源预览与自测不等于 P4 执行/声学验收。
   - 需要持久的公开 GET 与本地步骤串联、失败断点恢复或取消时，完整读取 [references/workflows.md](references/workflows.md)。使用显式组合 v3，共享有界工作队列；失败只允许明确恢复，不自动重提。尚不支持任意鉴权写请求或付费任务的未知结果恢复。
   - 需要任意命名变量驱动文字/进度/显隐，或组合计数、计时、本地时钟与公开 JSON 数据时，完整读取 [references/composition.md](references/composition.md)。分类汇总、筛选、排序、记录集合和多步本地事务可使用显式组合 v2，须核验宿主动作上下文能力；不能冒称已支持异步付费服务恢复。使用通用绑定与受控服务组合，不按组件 ID 添加代码；超出配额或需要鉴权/音频等服务时明确能力边界。
   - 需要"达到阈值才触发"、"有/没有资源时走不同分支"、变量相加或场景规则读写变量时，完整读取 [references/logic.md](references/logic.md)，声明 `logic: "p4-logic-v1"` 并核验 `widgetLogic`；它只提供有界比较、2 条 `all`、`else` 与 `{var}` 增量，不是表达式或脚本。
   - 变量、状态、页面、transition、节点或内容池项数超出 v1 上限时，完整读取 [references/quota.md](references/quota.md)，从实际 `widgetQuota`/`widgetQuotas` 选择显式 v2 或 v3；图片、精灵、表单及持久化仍按各自独立契约限制，不能把某一配额档位用于全局放宽。
   - 组件需要按键、结算或场景事件的短提示音时，完整读取 [references/sfx.md](references/sfx.md)，声明 `sfx: {"engine": "p4-sfx-v1", ...}` 并核验 `widgetSfx`；只有 6 个固定预设、最多 8 个 cue，没有采样、音乐或语音，不与木鱼/播放器混用。
   - 离线随机内容池、排除抽签、思考卡或设备端日期倒计时，完整读取 [references/local-tools.md](references/local-tools.md)。使用通用 local 引擎并校验 widgetLocalTools，不按组件 ID 写固件分支；断电后日期工具必须重新校时。
   - 近期资讯/新闻简报读取 [references/news.md](references/news.md)，采用有来源、时间窗和数量约束的 PC 资讯宿主，不把模型记忆当作联网结果。
   - 需要应用级布局、图标按钮、精细字号或原生 640×480 背景时，完整读取 [references/app-surface.md](references/app-surface.md)，核对设备图形能力；不得为了迁就旧图片配额静默降低分辨率或删掉图标。
   - 需要 PC 数据源驱动的可见集合行时，另完整读取 [references/host-collection.md](references/host-collection.md) 和 composition 契约；仅 UI 候选明确协商后使用，普通设备不支持时不得伪造能力或声称本地独立动态应用已完成。
   - 需要手机式滚动卡片、固定标题/底栏、打卡进度或记录历史时，另完整读取 [references/app-viewport.md](references/app-viewport.md) 与 [references/composition.md](references/composition.md)。使用通用裁切视口和 ledger，不按组件名字添加宿主逻辑；滚动须协商 widgetAppViewport，不冒充完整手机操作系统。
   - 本地应用需要多级详情、返回和页面级动作时，完整读取 [references/app-navigation.md](references/app-navigation.md)，协商 widgetAppNavigation；host 表单与动态列表不能冒用该本地导航能力。
   - 本地整数/枚举选择器与确认/取消编辑须完整读取 [references/app-form.md](references/app-form.md)，协商 widgetAppForm；该 v1 表单最多3字段。多字段文本、数字/Latin/拼音输入另读 [references/d-host-and-surfaces.md](references/d-host-and-surfaces.md) 并核验 widgetAppFormV3；提交/草稿隔离及实体可达要求保持，不能由 v1 能力推断 v3。
   - 组件需要点击、选择或直接操作画面时，读取 [references/touch.md](references/touch.md)。按需求灵活添加触屏区域，复用实体按键动作；应用点击与切果连续划线不是同一种能力，不能把可触屏等同于支持任意手势。
   - 需要滑动翻页、摇杆连发、按下即响应、确认弹窗或按状态显示按钮时，另读 [references/input-policy.md](references/input-policy.md)。这些能力必须由 `widgetInput=p4-input-v1` 明确协商，不能仅通过提高版本号假定设备支持。
   - 需要按住蓄力、松开发射和连续平台运动时，另读 [references/motion.md](references/motion.md)，使用 `widgetMotion=p4-motion-v1` 的通用单角色场景能力，不冒充任意物理引擎。
   - 需要完整横版跑跳、tile地图、巡逻敌人、移动平台和检查点时，完整读取 [references/side-scene.md](references/side-scene.md)，核验 `widgetSideScene=p4-side-scene-v1`；独立图集遵循该契约，不能退化为单角色蓄力跳跃示例。
   - 需要连续向前的随机平台和跟随镜头时，使用同一 motion 中的 `track` 扩展，另核验 `widgetPlatformTrack=p4-platform-track-v1`；不能用每次落台重置角色位置冒充连续前进。
   - 有目标、玩家输入、即时反馈、局面变化和回合结算时，使用 `game`。
   - 以计时、提醒、追踪、展示、查询或控制为主要价值时，使用 `tool`。
   - 展示行情等实时数据时，按 [实时数据约定](references/live-data.md) 选择设备直接 HTTPS、PC 经 USB 推送或 PC 经受信 Wi-Fi 推送。PC 快照共用一个 `data.source` 声明。设备直连使用 [设备 HTTPS 约定](references/wifi-data.md) 的能力与公开端点。鉴权来源如 `github.me` 由 PC 提供器处理，令牌留在 PC。接口未确认的 `codex.usage` 保持停用。
   - 把设备当快捷键面板向电脑发组合键时，另读 [references/hotkeys.md](references/hotkeys.md)，使用宿主服务 `hotkeys` 的白名单组合键与场景声明；发送默认关闭，由用户在 PC 开启。
   - 维护城市天气应用时，另读 [references/location-forecast.md](references/location-forecast.md)，使用包内声明式接口、字段映射、城市与文案；宿主只解释受控能力，不运行包内代码。
   - 分类知识库或长文阅读工具另读 [references/knowledge-library.md](references/knowledge-library.md)，知识和来源在 PC 资源包中，设备按页读取；校验数量、长度、随机不重复与授权，不用重复段落凑字数。
   - 农历/黄历工具另读 [references/calendar.md](references/calendar.md)，历法与民俗内容保留在 PC 数据包，宿主负责日期切换和时区，不把宜忌作为现实决策建议。
   - 新联网媒体组件使用双端声明式扩展，完整读取 [references/pc-extensions.md](references/pc-extensions.md)。PC 包声明面板与受控 GET 搜索映射，设备包声明 surface；宿主提供公共网络、解码与存储。先确认宿主和设备能力，不用模拟假装播放。维护旧 audio.player 时另读 [references/media.md](references/media.md)。
   - 本地木鱼音效工具另读 [references/instrument.md](references/instrument.md)，需要 widgetInstrument=p4-instrument-v1；不是任意音频或高分辨率素材的通用执行入口。
   - 触屏切果游戏另读 [references/touch-slice.md](references/touch-slice.md)。需要设备声明 p4-touch-slice-v1、widgetPointer=p4-pointer-v1 且 touchInput.ready=true；只支持该受控玩法，不代表任意触屏脚本均可运行。校验、自测和发布都传入实际设备能力文件；自测需要 Node.js。
3. 先判断这是新建组件还是优化/修复现有组件：
   - 新建组件才创建新的 `component.json.id`。
   - 优化、修复或继续迭代现有组件时，先读取用户指定的那个组件，并原样保留它的 `component.json.id`；除非用户明确要求另存为新组件，否则禁止生成新 ID。显示名称相同不能代替 ID 相同。
   - 优化现有组件时默认同时保留原 `component.json.name`，避免组件中心出现两个看似相同的条目；只有用户明确要求改名时才改显示名称，但 ID 仍保持不变。
4. 仅在缺失信息会改变状态机、数据来源、按键语义或无法确认要覆盖的现有组件时追问。不要为了填满模板而追问。
5. 在实现前梳理当前需求的机制。简单任务用简短要点即可，按需展开：
   - 用户目标与一局/一次使用的完整闭环。
   - 画面中的实体或数据，以及它们如何变化。
   - 每个可用输入产生的即时、可见结果。
   - 开始、进行、成功/失败、重开或复位条件。
   - `component.json.description` 用 1-2 句说明游戏目标、核心规则和结束条件，让用户在组件库里不进入游戏也知道要做什么；具体物理按键仍由 `buttons.json` 提供，避免用户改键后描述失真。
   - 每一项需求分别由哪个 runtime 原语实现。
   - 哪些动作适合摇杆上/下/左/右，游戏的 SW1 开始/重开动作，以及 SW2 的题材专属动作；退出由设备全局设置负责（出厂默认 SW3，但用户可以改绑或不绑定），组件不得定义退出动作或占用出厂默认退出键。
   - 开始界面设计前读 [references/ui-design.md](references/ui-design.md)，确定首屏焦点、主操作和与题材匹配的布局。控件使用动作名，键位提示以实际绑定为准。按设备尺寸渲染，再选用真实画面作封面。
   - 游戏先设计核心 PNG 素材：玩家角色、主要敌人和最重要的动态物件优先使用透明背景 PNG / 横向 sprite sheet。矩形、方块和内置 shape 只用于平台、子弹、金币、碰撞区等次要几何元素，或确实更符合极简题材时使用；不能因为省事让主体退化成色块。
6. 读取 [references/patterns.md](references/patterns.md) 检查机制是否完整。该文件只有抽象检查项，不是组件模板。
7. 在正式组件库之外准备独立工作包：
   - 新建组件必须从空目录开始，不要复制或改名任何既有组件。
   - 优化/修复现有组件时，只把用户指定的那一个组件复制到独立工作目录中修改，并保持原组件 ID；不得直接写入 `~/.claw-pet/components/library`。
8. 需要 PNG 精灵时，先完成素材再写 runtime 引用：
   - 使用当前 Agent 可用的位图生成/编辑能力，或在组件目录之外使用本地图像工具制作透明背景横向 sprite sheet；组件包内只保留最终 `.png`，不得保留生成脚本、提示词、临时帧或其他源文件。
   - 每帧使用完全一致的单元尺寸并从左到右紧密排列；最终宽度必须等于 `frame_width * frames`，高度必须等于 `frame_height`。文件名使用安全 ASCII，并保证 `scene.sprites[*].asset` 的大小写与磁盘路径完全一致。
   - 先确认 PNG 文件已真实写入 `assets/`、不是把 JPEG/WebP 改扩展名、不是空白占位图，再把它加入 `scene.sprites` 并让实体通过 `sprite` 引用。没有可用位图能力时优先改用语义 shape；不得引用一个尚不存在的素材路径。
   - 发布前逐帧检查横向帧带：首帧必须在默认画面中清楚可辨；每帧都要有非空主体，主体的尺寸、重心和朝向变化要连续，不能出现整帧透明、裁切、重影、突然缩放或大幅跳位。动画只改变当前动作所需的部分，避免设备上呈现“闪烁的一团”。
9. 准备 `pc/cover.png`，再执行结构与素材校验。需要先构建才能获取运行帧时，可以用 640×480 设计稿作临时封面：

   ```bash
   petui pack <component-dir> <component.hachiapp>
   petui validate <component.hachiapp>
   ```

10. 游戏、带 `scene` 的互动工具、声明 `data` 的实时列表、声明 `instrument` 的木鱼及声明 `media` 的播放器均执行自测。可用同一包的客户端试玩验证。若本机有 Python，也可运行自动自测：

   ```bash
   python <skill-dir>/scripts/smoke_test_widget_game.py <component-dir>
   ```

   游戏自测必须从真实输入映射出发，跑通开始、每个玩法动作的即时可见反馈、自动推进、得分或进展、成功/失败结算和重开。互动工具必须让每个声明输入在可达状态中产生即时可见变化，并验证声明的 tick/自动运动确实推进画面。只验证 action 名存在、速度字段变化或单帧动画不算通过。
   普通应用自测也探索同一状态/页面内的变量变化，例如先计数再清零。探索受 512 个快照和 32 层输入深度约束，并报告是否截断；这不是无限状态空间或全部路径证明。
   实时列表自测覆盖 0/1/6/11/16/20 行的分页可达性与真实按键反馈；它不验证网络取价、时效或设备画面，这些必须另外验收，不能把结构/模拟通过写成实时链路已通过。
11. 按 [界面验收指南](references/ui-design.md#验收) 检查运行画面。修正问题后复跑受影响的操作，用实际运行画面替换临时封面并重新打包校验。修复全部结构、素材和玩法错误。使用 PNG 时还要逐条核对没有素材缺失、格式、尺寸、帧数、总像素、文件大小或帧间闪烁风险。结构校验和适用的场景/玩法自测均通过后才可交付。自动脚本须退出码为 `0`，试玩自测须记录各项结果。
12. 原子发布到正式本地组件库：

   ```bash
   petui publish <component.hachiapp>
   ```

13. 向用户报告组件 ID、版本、玩法自测结果和本地发布结果。Pet Manager 会自动发现正式目录中的组件。
14. 如果本次是优化/修复现有组件，必须明确告诉用户：在 Pet Manager 组件中心刷新组件库，打开同一个组件卡片并点击“保存并同步”，用同一组件 ID 的新包覆盖设备上的旧包；只发布到本地组件库不会自动替换设备内容。

## 路由原则

- 扩展现有组件时先复用通用能力：视觉/文案/动作映射/场景规则和受支持的物理参数放资源包；PC 网络、日期、阅读和音频等复用声明式宿主服务。不得用组件 ID、书名或玩法名称在固件内新增专用分支。
- 发现缺失原语时，一次同步设备解释器、PC 预览、校验器、能力协商、技能参考和回归测试；记录支持边界。不能把仅某个包的补丁称为通用支持，也不能承诺未实现能力免固件升级。
- 输入扩展必须覆盖实体解码→事件队列→动作派发，再覆盖场景核心和触屏；浏览器模拟通过不能代替实体按住/松开验收。新资源包若只使用设备已具备的原语，不因更换主题、布局或参数而提高固件要求。

- 先实现用户真正要求的玩法或工具，不得把不支持的游戏静默替换成 Flappy Bird、贪吃蛇或其他示例。
- 如果 P4 当前契约无法表达需求，明确指出具体限制，并询问用户是简化玩法还是等待运行时能力；不要生成一个名字相似但玩法不同的包。
- 新建组件时，禁止读取或搜索 `pc/builtin-clawpkgs`、`references/examples`、`~/.claw-pet/components`、历史生成目录或其他完整组件包。只有用户明确要求修复某个现有组件时，才读取该组件本身。
- `component.json.id` 是版本升级和设备覆盖的唯一组件身份。优化/修复现有组件时必须复用原 ID，不能通过加后缀、时间戳或重新起包名创建另一条组件记录；显示名称相同但 ID 不同仍会被视为两个组件。
- 不存在“选一个最接近的游戏再改名”的步骤。必须从当前需求的机制清单推导状态、实体、规则、参数和文案。
- 禁止复制后改名、替换文案、微调速度/数量或换色来冒充新组件。若状态结构、实体布局和规则组合没有来自用户需求的理由，视为未完成。
- `component.json.kind` 必须明确为 `game` 或 `tool`。
- 游戏的 `component.json.description` 不是宣传口号：必须简要说明怎么玩、怎样得分或完成、何时结束。组件中心会在它下方根据 `buttons.json` 自动补充当前操作方法，因此描述不得写死可被用户重新映射的物理键位。
- 所有新组件都声明顶层 `engine: "p4-bounded-runtime-v4"`。`game/tool` 只是产品分类，底层运行时相同；v3 只用于读取和维护历史包。
- 按玩法和画面选现有渲染路径。简单信息可用 Dashboard，自定义构图、图标和动态内容布局可用 App Surface，复杂玩法或现有原语表达不了的画面可用有界 WASM。声明式行为可由变量、状态、transition 和 tick 表达，按需使用 `scene` 处理坐标、移动、碰撞和边界。
- 实时数据工具遵循契约 5.1 与 `references/live-data.md`；当前已实现 `stocks.watchlist`，其他 source 先确认或补齐 PC 提供器。股票默认项、刷新周期、鉴权都属于 PC，不写入组件包，不假装任意 URL 可以直接工作。
- 新声明式小游戏需要场景机制时用通用 `scene`。旧版 `game.type=blocks|snake|flappy` 仅用于读取和维护兼容包。
- 例外：用户要求五子棋 / 五子对弈 / 连珠时，使用原生预设 `game.type="gomoku"`（契约 §6「原生五子棋预设」、app-surface.md「原生五子棋画面」），固定 action 表 `place|left|right|up|down|undo`，需设备 `widgetGamePresets` 含 `gomoku`；description 规则不变，仍需说明目标、规则与结束条件。其他棋类不得借用该预设。
- 文案出现移动、飞行、射击、子弹、敌人或碰撞时，runtime 必须真实实现对应 scene 机制；不得只在 Dashboard 中描述不存在的玩法。
- 素材配额按运行模式区分：旧 scene 精灵的兼容配额是 4 个、单帧 8–64 像素、合计 4096 像素；这不是 app surface 或原生 WASM 的总配额。精细 UI 使用 app-surface.md 或 WASM 文档的能力和预算，不要因此缩小画面。PNG 会通过统一解码器校验和转换。
- `scene.entities[*].shape` 可使用 `rect|player-ship|enemy-ship|bullet|star|paddle|ball|circle|capsule|triangle|diamond|heart|cloud|coin|character`。优先用于平台、弹丸、金币、装饰和极简几何题材；省略时才使用兼容默认值 `rect`。若当前 Agent 没有生成或绘制 PNG 的能力，必须先说明限制并选择语义形状，禁止静默用矩形/方块冒充主体。
- `assets/` 保留该运行模式正式引用的 PNG、音频 WAV 或 clip 视频 MP4；按对应文档声明资源，不要求 app surface 图片写入 scene.sprites。删除候选图和临时文件；不得改扩展名冒充编码。
- Dashboard 默认使用 `visualStyle: "clean"`，像素题材可选 `pixel`。配色、排版、图标和素材风格随题材选择，按 640×480 检查实际画面。字段值见 [组件契约](references/contract.md)。
- 视觉选择必须来自当前需求：发布前说明该风格、配色和每个实体形状为何匹配题材。若两个不同游戏除标题、文案和数值外具有相同的视觉组合，视为未完成并重新设计。
- 不生成 JS、Python、HTML、CSS、SVG、shell 或可执行代码到组件包中。
- 已安装且校验通过的组件默认获准使用其声明的受控宿主服务，首次使用和更新均不增加授权确认。可展示非阻塞的数据使用说明；保留手动停用、系统权限、服务配置、实际操作及破坏性操作确认，不把默认授权实现为自动录音、播放或派发任务。

## 发布边界

- `.staging` 是发布器使用的短暂事务目录，不是用户组件库，也不在 Pet Manager 中展示。
- 正式组件只存在于 `~/.claw-pet/components/library/<component-id>/<version-hash>/`。
- 同一 `<component-id>` 下的新内容哈希是该组件的新版本；Pet Manager 展示最新版本。旧内容可以保留用于内容寻址，但设备仍需从组件中心对同一卡片执行“保存并同步”才会被新版本覆盖。
- 使用 `petui publish <组件.hachiapp>` 或 Pet Manager「组件中心 → 导入组件」发布。两者调用同一个导入器，由客户端计算内容哈希并写入正式目录。目录名使用包内文件的相对路径和内容计算，不能用压缩包文件的 SHA-256 代替。
- `petui publish` 遇到 `Operation not permitted` 或其他发布错误时，保留已校验的 `.hachiapp` 和原报错，交付文件路径，让用户在 Pet Manager 点「导入组件」。导入成功前，报告为「已打包，待导入」。不要手动创建哈希目录、复制暂存文件或改写正式组件库。
- 不启动 Pet Manager、Agent CLI 或额外终端。生成交互始终留在用户当前 Agent 会话中。

## 交付前检查

- `game/tool` 路由符合用户目标。
- 逐项对照生成前的机制清单；用户要求的每个核心行为都能在 runtime 中找到对应实现，且没有无来源的模板机制。
- 默认画面无需操作也有明确含义，且与 runtime 初始 state/page/vars 一致。
- 游戏在组件库中的 description 已经说明目标、核心规则与结束条件；按钮说明可由组件中心根据真实映射生成，不在 description 中写死 SW1/SW2/SW3。
- 首屏主内容、关键操作和交互反馈在设备尺寸下清晰可辨。按 [UI 验收指南](references/ui-design.md) 核对实际运行时画面和封面。
- 按运行模式检查素材引用。Scene 精灵使用透明背景横向帧带，文件名与 `scene.sprites.asset` 一致，首帧能表达默认状态，连续帧无空白、错位或尺寸漂移。App Surface、WASM、音频和视频按各自的素材契约检查。
- 每个 action 都必须有 runtime transition。传统包还必须绑定实体输入；声明 `p4-input-v1` 的资源包可以额外提供仅触屏按钮或滑动动作，但须真实可达，不能只是未绑定的空动作。旧版 game 预设动作仍须有实体键兜底。新硬件可使用摇杆上/下/左/右与中按；左右和中按沿用历史事件名以兼容旧包。组件不得声明 `page_main/page_back` 等系统导航 action；退出由设备全局绑定统一处理，摇杆中键长按默认不绑定。
- 游戏核心角色和主要动态物件已优先使用 PNG；若退回内置 shape，能够说明题材或能力限制上的具体理由，而不是为了省事。PNG 的透明背景、帧尺寸、帧数和引用路径均通过校验。
- 出厂默认 SW3 短按是设备全局退出键，用户可以改绑或不绑定；生成或更新组件时仍不得占用 SW3，也不得自行声明退出动作。游戏优先将方向动作放到四向摇杆，将 SW1 用作开始/重新开始，将 SW2 留给射击、技能、暂停等题材专属动作。工具以 SW1 为主操作、SW2 为次操作，并可按语义使用摇杆方向切页或调节。
- 组件画面、footer 和说明不得写死“SW3 退出”；需要提示时统一写“全局键退出”或“退出跟随设备全局设置”。
- 无触控设备不含触屏事件；需要移动/碰撞时只使用能力清单声明的通用 scene 原语。
- 没有虚构数据、未声明的数据源或示例残留。
- 结构校验、完整玩法自测和发布命令均成功；玩法报告证明开始、输入反馈、推进、进展/得分、结算与重开都可达。最终路径位于正式 `library`，而不是工作目录或 `.staging`。
- 带 scene 的互动工具自测报告证明每个真实输入均有可见反馈，声明的 tick/自动运动在互动后的可达状态中会生效；PNG 横向帧带已逐帧检查，没有空帧、裁切、重影、尺寸或重心突变。
- 优化/修复任务保留了原 `component.json.id`，交付说明包含组件中心刷新与“保存并同步”的设备覆盖步骤。

媒体切图扩展：已支持 widgetSurfaceImages=p4-surface-images-v1 时，遵循 references/pc-extensions.md 的 surface.images 契约；这是媒体独立配额，不扩大游戏 scene 精灵配额。

### PC 数据与应用界面

涉及 PC Fetch、应用壳标签页、form v3、data rows、长文、figure 或通知时，读取 [PC Fetch、页面、表单与通知](references/d-host-and-surfaces.md)。以目标设备实际报告的能力和相应验收结果判断是否支持同步与执行。

### 通用服务桥候选路由

需要原生服务投影、稳定键列表、类型化参数动作、显示时间轴、动态图片或显式本地媒体导入时，完整读取 [service-bridge.md](references/service-bridge.md)。纯标量桥可复用 legacy host 及原有能力；动态 image/timeline/event 参数或 local-media 必须显式声明 `surface.services="p4-service-bridge-v1"`、核验真实设备候选能力及各依赖，并用 `host_contract` 配对 PC 声明与设备 runtime（所有数值零统一 `i0;`，其他有限浮点保留）；不得按组件 ID 写特例或把 PC 预览当成设备支持。候选默认关闭，未完成统一固件/HIL验收；本地媒体不开放在线歌曲、任意 URL、账号或支付，不同步用户全局 skill。

## 跨电脑交付与能力边界

- 客户端每次进入组件中心会维护 Skill 和工具链；使用 `petui tool-info` 核对实际执行的客户端版本。不要从旧挂载镜像复制启动器。
- 所有打包入口必须通过客户端统一校验。Python 包装工具调用原生校验，不手工绕过；缺失的空 vars/transitions 可补默认值，非法状态引用、损坏资源必须修复。
- WASM 的通用实体输入是 KEY_DOWN 的 left/right/up/down/a/b（1–6）及触控。KEY_UP 与 SELECT 是保留 ABI 值，当前实体按键不派发，不要依赖它们启动游戏。
- `media_cmd` 是旧城市电台兼容入口，不是任意本地音乐播放器。需要自定义播放器与导入本地音乐时使用受控 pc/application.json 服务桥，声明真实依赖；未知设备能力不妨碍打包和社区发布，同步时再提示实际兼容情况。
- 试玩由固件同源解释器执行，遵守相同指令、内存和绘图预算。基础验收覆盖启动、连续帧和六种按键；它不代表所有玩法分支已测。交互组件须交付 game.trace 覆盖开始、主要操作、退出/结束；修改最终 WASM 后重新验收。
- 导入成功不等于同步成功，传输完成不等于运行通过。分别报告打包、导入、同步、实机试玩结果，不把未验证的阶段说成成功。
