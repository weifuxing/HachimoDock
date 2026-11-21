# 本地随身听组件参考

本地随身听通过注册的 `kind: "local-media"` 服务访问用户选择的本地曲库。开启组件不会自动播放。设备使用稳定歌曲键选曲，实际播放回执提供进度、标题、歌词和封面。没有可靠时长时禁用跳转。空库提示用户导入，导入失败保留已有曲库。

- SW1 播放选中项，SW2 播放或暂停。摇杆上下选曲，左右以 5 为步长调节音量，中按切换播放模式。退出遵循全局操作。
- 队列由宿主分页。PC 服务面板与可用的设备触控使用相同的命令入口。移除曲目须显式确认。
- 动态封面由宿主通过有界句柄提供。加载失败沿用组件内静态唱片图。音频文件和原始文件路径保留在电脑上。
- 退出、断线、语音打断或停用时终止已有播放。回执结果未知时先查询状态。游标和持久化操作继续遵循[服务桥契约](../skills/petui/references/service-bridge.md)。

## 示例包与离线验证

[源码](../candidate-clawpkgs/local-music-player/) 与 [候选 ZIP](../candidate-clawpkgs/local-music-player.clawpkg) 共用 [清单](../candidate-clawpkgs/local-music-player.candidate.json)。[预期能力夹具](../candidate-clawpkgs/local-music-player.expected-capabilities.json) 用于离线校验。同步前须从目标设备核对 `widgetServiceBridge=p4-service-bridge-v1`、宿主集合和包声明的其他能力。

从仓库根目录运行：

```sh
node --test pc/src/component-center/service-bridge-controls-model.test.js
python3 pc/skills/petui/scripts/validate_generated_widget.py \
  pc/candidate-clawpkgs/local-music-player \
  --capabilities pc/candidate-clawpkgs/local-music-player.expected-capabilities.json --json
```

准备好桌面资源与 Rust 依赖后，可在 `pc/src-tauri` 运行 `cargo test --offline --lib service_bridge::` 和 `cargo test --offline --lib music_player::tests::local_`。

## 使用与验收

覆盖空库、单曲、20 首上限、重复或缺失文件、损坏曲库、导入失败、长标题与同名歌词或封面。核对选曲、播放、暂停、继续、停止、音量、模式、跳转及删除确认。播放中退出、断线或进入语音时，确认旧播放停止，旧请求和视图无法恢复它。分别验证实体输入、实际声音与资源释放。结构校验和预期能力夹具只能证明离线输入通过。
