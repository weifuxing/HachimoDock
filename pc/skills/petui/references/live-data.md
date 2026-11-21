# 组件实时数据与链路验收

## 组件取数路径

| 路径 | 运行时声明与设备条件 |
|---|---|
| 设备直接 HTTPS | 设备固件宿主按声明访问公开端点。内置行情和资讯查验 `widgetWifiData=p4-wifi-data-v1`，自定义公开列表 `data.source=wifi.feed` 查验 `widgetWifiFeed=p4-wifi-feed-v1`。可选字段映射、HTTP 缓存和网络音频独立协商。 |
| 设备直接 MQTT | 公开只读订阅声明 `mqtt-public`，核验 `widgetPublicMqtt=p4-public-mqtt-v1` 后自动订阅。私人订阅声明 `mqtt`，组件内连接账号后由设备恢复订阅。 |
| PC 快照经 USB | PC 提供器获取并投影数据，通过 `widget/data` 发布到设备 RAM 列表。查验 USB 握手的 `widgetData=p4-data-list-v1`。 |
| PC 快照经 Wi-Fi | PC 沿用相同的快照、来源授权、设备校验和 TTL。没有活动 USB 链路时，客户端向已配对、已授权且认证状态报告 `widgetDataDelivery=true` 的设备加密发送。连接会绑定板卡与本次启动。 |

PC 订阅用同一个 `data.source`，具体传输由客户端选择。USB 优先，每次发布走一条链路。录音、播放或传输忙时，后台 Wi-Fi 快照让出链路并等后续采样。设备直连的命名空间、HTTPS 端点策略与授权保持独立。设备可直接访问的 HTTPS Bearer 或 MQTT TLS 服务使用 [设备私有数据源](wifi-data.md#设备私有数据源)。电脑本机的数据和必须在电脑执行的动作使用 PC 提供器。

能在设备完成的组件使用设备时钟、输入、存档和数据宿主。设备联网组件验收要覆盖初次配置、断网恢复和重启，整个过程可以在没有活动 PC 客户端时运行。包含本机数据的组件可同时保留设备侧功能，只有读取、同步或执行电脑动作的部分调用 PC。

这里的快照是由 PC 主动推送的列表数据，也可以持续刷新。旧版交互式 PC 应用的实例和动作、`app/snapshot`、设备发起的 PC Fetch/`data.refresh` 及 PC 媒体控制或音频流仍走 USB。声明 `fetch.*` 只标识数据源，现有 Fetch 宿主仍负责启用、主机授权和发布归属。设备直接获取公开音频是单独的能力。

## WiFi6 设备直连

WiFi6 固件报告 `widgetWifiData=p4-wifi-data-v1` 时，设备可以在自选股组件可见期间
直接向腾讯行情取价。组件沿用 `data.source=stocks.watchlist` 和既有分页、输入及快照格式。
设备直连每 15 秒尝试刷新，成功快照保留 30 秒有效期。快照记录单调时钟缓存年龄，
行内保留上游报价时间。设备直接使用股票代码作为名称，涨跌幅不套用 PC 的盘中配色规则。
失败后退避，PC 新鲜快照可接替过期的直连快照。离线且两端都没有新数据时，现有列表标出过期。

PC 仍管理最多 20 个代码。确认精确能力后，PC 将规范化列表随 `widget/data` 的
`deviceWatchlist` 字段发送。设备仅在成功验证整个快照后接受列表，按变化在后台写入 NVS，
重启继续使用。首次未配置沿用小米和阿里默认项，显式空列表会持久保存。设备当前只提供读取，
编辑及代码联想通过 PC。旧固件继续接收原格式。PC 推送的报价行保留在 RAM，设备直连另有有限频率的 NVS 离线缓存。

详情见 [设备 HTTPS 数据服务](wifi-data.md)。

## 先确认能做什么

当前 renderer 是固定每页 5 行的有界列表，不支持任意图表、趋势图或 HTML。
离线时可以校验声明。设备能力以实际握手或已认证状态为准，链路收包以本次诊断为准。

目前可直接使用的 source：

| source | 内容与管理 | 刷新与限制 |
|---|---|---|
| `stocks.watchlist` | PC 组件详情管理最多 20 只股票，中文/代码联想、添加、删除、排序 | 正常每 2 秒批量取价并推送；失败退避；断连 30 秒标过期 |
| `computer.status` / `todos.upcoming` | 本机采样 / 本机待办（见 `pc/docs/local-live-widgets.md`） | 每 2 秒，30 秒过期 |
| `location.current` | PC 按共享设置投影城市级位置 | 变化时推送，定期续期，30 秒过期后清除展示 |
| `github.me` | **鉴权提供器**：用户在 PC 组件详情填入自己创建的 GitHub OAuth App client_id，完成设备码授权后读取 `api.github.com/user` | 120 秒 TTL，失败保留上次结果并退避 |
| `codex.usage` | **接口契约尚未确认，提供器未启用**：组件会显示"接口契约尚未确认"，不猜测端点、不冒充其他客户端 | — |

鉴权提供器（`pc/src-tauri/src/auth_provider.rs`）的规则：端点与主机名是 PC 内置描述符，资源包只写
`data.source`；令牌只存在这台电脑（macOS/Linux 为 0600 私有文件，Windows 为系统凭据库），不进包、
不进快照、不进日志、不下发设备；设备码流程由用户在 PC 面板逐步完成，不自动打开浏览器、不循环轮询；
未授权显示"尚未授权"，上游失败显示上次结果并标过期。新提供器必须先确认端点、鉴权方式与使用条款。

PC 首次配置默认港股小米 01810、阿里 09988，已有列表及用户主动删空不覆盖。
这些默认项、代码、价格、Key 和 URL 都不进入组件包。多个订阅同一 source 的组件共享该列表；
当前不能靠换组件 ID 获得各自独立的自选股列表。

天气、汇率和网络传感器可先检查能否通过设备 HTTPS、MQTT 及字段映射读取。只有电脑能访问的数据需要相应 PC 提供器和配置入口。
若用户只要求生成组件，先说明缺失的数据接入；获得产品代码修改范围后，才新增受控取数、
身份信息管理、超时/退避、行字段映射和测试。不要静默改为随机数、静态示例或复制股票包换名。

## 包内只写订阅和交互

- `component.json.kind=tool`、runtime `engine=p4-bounded-runtime-v4`。
- PC 快照基本声明为 `source` 与 `page_var`，后者引用声明的 int 变量，通常从 0 开始。过滤、选择、刷新动作按对应能力声明。
- 标准 transition 对页码增减或归零；摇杆左右翻页、主操作回首页是行情适用的输入。
- `dashboard` 使用 clean/tool，包含题材对应的 title/footer；预览显示“等待数据”，不用示例价格。
- 不混用 scene/game，不放请求脚本、URL、refreshInterval、rows 或用户名单；不靠 tick 伪造更新。
- 设备按实际页数循环，负页码向后翻页；仅设置 `page_var` 而没有有效输入会造成后面的数据不可达。
- 现有渲染布局/颜色不由组件 CSS 配置：label/meta 正常色，value/detail 用 tone；日期来自可选 date。

## 新提供器的产品边界

这部分用于用户明确授权扩展 PC 产品时，而非每次生成组件都要执行：

- 配置由 PC 持久化，组件包不携带用户资料。网络限制在提供器明确允许的接口，不能接收任意组件 URL。
- 快照 `schema=1`、受控 source、`status=ok|error|empty`、`message`、可选 `date=YYYY-MM-DD`、`ttlMs` 和 rows。
- rows 最多 20；每行 `id/label/value/detail/meta/tone`，UTF-8 上限依次 23/36/24/24/63 字节，tone 为 -1/0/1。
- `ttlMs` 为 5000–300000；不能把 2 秒刷新间隔当作 TTL。日期不冒充报价时间；零变化/未开盘等业务状态由提供器判断。
- 请求串行或互斥，失败退避；保留最后有效数据并标过期/错误。正常行情 message 为空，不加底部来源文案。
- USB 写入复用应用传输管理器。Wi-Fi 写入通过受信发布通道，避让固件、形象和音频。PC 实时行快照保留在设备 RAM。

## 离线核对接收证据

从 Wi-Fi 设备诊断采集。诊断提供「复制组件数据链路」时，用该按钮复制元数据并保存为 UTF-8 的 `diagnostics.json`。副本只含顶层 `widgetData`，可直接无范围核对。也可保存客户端通过配对认证取得的 `lan_panel_control` 操作 `readDiagnostics` 的完整返回 JSON。使用组件现有的 `runtime/widget.json` 检查同一个来源。

```sh
"$HOME/.claw-pet/authoring/petui" data-link "runtime/widget.json" "diagnostics.json"
```

```powershell
& "$env:USERPROFILE\.claw-pet\authoring\petui.cmd" data-link "runtime/widget.json" "diagnostics.json"
```

使用完整诊断时，可附加 `--board <boardDeviceId>`、`--boot <bootId>` 或同时使用两者。带范围时，诊断顶层对应 ID 必须完全一致。按钮副本省略这两个 ID，比较前从设备侧确认同一板卡和同一次启动。工具只读这两个本地 JSON 文件，输出 JSON，适合逐次比较。

```json
{"source":"computer.status","declaredRoute":"pc-snapshot","status":"observed","received":true,"fresh":true,"usbCount":0,"wifiCount":4}
```

- `usbCount` 和 `wifiCount` 是本次开机中该来源被固件接受的累计帧数。`fresh` 说明采集诊断时最新的 PC 快照是否仍在 TTL 内，两种传输共用这份时效。
- 无对应来源、旧诊断缺少计数、无效计数或范围不匹配时，`status` 为 `unknown`，接收证据为 `null`。无法读取或解析输入文件、runtime 缺少有效声明时，命令以错误退出。
- `declaredRoute` 来自 runtime 中的来源声明。`wifi.feed` 和内置 `news.hn` 归为 `device-https`。`stocks.watchlist` 为 `pc-snapshot-or-device-https`。设备直连取数需要独立的实际能力与时效证据，工具把 `deviceHttps` 保持为 `unknown`。版本号和产品配置不会产生收包证据。
- 输出仅含声明来源和接收元数据。保留诊断的采集出处；本地文件与 ID 匹配本身不认证文件。核对 Wi-Fi 实际收包可在同板同启动、断开 USB 后比较两次诊断，观察 `wifiCount` 增长与 `fresh=true`。PC 收包不能代替设备直连数据或屏幕渲染验收。

## 验证与交付

1. `validate_generated_widget.py` 校验结构、协议能力、整数分页变量、无脚本/任意请求。
2. `smoke_test_widget_game.py --json` 对 data 工具实际执行已声明 transition，检查空/单行/跨页/满表的页面可达性、每个按键在可达页面有反馈；发布器重复此测试。
3. 模拟使用合成行数而非真实行情，不调用网络、不改用户自选股；它不是实机验收。
4. 有授权且环境允许时，再以实机诊断和画面验收所用链路、真实更新、设备翻页、断连过期及重连恢复。设备直连 HTTPS 另核对该服务的能力、缓存时效和失败表现。
5. 同 ID 修改沿用正式发布流程；单纯发布不会更新已装设备，需组件中心“保存并同步”。
