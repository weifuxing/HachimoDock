# Devin 接入说明

Pet Manager 可同步 Devin 云端会话状态与气泡，并将设备语音草稿追加到 Windows Devin Desktop 当前可见的对话中。

## 配置云端状态

1. 在 [Devin Personal Access Tokens](https://docs.devin.ai/api-reference/personal-access-tokens) 创建 PAT。
2. 在「Agent 交互」找到 Devin，在该行打开设置，填写 PAT 和组织 ID，然后保存。
3. 点击“测试连接”确认 PAT 可访问 Devin API。

Pet Manager 通过 PAT 用户身份只读取该用户的会话，排除已归档会话并轮询最近更新的会话。正常情况下，状态更新延迟不超过约 15 秒；网络错误时会逐步退避。气泡显示会话标题和简短状态，不显示 Devin 回复正文，也不会向云端会话发送语音或任务。

## Windows 语音草稿

在 Windows 上先打开 Devin Desktop，并将目标对话保持为当前可见窗口，再开始设备语音输入。识别结果会在录音结束后一次追加到该对话的输入框，不会自动发送。macOS 暂不支持 Devin 语音输入。

## Pet UI Skill

安装组件 Skill 后，Pet Manager 会在已存在的 Devin 配置目录中安装到全局路径：

- Windows：`%APPDATA%\devin\skills\petui`
- macOS / Linux：`~/.config/devin/skills/petui`

如果 Devin 配置目录尚不存在，安装器会跳过该目标；先启动 Devin 并再次安装即可。
