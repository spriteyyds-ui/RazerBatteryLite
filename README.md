# 蝰蛇电量 Lite（Razer Battery Lite）

专为 Windows 10/11 上的雷蛇炼狱蝰蛇 V3 Pro（DeathAdder V3 Pro）设计的极简任务栏电量工具。

## 功能

- 托盘直接显示 **数字电量**（例如 `85`），无需悬停。
- 全中文菜单：电量、立即刷新、开机自动启动、关于、退出。
- 每 30 秒查询一次电量；低于 50% 显示橙色、低于 20% 显示红色。
- 无安装程序、无广告、无网络请求、无遥测、无设置文件。
- 不需 Python/.NET/Razer Synapse，不修改鼠标的 DPI、回报率或固件。
- 同一用户只会启动一个实例，断开时显示 `--`。

## 重要：兼容性

支持代码已包含：Razer VID `0x1532`；DeathAdder V3 Pro 标准无线 `PID 0x00B7`、有线 `PID 0x00B6`，可能的修订版 `PID 0x00C3/0x00C2`，以及 HyperPolling Dongle `PID 0x00B3`。协议为 Razer 90 字节 HID report，`class 0x07`、`GET_BATTERY_LEVEL 0x80`、transaction ID `0x1F`。

**重要**：该版本未在真实 DeathAdder V3 Pro 及其不同接收器上实机测试。普通 2.4G 接收器的证据较充分；HyperPolling 接收器是否能直接向鼠标转发这个查询仍需实际验证。如果读不到会显示 `--`，不会假装读取成功。

**注意**：可能需要先完全退出 Razer Synapse，因为它有时会独占 HID 接口。如果休眠，移动鼠标后右键点「立即刷新」。

## 构建 Windows EXE

### 方法 A：本地（建议）

1. 安装 Visual Studio 2022 的免费 **Build Tools**，勾选「使用 C++ 的桌面开发」（含 Windows SDK）。
2. 打开开始菜单里的「x64 Native Tools Command Prompt for VS 2022」。
3. 进入本文件夹，运行 `build.bat`。
4. 得到 `dist\RazerBatteryLite.exe`，直接双击即可，无需管理员权限。

也可手动运行：

```bat
cl /nologo /std:c++14 /O2 /EHsc /W4 /DUNICODE /D_UNICODE /utf-8 /Fe:dist\RazerBatteryLite.exe src\RazerBatteryLite.cpp /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib setupapi.lib hid.lib advapi32.lib
```

### 方法 B：GitHub Actions（不用在本机装编译器）

把本项目文件上传到你自己的 GitHub 仓库，在 `Actions → Build Windows EXE → Run workflow` 中运行，完成后下载构建产物 `RazerBatteryLite-Windows-x64`。如编译器环境变化，可能需要更新 workflow 中的 VS 路径。

## 使用

双击 exe，系统托盘出现数字。若显示在右下角 `^` 折叠区，把它拖到可见区即可。右键菜单中勾选「开机自动启动」会写入当前用户的 `HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run` 注册表项，无需管理员权限。

## 隐私与资源

不联网、不写日志、不保存历史。CPU 主要在每 30 秒的一次 HID 查询时短暂活动。使用原生 Win32 API，依赖系统 DLL，无 Python/.NET 运行时。**可执行文件大小和实际内存占用应以 Windows 编译与实测为准**，此处不承诺具体数字。

## 排错

1. 鼠标是否开启、USB 2.4G 接收器是否插好？
2. 尝试关闭 Synapse 与其他雷蛇管理软件；鼠标移动后刷新。
3. Windows「设备管理器 → 鼠标/HID 设备 → 属性 → 详细信息 → 硬件 ID」，确认 VID/PID。不同修订版或第三方转接器的 PID 可能不一样。
4. 如仍显示 `--`，说明 HID 查询尚不兼容当前接口；并不等于鼠标没电。报告 VID/PID 和接收器型号后可调整匹配与事务方式。

## 开源声明

本程序为新的原生 C++14 实现，设计和 HID 协议研究参考了 `incconutwo/mouse-battery-tray` 等项目；原参考项目采用 MIT 许可证。保留其开源声明见 `NOTICE.md` 和 `LICENSE-original-MIT.txt`。Windows HID 与 Razer 协议详见 README 末尾的参考链接。

### 参考来源

- 原始项目：https://github.com/incconutwo/mouse-battery-tray
- 独立硬件验证及报告格式：https://github.com/OpenMouse-Project/mouse-protocol/blob/main/docs/razer-testing.md
- Razer 90 字节协议说明：https://hyperb1iss.github.io/uchroma/reference/razer-protocol-reference.html
- V3 Pro 型号及 VID/PID：https://github.com/xzeldon/razer-battery-report
