# 组件媒体、事件与闹钟的设备验收

使用 [设备测试夹具](../../../../firmware/tests/fixtures/track_c_private/manifest.json)。生成器 [prepare_track_c_private_fixtures.py](../../../../firmware/tests/prepare_track_c_private_fixtures.py) 将共享契约和已有素材写入新的测试目录，拒绝覆盖现有目标。夹具不依赖账号，不修改已安装组件。

这些夹具只使用 SW1、SW2 和摇杆。保留设备的全局退出动作；事件发送与接收夹具应同时可用。先读取目标设备的实际能力声明。界面计数可帮助定位输入，声音、画面和资源释放须独立观察。没有可用观测项时记录未验证。

## 一轮动作与期望

| 顺序/源目录后缀 | 动作 | 新能力验收期望 | 退出/切换释放检查 |
|---|---|---|---|
| 1 `sequence` | 打开先静置；SW1 `start`；待至少 5 秒；SW2 `stop`；再 start | 打开不自动响。120 BPM 的契约三音/休止共 8 拍，约 4 秒后自然停止，不因 requested 仍为 true 自动重播；stop 即停止，stop 后可再 start | sequence state inactive；队列/voice token 为零；不抢录音/AEC 前端；退出后不再发声 |
| 2 `sample` | SW1 `pet` 一次，再短间隔按 3 次；播放未完立刻全局退出；重进后静置 | 复用 `pc/public/instruments/wooden-fish/knock.wav`，1 秒、16 kHz mono。每次命令计数增加；实际输出一次性 PCM；快速连按最多 2 queued + 2 voices，不无界排队。重进不自动播旧 sample | 自然结束/队列覆盖/退出均归还各自 token；deferred 回收增加；退役 bank 最终清零 |
| 3 `clip` | 进入 `show`；观察实际动态视频；SW2 hide；快速 SW1 show；decoder 工作时切至 sample/big | 复用真实动态 `touch.lick.mp4`，由正常 PC 编译阶段压为 320×240/15 fps/≤5 秒；不能用单帧黑屏证明。show/hide 计数变化；帧顺序依赖 predecessor；hide/切换不读已释放源 | QUEUED/WORKING/CONSUMING 或 decoder cleanup 未完时 source pin 仍有效。`renderer_ext_clip_retire` false 不释放、不改 descriptor；物理完成后 true，readers=0、phase=EMPTY，clip pin/broker buffer lease 归还 |
| 4 `big` | 初始看左上；SW2 `bottom`；摇杆右/左/上/下；SW1 origin | 复用现有 640×480 PNG，320×240 可视窗口，6 个 tile。显示内容随 offset 改变，最大实际偏移 (320,240)，负值/超大值被夹住；跨 tile 边界无缺块/越界 | 每帧至多一个缺失 tile load；generation 改变不能取新包数据填旧 node；退出后 table_alloc_bytes 回到基线 |
| 5 `event-recv` + `event-send` | 两者完成安装后先打开 recv；SW1 clear；切 send；连续发送 10 次；返回 recv | 只由 send 的 SW1 发 `done`。recv 未绑定假物理 event。队列上限 8，孤立测试时 delivered 8、dropped 2；返回后 rx 增加 8。需记录实际 emitted/delivered/dropped 与 rx，不以 UI 计数单独证明总线 | 删除 recv 清订阅/队列；再次发无旧订阅 delivery；不触发隐藏程序执行 |
| 6 `alarm` | SW1 set（60000 ms）；切到 big 等待；分别测试本 boot 通知策略 false/true；回到 alarm；SW2 cancel | 一次性计时不是 epoch deadline。隐藏仍到期；未授权保留 pending，授权后补发一次通知，无隐藏 action/program 执行。可见时才走 ring transition 增加计数。cancel 后不再次到期 | 记录 armed/pending/last_var/deadline 与通知归属 id；明确取消清同一 alarm 待通知；删除清 parked/notice/subscription |
| 7 `alarm` reboot/负向 | arm 后约 20 秒 checkpoint；重启未校时恢复；再次观察同一 delay var；取消 checkpoint 后再重启 | 在有效 UTC 下设定并保存。重启后尚未校时时保留待处理状态；校准后按原 UTC 截止点比较，到期只通知一次，未到期不重新倒计时。已保存的取消状态在重启后仍有效 | 记录 UTC snapshot valid/calibration generation、utc_deadline_ms/waiting_for_clock 和 hidden checkpoint 的 durable 状态；不得把 boot uptime 当 epoch |
| 8 合并切换 | sequence→sample（播放中）→clip（worker 工作中）→big→event/alarm→主页，重复有界轮数；最后等待物理 cleanup | 无 stale snapshot 触发新包 sample，无旧 clip 源提前 free，无 ABBA 卡死；bank/token 数不随轮数增长 | 回到主页且 cleanup 完成后，store 的 live_banks/live_tokens/bank_pins 和三类 allocation 都归零；broker 回到进入测试前基线；不以全局 heap 瞬时差为唯一证明 |

当前 one-shot 样例没有周期取消按钮，不伪造未实现的 alarm_cancel runtime op。周期 catch-up/取消已有 host core 回归；需要专门物理周期验收时，应另建纯 periodic 契约候选并接明确取消入口，不能拿 one-shot 验收冒充。

## 资源观测

`pet_cmedia_stats()` 已在 owned store 实现，main lane 调用同时 collect deferred releases：

- `generation`、`tokens_issued`、`tokens_reclaimed`、`deferred_reclaimed`。
- `live_banks <= 3`、`pinned_banks <= 3`、`live_tokens <= 8`、`bank_pins == live_tokens`（snapshot collect 后）。
- `sample_alloc_bytes`、`clip_alloc_bytes`、`table_alloc_bytes`，不包含 decoder/framebuffer/staging。
- 全部物理 reader 结束并退出后：`tokens_issued == tokens_reclaimed`，上述 resident bytes、live_tokens、live_banks 为 0。

renderer 需要同一观察点：`g_h264_prefetch_lifetime.phase/cancelled`、`g_clip_entry.decoder_readers`、`g_clip_lease.token/generation`、`g_clip_entry.buffer_lease` 的 broker current/finish 状态，以及 retire 返回值。不要在 worker 仍占有时强制读/改 descriptor。

音频状态应在已有 stream lock 的保护下观察：`g_cext_sample_queued`、`g_cext_voice_tokens[]`、sampler.voice active、sequencer playing/started；只读记录不增加录音/AEC 路径等待。

事件/闹钟记录 bus emitted/delivered/dropped、目标 queue.count/dropped；alarm 的 widget/id、armed/pending/last_var/deadline_ms、通知策略与 notice_pending/归属。这些 C 调试字段和计数只用于有相应观测接口的目标。

## 证据文件要求

每轮至少记录：实际固件构建标识与启用能力、设备握手、fixture 源与素材 hash、输入时序、独立声学/视频证据、切换前/中/后的资源快照、cleanup 完成点。事件/闹钟必须含 sender→bus→receiver 或 arm→due→notification 的关联记录。

离线夹具不记录设备验收结果。分别记录设备上的失败、因缺少观测受阻以及未覆盖的项目。
