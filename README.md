# RazerBatteryLite

**为 Razer DeathAdder V3 Pro 制作的轻量级 Windows 鼠标电量托盘工具。**

无需常驻运行 Razer Synapse。只需一个 EXE，就能在任务栏右下角通过鼠标图标的颜色查看电量区间；悬停或右键可查看具体百分比和状态。

**最新版本：v1.3.0 · Plan B Icons**　|　Windows 10/11 · C++14 / Win32 · 中文界面

> 本项目为非官方社区工具，与 Razer 无关联。v1.3.0 已由项目使用者在 DeathAdder V3 Pro 的 Windows 环境中验证可运行；其他设备及连接方式未全面测试。

## 功能

- **简洁的鼠标图标**：圆润外形，独立的顶部滚轮；不放数字、不放品牌 Logo。
- **颜色表示电量**：五档电量颜色，低于 20% 始终为红色。
- **充电状态**：读取到正在充电时，保留滚轮并在其下方显示闪电；充电状态未知时不误报。
- **查看精确百分比**：悬停提示或右键菜单显示电量和设备状态。
- **轻量运行**：原生 C++14 / Win32；无需 Qt、Electron 或 WebView；约每 30 秒自动刷新一次。
- **易于操作**：左键刷新，右键使用中文菜单，可切换开机自启动。
- **单文件交付**：11 种预制 ICO 内嵌于 EXE，无需额外图标文件。

## 电量图标规则

| 电量 | 颜色 | 使用中 | 充电中 |
| --- | --- | --- | --- |
| 80–100% | 绿色 | 滚轮 | 滚轮 + 闪电 |
| 60–79% | 浅绿色 | 滚轮 | 滚轮 + 闪电 |
| 40–59% | 黄色 | 滚轮 | 滚轮 + 闪电 |
| 20–39% | 橙色 | 滚轮 | 滚轮 + 闪电 |
| 0–19% | 红色 | 滚轮 | 滚轮 + 闪电 |
| 未连接或读取失败 | 灰色 | 状态未知 | 状态未知 |

**颜色代表电量，不代表是否充电。** 例如电量 15% 正在充电时，图标仍然是红色，但会出现闪电。充电状态无法确认时，只显示已经读取到的电量，不会擅自出现闪电。

## 下载与安装

1. 打开 [Releases](https://github.com/spriteyyds-ui/RazerBatteryLite/releases)，下载最新版本的 **`RazerBatteryLite.exe`**（在 Release 的 **Assets** 中，不是自动生成的 Source code ZIP）。
2. 退出旧版程序，将 EXE 放到固定目录，例如 `%LOCALAPPDATA%\Programs\RazerBatteryLite\`。
3. 双击 EXE 运行。图标位于任务栏右下角；如果隐藏在 `^` 内，可将它拖到可见区域。
4. **左键**立即刷新，**右键**查看电量、状态及开机自启动选项。

可选：项目源码附带 `install-user.bat`，将 `RazerBatteryLite.exe` 放在项目根目录或 `dist/` 后运行，可将其复制到当前用户的固定安装目录，再通过菜单启用开机自启动。

> **Windows 安全提示：** 未签名或信誉较低的 EXE 可能触发 SmartScreen / Smart App Control。确认下载来源和文件安全性；不要关闭 Windows 安全防护。被安全机制阻止的程序可能无法正常开机自启。

## 兼容性

- **已由项目使用者验证：** Razer DeathAdder V3 Pro，Windows，设备 `VID_1532 / PID_00B7`；v1.3.0 可正常运行。
- 程序包含独立的电量查询与充电状态查询。因连接方式、固件或接收器不同，**不保证所有设备都能准确返回充电状态**。
- 其他雷蛇型号、有线与 HyperPolling 连接方式并未全面验证。若出现灰色图标，请检查接收器连接，移动鼠标唤醒后点击“立即刷新”。

## 从源码构建

### GitHub Actions

1. 打开 [Actions](https://github.com/spriteyyds-ui/RazerBatteryLite/actions)。
2. 选择 **Build Windows EXE**，打开最近一次构建成功的运行记录（也可使用 **Run workflow** 手动触发）。
3. 下载 **Artifacts → `RazerBatteryLite-Windows-x64`**，解压获得 EXE。

> Actions 的 Artifact 是构建产物，不会自动附加到 Release。发布版本时需要将 EXE 上传到 Release 的 **Assets**。

### Windows 本地构建

安装 Visual Studio 2022 C++ Build Tools 与 Windows SDK，然后打开 **x64 Native Tools Command Prompt for VS 2022**：

```bat
cd /d D:\Codex\RazerBatteryLite
build.bat
```

生成路径：`dist\RazerBatteryLite.exe`。

## 项目结构

```text
RazerBatteryLite/
├── src/RazerBatteryLite.cpp        # C++14 / Win32 主程序
├── res/
│   ├── resource.h                  # 图标资源 ID
│   ├── icons.rc                    # Windows 资源脚本
│   └── icons/*.ico                 # 11 个多尺寸 ICO
├── assets/
│   ├── svg/*.svg                   # 可编辑的 SVG 原图
│   └── png/*_256.png               # PNG 预览
├── PlanB_icons_preview.png        # 图标效果预览
├── .github/workflows/windows.yml  # Windows 自动构建
├── build.bat                       # 本地编译脚本
├── install-user.bat                # 可选安装脚本
├── NOTICE.md                       # 参考项目与协议说明
└── LICENSE-original-MIT.txt        # 参考项目的 MIT 许可
```

## 常见问题

**为什么没有闪电？** 只有查询明确返回“正在充电”时才显示闪电。无法查询充电状态时，仍可正常显示电量颜色和百分比。

**为什么图标变灰？** 鼠标可能休眠、接收器断开或查询失败。移动鼠标，检查连接并点击“立即刷新”；必要时退出 Synapse 后再试。

**开启开机启动却没有运行？** 确保 EXE 在固定位置且未被移动，并检查 Windows 启动项及系统安全拦截。

## 开源与致谢

项目参考了 [incconutwo/mouse-battery-tray](https://github.com/incconutwo/mouse-battery-tray) 等开源实现，相关来源和署名详见 [NOTICE.md](./NOTICE.md)。仓库保留了参考项目的 [MIT 许可证文本](./LICENSE-original-MIT.txt)。**该许可证文件属于参考项目，不自动决定本项目自身的开源授权方式**；正式授权请以本仓库单独声明的 LICENSE 为准。

---

*一眼看电量，无需打开整套鼠标管理软件。*
