# RazerBatteryLite v1.3.0 — 方案 B 图标资源版

适用于 Windows 10/11 的雷蛇炼狱蝰蛇 V3 Pro 托盘电量工具。**图标在编译时内嵌进 EXE，运行时不需要额外 ICO/PNG 文件。**

## 功能

- Windows 托盘显示 **方案 B：柔和圆润鼠标轮廓**，顶部滚轮与外框分离；不显示数字，也没有品牌 logo。
- 五档颜色：80–100% 绿色、60–79% 浅绿、40–59% 黄色、20–39% 橙色、0–19% 红色，读取失败灰色。
- 充电中保留滚轮并在下方显示闪电；**低于 20% 即使充电也为红色**。
- 悬停和右键中文菜单显示精确百分比和充电状态。每 30 秒刷新，左键可立即刷新，支持开机自启。
- 沿用 v1.2.1 的 `0x07/0x80` 电量查询及独立的 `0x07/0x84` 充电状态查询。不要求安装 Razer Synapse。
- Windows 11 风格紧凑自绘菜单，无 Qt、Electron 或 WebView。

## 文件目录

```text
RazerBatteryLite/
  src/RazerBatteryLite.cpp     完整 Windows C++14 源码
  res/resource.h              Win32 资源编号
  res/icons.rc                编译图标资源
  res/icons/*.ico             11 个多尺寸 Windows 图标
  assets/svg/*.svg            11 个可修改矢量母版
  assets/png/*_256.png        11 个透明背景高清预览
  PlanB_icons_preview.png     色阶与原尺寸效果预览
  .github/workflows/windows.yml  GitHub Actions 构建
  build.bat                   本机 MSVC 构建
  install-user.bat            可选固定目录安装
  NOTICE.md, LICENSE-original-MIT.txt  协议及署名
```

## 从 VS Code 上传 GitHub（推荐）

把本 ZIP 内**所有文件夹和文件**覆盖到 `D:\Codex\RazerBatteryLite`，原有的 `.github`、`src` 等目录必须保留。尤其要把 `res` 文件夹完整复制进去，否则 GitHub 编译会报资源文件不存在。

```powershell
cd D:\Codex\RazerBatteryLite
git add .
git commit -m "feat: embed predesigned Plan B mouse tray icons"
git push
```

进入 GitHub 仓库的 Actions，下载最新成功构建的产物 `RazerBatteryLite-Windows-x64`；解压得到 EXE。建议先退出托盘中的旧版本再运行新版本。

## 本机编译

需 Visual Studio 2022 C++ Build Tools + Windows SDK。打开 **x64 Native Tools Command Prompt for VS 2022** 后，进入项目目录运行 `build.bat`。

`build.bat` 会先用 `rc.exe` 将 11 个 ICO 编译为 `dist\RazerBatteryLite.res`，再通过 `cl.exe` 链接为 `dist\RazerBatteryLite.exe`。

## 图标资源设计

`res/icons/` 每个 `.ico` 含 16、20、24、32、48、64、128px 共 7 个分辨率，并带有透明背景；`assets/svg/` 是可独立编辑的 SVG。可修改 SVG 后重新生成 ICO，但修改后的 ICO 必须同步到 `res/icons/`，重新编译 EXE。

## 注意事项

- 不保证其他雷蛇设备和所有接收器的充电状态读取；未知充电状态不显示闪电，**电量读取不会受影响**。
- 该项目未在本环境通过 Windows MSVC 实际编译，也未实测 V3 Pro；请先保留上一版稳定 EXE。
- GitHub 下载的未签名 EXE 可能被 Windows SmartScreen 警告；这是信誉/签名问题。不要关闭系统安全保护。固定安装目录后，使用菜单中的“开机自启动”。
- 原项目 MIT 许可与协议来源归属记录见 `NOTICE.md` 和 `LICENSE-original-MIT.txt`。
