# 组件媒体与内部动作验证

组件媒体使用两阶段发布。完整校验后才使新一代资源可见。失败或取消保留旧资源。事件接收动作与闹钟目标只在声明的确切作用域内获得内部调用权限，其他动作仍需通过正常的实体键或触屏可达性检查。实现和资源限制见 [媒体资源与事件实现](capability-ext-audit.md)。

共享向量见 [内部动作契约](../contracts/internal-actions-v1.json)，完整包夹具及其素材索引见 [夹具清单](../../../../firmware/tests/fixtures/track_c_private/manifest.json)。从仓库根目录运行：

```sh
python -m pytest firmware/tests/component_internal_actions_test.py firmware/tests/component_media_owned_test.py -q
```

准备好桌面资源与 Rust 依赖后，可在 `pc/src-tauri` 运行 `cargo test --lib component_capability_ext::tests`。该组覆盖完整包引用、内部动作作用域和有界转换子进程。[设备验收](capability-ext-hil.md) 包括实际协商、播放、显示、切换和资源释放。
