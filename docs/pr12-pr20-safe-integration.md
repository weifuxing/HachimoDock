# 会话与审批的集成回归

会话卡片保留自己的所有者。文本变更推进气泡版本，移除卡片独立撤销该版本的显示资源。审批文本使用本地字体，不能借用普通气泡的像素快照。协议细节见[固件协议](../firmware/protocol.md)与[文字像素传输](pr12-safe-integration.md)。

维护会话或审批代码时，保留以下边界：

1. 审批本地文本向通用正文绘制入口传入空像素引用。
2. 停止提示音只在会话快照通过版本与保留期校验后触发，同一版本去重。
3. `approval/cancel` 使用协议的统一消息清理路径，包含过期请求。

[协议测试](../firmware/tests/protocol_contract_test.py)和[气泡渲染测试](../firmware/tests/p4_title_masks_test.py)覆盖源代码入口与文本所有权。运行桌面和 Bridge 回归时，同时核对 DSH 的审批所有权、过期事件和重连行为：

```sh
cd pc
npm test
npm run test:bridge
```

主机测试完成后，分别验证设备显示、音效、OTA 余量，以及 Windows 上已安装的 Bridge 与 ACL 行为。
