# Mini DAW

[English](README.en.md) | [中文详细使用说明](docs/usage.zh-CN.md)

Windows 10 / 11 x64 实时音频调音台，支持 ASIO、64 位 VST2 / VST3 效果插件和 Windows 系统音频回送。

**powered by WanAn522z** · [项目主页](https://github.com/wanan522z/MINI-DAW) · [Aggregate Audio ASIO](https://github.com/wanan522z/AggregateAudio-asio)

Mini DAW 专注于通道路由、插件处理和实时混音。EQ、压缩、降噪等音色处理由加载的插件完成；ASIO 驱动在独立项目中维护，本仓库可单独编译。

## 功能

- 默认 12 条通道，最多 32 条；每条通道支持单声道 / 立体声输入输出、12 个效果插件槽、推子、电平表和静音。
- 加载 64 位 VST2 DLL 和 VST3 插件；后台扫描使用独立进程，并缓存结果、合并重复版本。
- 支持主输入发送、插件侧链以及 Pre-FX / Post-FX；检测并阻止宿主路由反馈环路。
- 拖放排列通道、插件和发送，跨通道复制插件，支持通道复制、颜色设置和最近 32 次宿主操作的撤销 / 重做。
- 内录通道采集 Windows 输出，或合并指定 ASIO 端口的宿主输出与 Windows 系统声音。Windows 来源跟随所选设备的系统主音量和静音。
- 内录输入菜单区分“ASIO 设备”和“Windows 输出”，Windows 列表排除已识别的 ASIO 占用设备。
- 设备断开时保留名称和绑定；同一设备重新接入后自动恢复，也可手动切换。
- `.minidaw` 预设保存插件状态、通道、路由、设备绑定和窗口布局；记住最近打开的预设。
- 支持 Windows 触屏和触控笔、平滑滚动、可调整通道尺寸，以及后台音频处理和托盘恢复。

## 安装与使用

需要 Windows 10 / 11 x64 和可用的 64 位 ASIO 驱动。默认使用 Aggregate Audio，也支持其他已安装的 ASIO 驱动。

构建后的安装包位于 `dist/MiniDAW-<版本>-x64-Setup.exe`。组件列表包含必选的 Mini DAW 和可选的 Aggregate Audio ASIO，并包含离线 Microsoft Visual C++ x64 运行库。

1. 安装 Mini DAW；没有 ASIO 驱动时，可同时勾选 Aggregate Audio。
2. 在 ASIO 控制面板中选择设备声道、采样率和缓冲区，然后保存。
3. 在 Mini DAW 设置中选择驱动，为通道分配输入和输出，再加载效果插件。
4. 使用发送将通道连接到其他通道或插件侧链，保存工程或预设。

已有 Aggregate Audio 时，Setup 默认只安装主程序。需要升级驱动时请手动勾选 Aggregate Audio；当前配套修复版本为 **v21**。安装或升级前退出正在使用该驱动的软件。两个软件保留独立卸载入口，卸载 Mini DAW 会保留共享驱动、运行库和个人设置。

### Windows 系统声音

通道右键选择“转为内录通道”，再从输入菜单选择来源：

- **ASIO 设备**：使用宿主写给该输出端口的声音，并按内录设置中的映射合并 Windows 回送。
- **Windows 输出**：直接采集可共享的 Windows 播放设备。单实例设备若已被 ASIO 占用，应先在 ASIO 面板中取消勾选，再用于 Windows 输出。

内录设置支持手动指定 Windows 设备和声道。若 Windows 回送已经包含宿主声音，可取消“合并宿主输出”避免重复。直接 Windows 来源需要通过通道发送来接收额外的宿主音频。

### 触屏手势

| 操作位置 | 手势 |
| --- | --- |
| 通道标题、空白处、插件槽、加载预设按钮 | 按住约 650 毫秒打开对应菜单 |
| 底部发送小旋钮 | 按住约 350 毫秒打开菜单；继续按住至约 850 毫秒，收起菜单并进入浮动拖动 |
| 底部发送小旋钮 | 直接滑动调节音量；双击恢复 0 dB |
| 通道标题 | 双击重命名；拖动交换或插入通道 |

长按允许少量触点抖动；第二个触点会取消待触发的长按。鼠标发送旋钮仍支持右键菜单和约 550 毫秒长按浮动拖动。

## 从源码编译

需要 Visual Studio 2022 的 C++ 桌面开发工具、Windows SDK、CMake 3.24+ 和 Git，目标架构为 x64。

JUCE 8.0.12 固定提交为 `29396c22c93392d6738e021b83196283d6e4d850`。没有本地 `third_party/JUCE/` 时，CMake 会联网获取该版本。

VST2 宿主另需合法取得的 SDK 接口头文件 `pluginterfaces/vst2.x/aeffect.h` 和 `aeffectx.h`。默认放在 `vendor/vst2-sdk/` 下，也可通过 `MINIDAW_VST2_SDK` 指定 SDK 根目录；这些头文件不随仓库提交。

```powershell
git clone https://github.com/wanan522z/MINI-DAW.git
cd MINI-DAW
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=OFF -DMINIDAW_VST2_SDK="C:/SDKs/vst2-sdk"
cmake --build build --config Release --target mini_daw --parallel 4
```

可执行文件生成于 `build/mini_daw_artefacts/Release/MiniDAW.exe`。使用默认 SDK 目录时可省略 `-DMINIDAW_VST2_SDK`。

### 可选测试

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --target mini_daw mini_daw_routing_test --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

`tools/build.ps1` 会构建、运行测试并更新 `out/MiniDAW.exe`；程序被占用时保存为 `out/MiniDAW-next.exe`。硬件和实际音质仍需在目标设备上验证。

## 生成 Setup

先准备独立 [Aggregate Audio](https://github.com/wanan522z/AggregateAudio-asio) 项目的安装包及 `dist/package-manifest.json`，然后执行：

```powershell
.\packaging\build-installer.ps1 -AsioProject "C:\Projects\AggregateAudio-asio"
```

脚本校验 ASIO 安装包 SHA256、Inno Setup 7.1.0 与 Microsoft 运行库签名，编译 Mini DAW 并生成 Setup；不会运行测试或安装程序。已有构建可使用 `-SkipBuild`。参数及安装行为见 [打包说明](packaging/README.md)。

生成的 EXE、Setup、编译缓存、个人预设和本地 SDK 均由 `.gitignore` 排除，不进入源码仓库。

## 设置、日志与限制

- 工程、插件缓存和最近预设：`%APPDATA%\MiniDAW\`。
- 宿主音频错误：`%APPDATA%\MiniDAW\audio-errors.log`。
- Aggregate Audio 配置与 v21 驱动错误日志：`%LOCALAPPDATA%\AggregateASIO\`。
- ASIO 占用错误修复记录：[保存配置时设备被占用](docs/asio-save-device-busy.md)。

ASIO 用于主音频处理，WASAPI 只用于 Windows 系统回送，不作为主引擎后备。回送具有额外采集延迟；硬件直通、DSP 混音和独占播放是否可被采集取决于设备驱动。单实例设备能否同时供 ASIO 和 Windows 使用，也由驱动决定。

目前没有插件延迟补偿、多轨音频录制或插件播放处理的进程隔离。原生插件参数的撤销由插件自身负责。

## 目录与依赖

```text
src/          调音台、宿主、路由、Windows 回送、插件扫描
assets/       图标和工具栏资源
tests/        路由回归测试
docs/         使用说明与修复记录
tools/        构建脚本和图标生成工具
packaging/    Setup 源码、打包脚本与安装说明
third_party/  JUCE 版本说明与许可证
vendor/       本地 VST2 SDK 放置说明
```

JUCE 提供 AGPLv3 和商业许可；依赖许可见 [third_party/README.md](third_party/README.md)、[JUCE 许可](third_party/JUCE-LICENSE.md) 和 [打包依赖声明](packaging/THIRD-PARTY-NOTICES.txt)。VST2 SDK、ASIO 驱动及效果插件分别适用其提供方的许可。
