# HachimoDock

**Bring your AI assistant, voice controls and everyday tools to your desk.**

[简体中文](README.md) · [Downloads](https://github.com/ai-maker-lab/HachimoDock/releases/latest) · [Issues](https://github.com/ai-maker-lab/HachimoDock/issues) · [Security](SECURITY.md)

HachimoDock pairs an **ESP32-P4 desktop device with a Windows/macOS client**. A small animated companion shows what your AI Agent is doing. Use the physical buttons to dictate a new request, start a voice conversation or open a game or utility.

Keep it beside your monitor for task updates, give it a custom appearance, or use a supported WiFi6 board as a wireless companion or Bluetooth shortcut keyboard. The device handles display, input and audio. The computer connects to Agents and services. Wireless use still requires power, and computer-assisted features still need the desktop client running.

The installed app is named **HachimiDock**. Developer documentation may also call it **Pet Manager**.

## Versions and downloads

| Component | Version | Download |
| --- | --- | --- |
| Windows x64 | **0.1.245** | [Download installer](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_Windows-x64-setup.exe) |
| macOS (Apple silicon) | **0.1.245** | [Download installer](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_macOS-arm64.dmg) |
| Device firmware | **0.7.107-p4-r292 / R292** | OTA included in the desktop client · [Factory kits](https://github.com/ai-maker-lab/HachimoDock/releases/tag/v0.1.245) |

See the [0.1.245 release notes](https://github.com/ai-maker-lab/HachimoDock/releases/tag/v0.1.245). Both installers include R292 OTA firmware for WiFi6 and Pico boards with v1 and v3 chips. Wi-Fi button and conversation-state delivery requires both the updated client and firmware. After Wi-Fi pairing, the device can disconnect from the computer's USB data connection; it still needs power, and desktop-assisted features still require the client to run. Pico firmware does not include Wi-Fi or Bluetooth. Verify downloads against the SHA-256 shown on the release page.

Complete flashing kits are for first installation or factory recovery and erase settings, appearances and components. Match both board model and chip revision; WiFi6 and Pico, and v1 and v3, are not interchangeable. For routine upgrades, use the built-in OTA updater.

Prebuilt installers support Apple silicon Macs and Windows x64. The Mac package is Developer ID signed and Apple notarized; the Windows installer is not Authenticode signed. No service API keys are bundled. See “Source and release provenance” below for the relationship between public source and installers. See [firmware build and flashing](firmware/BUILD.md).

### Complete flashing kits

**For your first flash, we recommend using an AI tool such as ChatGPT or WorkBuddy to guide you.** Download and unzip the kit for your board, share its `FLASH.md` with the tool, and tell it your operating system and board model. For example, ask “Can you help me check that this kit matches my connected board, walk me through the first flash, and verify the result?”

| Board | Chip revision | R292 factory kit |
| --- | --- | --- |
| WiFi6 | v1 | [Download WiFi6 v1](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_wifi6-v1_FULL-FACTORY.zip) |
| WiFi6 | v3 | [Download WiFi6 v3](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_wifi6-v3_FULL-FACTORY.zip) |
| Pico | v1 | [Download Pico v1](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_pico-v1_FULL-FACTORY.zip) |
| Pico | v3 | [Download Pico v3](https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.245/HachimiDock_0.1.245_pico-v3_FULL-FACTORY.zip) |

## What you can do

### Keep an eye on your Agent's work

Pet animations and task bubbles show thinking, working, waiting, completion and errors. Choose the Agent or session you want to follow and keep its progress visible beside your main screen.

Integrations include **ChatGPT (Codex), Claude, OpenClaw, MiMo Desktop / MiMoCode, WorkBuddy and DeepSeek Harness**. Available events and controls vary by integration: status reporting, dictation, sending and switching desktop conversations are separate capabilities.

WorkBuddy and DeepSeek Harness can show the current session title and task state. Over USB or Wi-Fi, task bubbles use MiSans Medium at 32 px for titles and 24 px for body text.

<p align="center"><img src="assets/agent-status.gif" alt="Agent task states shown through pet animations and conversation bubbles" /></p>

### Dictate a draft, or talk directly with your companion

| Your goal | How it works | Requirements |
| --- | --- | --- |
| Give a request to an Agent on your computer | Hold SW1 to record, then release to append recognized text to the target editor. Review the draft and confirm separately to send. | USB, a configured recognition service, an available target editor and the required system permissions |
| Have a voice conversation with the companion | Long-press SW2 in the pet view to start. Long-press again or press Back to end. Listening, thinking and reply captions appear on the device. | Configured recognition, chat and speech-synthesis services; USB or a paired and authorized WiFi6 connection |

Real-time conversation can also work with local to-dos and computer-status tools. With a connected Xiaomi Home account, it can control authorized devices and scenes that expose supported capabilities. This integration currently uses mainland China services; see the [smart-home guide](pc/docs/smart-home.md). These features require the desktop client and their respective services.

**Dictation creates a draft; it does not send automatically.** Desktop input for ChatGPT (Codex), Claude, WorkBuddy and DeepSeek Harness depends on system accessibility and an accessible editor. Open the target conversation in WorkBuddy or DeepSeek first: selecting a device bubble does not switch those desktop conversations. MiMoCode voice support is platform-dependent, while OpenClaw uses its session routing. If the target is ambiguous or unavailable, the operation stops instead of guessing a historical conversation or sending again.

See the [desktop compatibility guide](pc/README.md), [WorkBuddy guide](pc/docs/workbuddy.md) and [real-time conversation guide](pc/docs/realtime-chat.md). Confirming a voice draft is separate from approving an Agent's tool permissions.

### Place it where you like with Wi-Fi

Supported WiFi6 boards connect to a computer on the **same local network** for Agent status, real-time conversation, and authorized wireless component installation and firmware updates. After disconnecting the computer's USB data cable, power the device separately. Keep the desktop client running for computer-assisted features and connect to the internet for cloud services.

Pair for the first time on a trusted network. For wireless control, including real-time conversation, connect that same device over USB and open the client's Wi-Fi pairing settings to complete a one-time wired authorization. **Pairing and control authorization are separate steps.** Unpair on the device before moving to a different computer. See [Wi-Fi pairing and authorization](pc/docs/lan-agent-pairing.md).

An active USB client takes priority and ends an existing Wi-Fi voice conversation. The Computer Status widget receives data while the desktop client is running, over USB or paired Wi-Fi. Playback options vary by board and stream, and WiFi6 devices can play supported radio streams directly. Pico firmware has no Wi-Fi or Bluetooth support.

### Turn the buttons into a shortcut keyboard

On a WiFi6 device, configure **Bluetooth shortcuts**, enable Bluetooth mode, and pair **HachimiDock** in your computer's Bluetooth settings. Assign separate single, double and long presses to the three buttons, with additional mappings for joystick directions and its centre button.

Combine a main key with modifiers for your preferred application shortcuts. The initial single-press mappings are **Ctrl+C, Ctrl+V and Ctrl+A**; Mac users can change these to Command combinations. What a shortcut does depends on the operating system and active application.

While the Bluetooth keyboard is connected, the buttons and joystick send shortcuts to the computer and their normal device actions pause. Disconnect Bluetooth or turn the mode off to restore those actions. Each action sends a single key combination.

### Make it your own, then take a short break

Use the built-in West Highland terrier or import your own appearance, state videos and sounds. Configure a generation service if you want to create new materials. The component centre lets you preview, install and remove games and utilities such as a Pomodoro timer, Gomoku, Snake, a wooden-fish instrument, computer status and city radio.

- **Local components** run on the device once installed. Whether they need the computer or a network depends on the component.
- **Online components on WiFi6** can fetch HTTPS JSON and subscribe to MQTT directly. Public feeds work immediately. For a private feed, authorize the component once on the device. It reconnects after restart.
- **Computer status** uses the desktop client as its data source over USB or paired Wi-Fi.
- **Your own components** can be created with the [PetUI Skill](pc/skills/petui/SKILL.md) in a supported Agent, then previewed, validated and synced from the component centre. Device capabilities and storage limits apply; this is not an unrestricted browser or script environment.

Missing touch support does not prevent general component installation, but touch-based gameplay may be unavailable. Set a city manually or disable automatic location; automatic mode estimates a city from the outgoing IP address, not GPS. To-dos are stored locally, without cross-computer sync or scheduled notifications.

Learn more: [data components](pc/docs/widget-live-data.md), [computer status and to-dos](pc/docs/local-live-widgets.md), [volume and city settings](pc/docs/shared-device-settings.md), and [city radio](pc/docs/city-radio.md).

## Quick start

For a new, unflashed board, follow the [complete flashing kit guide](#complete-flashing-kits) before connecting it to the desktop client.

1. **Install and connect.** Download the client for your system and connect the device with a USB data cable. Confirm the board and firmware version in the client.
2. **Try Agent status first.** Open a supported Agent, choose what to follow, and select an appearance and button mappings. Use client OTA for routine firmware upgrades.
3. **Add voice if you want it.** Configure your own service credentials and grant the necessary system permissions. Open the target conversation for dictation; configure a chat model and voice for real-time conversation.
4. **Optionally set up wireless or Bluetooth.** WiFi6 users can pair and authorize the device for local-network use, or configure it as a Bluetooth shortcut keyboard.

| Default control | Action |
| --- | --- |
| SW1 short press / joystick centre | Confirm or enter |
| Hold SW1, then release | Record speech, then append a recognized draft |
| SW2 short press | Switch between pet and component views; components may assign their own action |
| SW2 long press | Start or end real-time conversation |
| SW3 short press | Back or cancel |
| SW3 long press | Open device settings |
| Joystick directions | Select sessions/components or use component controls |

Saved mappings take priority over these defaults. For Agent input on macOS, allow the client under **System Settings → Privacy & Security → Accessibility**. If installation is blocked, verify the download source and checksum, then follow the operating system's security prompts; do not disable Gatekeeper for an unknown app.

### First installation or factory recovery

Choose the complete kit for **both your board model and chip revision**. WiFi6/Pico and v1/v3 kits are not interchangeable. The “M” in ESP32-P4-WIFI6-M does not identify the chip revision. Ask the hardware supplier if you cannot identify it reliably.

Factory kits include the bootloader, application, appearances, components and tools for Windows/macOS. Their scripts need Python **3.10+** and install esptool **5.4.0**. Complete flashing erases existing device data. See the [firmware build and flashing guide](firmware/BUILD.md).

## Gallery

<table>
  <tr>
    <td width="50%"><img src="https://image.lceda.cn/oshwhub/pullImage/6c47814b144549beb179528ea77cd75e.png" alt="HachimoDock on a desk" /><br /><sub>Keep Agent activity within sight beside your monitor.</sub></td>
    <td width="50%"><img src="https://image.lceda.cn/oshwhub/pullImage/0187d6df22f74724acda54e111c7c850.png" alt="HachimoDock buttons, joystick and terrier appearance" /><br /><sub>Three buttons, a directional joystick and a customisable companion.</sub></td>
  </tr>
</table>

## Hardware and displays

The original display is currently out of stock. Community contributor fosen provides an [alternative 2.8-inch touch-display adapter project](https://oshwhub.com/fosen/project_tzekpmhv), including adapter design, a matching enclosure model, HD28091C40-V1 mechanical drawings and display initialization data. Use its matching panel, adapter, cable, enclosure and configuration. The replacement panel has not yet been tested with this project’s firmware; the same diagonal size does not establish drop-in compatibility.

The paired hardware uses ESP32-P4, 32 MiB Flash, 32 MiB PSRAM and a 480×640 MIPI-DSI panel. Components use a rotated 640×480 logical canvas. Connect the display to **DSI**, not CSI, with power disconnected. Consult the [hardware project](https://oshwhub.com/eda_gqvzlprk/project_cnbmkbjc#3-%E7%A1%AC%E4%BB%B6%E5%A4%8D%E5%88%BB) before substituting parts.

## Source and development

- [Desktop source and build guide](pc/README.md)
- [Firmware source and build guide](firmware/BUILD.md)
- [PetUI component contracts](pc/skills/petui/SKILL.md)
- [Volume and shared city settings](pc/docs/shared-device-settings.md)

### Source and release provenance

The `main` branch and `v0.1.245` tag provide the public 0.1.245 / R292 source. The [release metadata](latest-release.json) lists SHA-256 digests for installers and firmware.

Resources are stored in ordinary Git; Git LFS is not required. Preserve dependency locks and choose the correct target configuration. Published binaries retain their original build contents even when `main` changes.

## Frequently asked questions

**The first flash succeeded. Why is the screen white?** Wait until the flashing tool confirms that both writing and verification succeeded. Then unplug the USB cable, plug it back in, and leave the device powered. The display may need about **5 minutes** to warm up before the picture appears normally. If it stays white, check that the kit matches both your board model and chip revision.

**Can it work with the computer off?** Installed components that do not depend on a PC can continue while the device is powered. Agent status, speech services and PC-fed data require the client. The device does not run your desktop Agent or cloud model on its own.

**How do USB, Wi-Fi and Bluetooth differ?** USB handles initial setup and everyday data connections. Wi-Fi provides supported local-network features, with a one-time USB authorization for control. Bluetooth sends keyboard shortcuts; it does not carry Agent conversations or replace Wi-Fi pairing.

**Do I need my own API keys?** No service keys are included. Configure supported services and your own credentials for online recognition, chat, speech synthesis or media generation. Costs, regions and availability depend on those providers. You can start with Agent status and local components without enabling those services.

**Will an update erase my device?** Use client OTA for routine upgrades. Factory recovery, initial setup and partition migration require complete flashing, which erases device settings, appearances and components. Keep any materials you need and verify the board and chip revision first.

**How should I report a problem?** Include client and firmware versions, OS, board model, connection type and reproduction steps in [Issues](https://github.com/ai-maker-lab/HachimoDock/issues). Remove credentials, conversations and personal data. Report security issues privately through [SECURITY.md](SECURITY.md).

## Download integrity

Compare downloads with the SHA-256 digests on the release page. Factory kits also include image checksums. Do not mix images and manifests from different versions or chip revisions. Checksums establish file consistency; also verify the download source and package signature where available.

## Acknowledgements

We thank the following projects and communities for their code, tools and foundational capabilities:

- **[Xiaomi MiLoCo](https://github.com/XiaoMi/xiaomi-miloco) and [MiSans](https://hyperos.mi.com/font/zh/)**: smart-home protocol adaptation references and device display fonts.
- **Espressif and Waveshare**: device development frameworks, board drivers and speech-processing capabilities.
- **Tauri, React, Node.js, FFmpeg and Lucide**: foundations for the desktop application, audiovisual processing and user interface.
- **[Clawd on Desk](https://github.com/rullerzhou-afk/clawd-on-desk)**: portions of the Agent state-monitoring and Hook bridge code.

We also thank everyone who contributed development, testing, feedback and community components. See [Third-Party Notices](THIRD_PARTY_NOTICES.md) for third-party copyrights and licenses.

## License and feedback

**Non-commercial source available.** The [project license](LICENSE) permits use, modification and component creation within its stated scope. Commercial use and independent products require prior written permission. Third-party software and materials retain their [own licenses](THIRD_PARTY_NOTICES.md); earlier releases retain their accompanying terms.

Home-control capabilities integrate [Xiaomi MiLoCo](https://github.com/XiaoMi/xiaomi-miloco), whose relevant works are copyrighted by Xiaomi and governed by the [Xiaomi Miloco License](licenses/Xiaomi-Miloco-LICENSE.md). This project grants no additional commercial, sublicensing or trademark rights.

For project contact, see [作者 / Author](README.md#作者--author). Report bugs through Issues with versions and reproduction steps, with personal data removed. Report security issues privately under [SECURITY.md](SECURITY.md).
