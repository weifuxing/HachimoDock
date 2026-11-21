# WiFi6 设备 HTTPS 与 MQTT 数据服务

设备通过自己的 Wi-Fi 读取 HTTPS 数据或订阅 MQTT。PC 从 USB 或受信 Wi-Fi 发送的列表快照使用独立的授权和验收路径。见 [组件实时数据与链路验收](live-data.md)。

仅在 `PET_P4_SYSTEM_WIFI` 构建确认 WiFi6 板型后运行。握手的精确能力是
`widgetWifiData=p4-wifi-data-v1`。组件订阅已内置的数据源。下面的内置数据源由固件实现。自定义来源另见本文后半部分。

| 来源 | 固定 HTTPS 服务 | 有界参数与输出 |
|---|---|---|
| `stocks.watchlist` | `qt.gtimg.cn` 的腾讯行情 | 一批最多 20 个市场代码。解析已有 PC 提供器使用的 GBK 行情格式中的 ASCII 代码、数字和时间字段 |
| `news.hn` | `hacker-news.firebaseio.com` 的 Hacker News API | 最多读 8 个榜单 ID 和对应条目，取过去 24 小时内的最多 5 条 story。标题以纯文本显示，第三方链接不参与网络请求 |

固件只允许按固定端点构造 GET。每个服务仅使用自身的固定根 CA，并校验主机名、证书链和有效期，拒绝重定向。
腾讯使用 DigiCert TLS RSA4096 Root G5，Hacker News 使用 GTS Root R1。根变更需要经核验的固件更新。
等待 WiFi 获得 IP 且 SNTP 校时后才发起 TLS。单一低优先级任务执行 HTTPS、解析与 NVS。
请求限时 5 秒、响应正文最多 16 KiB；一轮资讯最多 20 秒。PSRAM 保存响应缓冲和
两路数据快照。网络失败时退避，成功缓存沿用单调时间戳。行情与资讯将最近成功的快照按限频策略写入 NVS，离线重启时以已过期状态显示。
行情快照与设备保存的公开股票代码列表绑定。

验收分为宿主解析和缓存回归、PC 包校验与输入模拟、桌面 HTTPS 源兼容性、P4 链接尺寸、
实机 TLS/断网恢复及物理输入。桌面端点成功不证明设备端证书、内存或射频已通过。

服务文档 [Hacker News API](https://github.com/HackerNews/API)、
[ESP-IDF HTTPS 客户端](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/protocols/esp_http_client.html)。


## 组件自定义数据源

设备握手声明 `widgetWifiFeed=p4-wifi-feed-v1` 时，列表组件可在 `runtime/widget.json` 声明：

```json
{"data":{"source":"wifi.feed","url":"https://feeds.example.org/today.json","page_var":"page"}}
```

默认使用下面的 JSON 格式。行 ID 在同一响应中唯一。

```json
{"schema":1,"status":"ok","message":"","rows":[{"id":"1","label":"示例","value":"12","detail":"","meta":"","tone":0}]}
```

当前选中的自定义数据源可在设备重启后继续取数，常规刷新间隔 60 秒，最多 20 行、16 KiB，结果缓存 120 秒。
可用数据行的 `data.refresh` 手动刷新，遵守最小间隔和失败退避。

### 第三方 JSON 字段映射

设备还需声明 `widgetWifiFeedMap=p4-wifi-feed-map-v1` 才能使用可选的 `data.response_map`。

```json
{
  "data": {
    "source": "wifi.feed",
    "url": "https://feeds.example.org/today.json",
    "page_var": "page",
    "response_map": {
      "rows": "data.items",
      "id": "station.id",
      "label": "station.name",
      "value": "reading",
      "detail": "unit"
    }
  }
}
```

此例读取 `{"data":{"items":[{"station":{"id":7,"name":"室外"},"reading":21.5,"unit":"°C"}]}}`，
得到 ID 为 `"7"`、标签为 `"室外"`、值为 `"21.5"` 的一行。

- 必须声明 `rows`、`id` 和 `label`。可选的 `value`、`detail`、`meta` 默认为空字符串，`tone` 默认为 0。
- `rows` 从响应根对象开始查找数组。用 `"$"` 选择根数组。其余路径从每个行对象开始查找。
  路径区分大小写，最长 63 个 ASCII 字符，最多 4 段。每段只允许字母、数字、下划线和连字符，段间使用点。路径仅访问对象键。
- `id` 接受非空字符串或绝对值不超过 9007199254740991 的整数。其他文本字段接受字符串、有限数字或布尔值。
  数字按最多 15 位有效数字显示，布尔值显示为 `true` 或 `false`。`tone` 只接受整数 −1、0、1。
- `id`、`label`、`value`、`detail`、`meta` 分别限 23、36、24、24、63 个 UTF-8 字节，控制字符拒绝。
  已声明的路径必须在每行解析出合法值。缺失值、重复的选中键、重复行 ID 或越界会拒绝整份响应。
- 响应仍限 16 KiB 和 20 行。映射响应最多嵌套 9 层对象或数组，字符串外的逗号及容器左括号合计最多 512 个。
  无关字段会被忽略，空数组显示空列表。

### 自定义 HTTP 读取与离线缓存

设备另需声明 `widgetWifiFeedHttp=p4-wifi-feed-http-v1` 才能使用可选的 `data.http`。
可与 `response_map` 一起使用。下面的例子读取公开 GeoJSON 的站点属性。

```json
{
  "data": {
    "source": "wifi.feed",
    "url": "https://feeds.example.org/stations.geojson",
    "page_var": "page",
    "response_map": {"rows":"features","id":"properties.id","label":"properties.name"},
    "http": {"accept":"application/geo+json","revalidate":true,"cache":"last-good"}
  }
}
```

- `http` 至少声明一个字段。`accept` 可选 `application/json`、`application/geo+json`、
  `application/hal+json` 或 `application/vnd.api+json`，由设备生成固定的 Accept 头。
- `revalidate: true` 启用 ETag。设备只对同一活动数据源、成功校验过的响应保留一个内存 ETag。
  匹配的 `304 Not Modified` 只延长现有快照的时效，不下载正文、不写 NVS。重启后首次联网完整读取。
- `cache: "last-good"` 可在刷新失败时展示上次成功结果，并标为过期。
  所有自定义来源共享一个 4 KiB NVS 槽，写入成功后至少隔 15 分钟才会再写。
  仅保存校验后的数据行快照。超出槽位容量的快照仍可在当前会话显示。
  组件 ID、端点、字段映射或 HTTP 选项发生变化时，旧记录不会恢复到新配置。
  新响应标记 `Cache-Control: private` 或 `no-store` 时不写入离线缓存，也不保留其 ETag。
- HTTP 请求仍是公开 HTTPS GET。不存在组件自定义请求头、请求体、设备数据插值、账号或认证机制。
  URL（含查询串）须公开且不含凭据。设备发出的读取请求应使用服务方提供的公开只读端点。
  现有可见性、刷新限频、失败退避、TLS 校验和正文上限均适用。

## 组件自定义网络音频

设备声明 `widgetWifiAudio=p4-wifi-audio-v1` 时，使用既有的 `p4-media-surface-v1` 媒体界面和 `media.*` 控件：

```json
{"media":{"source":"wifi.audio","playlist":[{"title":"音乐","url":"https://audio.example.org/music.mp3"},{"title":"直播","url":"https://audio.example.org/radio.mp3","live":true}]}}
```

列表可放 1–16 项。普通曲目支持 MP3 和 16 位 PCM WAV，`live:true` 用于 MP3 直播。
组件自行声明曲目和地址。播放须由用户发起，沿用媒体通道的录音和实时对话优先级。

两项能力都使用公开 DNS 域名上的 HTTPS GET（443）。设备校验公网 IPv4、证书链和域名，使用固件的常用公网根证书集合，
拒绝私网、重定向和 URL 中的用户信息。设备不添加认证头，组件地址及其查询参数不得包含密钥或令牌。

## 设备私有数据源

`widgetPrivateFeed=p4-private-feed-v1` 增加两种 `data.connection`。两种入口沿用 `wifi.feed`、整数分页变量和可选的 `response_map`。运行时包只声明 URL、类型及确切的 MQTT Topic。

```json
{"data":{"source":"wifi.feed","url":"https://api.example.org/readings","page_var":"page","connection":{"type":"bearer"}}}
```

```json
{"data":{"source":"wifi.feed","url":"mqtts://broker.example.org:8883","page_var":"page","connection":{"type":"mqtt","topic":"room/readings"}}}
```

- HTTPS 使用已校验证书的公网 GET，凭证由固件放入 Bearer 请求头。URL 无查询串。请求频率、超时、响应上限及行映射沿用设备数据宿主。
- MQTT 支持经过证书验证的 TLS，默认端口 8883，可声明其他端口。采用 MQTT 3.1.1，设备以 QoS 0 订阅一个确切的 Topic，最多 127 个可打印 ASCII 字符，支持分片接收，单个 JSON 最多 16 KiB。Topic 通配符和管理 Topic 拒绝。
- 当前选中的私有数据源在设备重启后恢复运行。选择其他组件后释放旧连接。HTTPS 失败退避重试，MQTT 断开后重连。发布端若希望订阅建立后立刻取得最后一条数据，可发送 retained 快照。
- 信息缓存留在设备内存中。私有源无 `data.http`、磁盘快照或 ETag。凭证更换、清除或数据过期后，组件移除旧数据。
- 真机还须报告 `widgetCredentialStorageReady=true` 才能保存凭证。固件使用已写入且受保护的设备 HMAC eFuse 派生加密密钥，NVS 中只保存经 AES‑GCM 加密和绑定的记录。普通固件启动无需烧写 eFuse。准备安全存储的硬件操作单独进行。

公开 HTTPS 只填写 URL。匿名 MQTT 声明 `{"type":"mqtt-public","topic":"room/readings"}`，要求设备报告 `widgetPublicMqtt=p4-public-mqtt-v1`。组件作者指定公网 TLS 服务和 Topic，打开组件便开始订阅，无需账号或硬件凭证槽。

私人 HTTPS 和 MQTT 首次打开并发现缺少授权时，在组件内显示「连接服务账号」。设备显示目标服务，输入时遮罩。在私人组件页面点右上角「服务账号」，即可重新连接账号或移除授权。自定义界面请为这个触控按钮预留区域（640×480 画布，左上角 436,8，宽 196、高 56）。返回后继续使用当前组件。

也可在受信配对后使用 `petui feed-credential <set|status|forget> <板卡 ID> <组件 ID> <runtime/widget.json>`。使用 `set` 时由密码管理器向标准输入提供凭证。HTTPS 输入一行 Token，MQTT 输入两行，分别为用户名和密码。命令返回配置状态，不提供凭证导出接口。

离线验收需在板上保存凭证，把测试服务放在独立于电脑的主机上，关闭电脑后重启 Wi‑Fi 6 设备，确认 HTTPS 请求重新成功且 MQTT 订阅接到重启后发布的新值。确认实际固件能力、有无凭证、断网恢复和清除后停止取数。
