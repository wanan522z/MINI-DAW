# Mini DAW

Windows 10/11 x64 的 ASIO / VST3 麦克风调音台，powered by WanAn522z。

本项目从 Aggregate Audio 驱动工程独立迁出，可单独编译。音色处理全部由加载的效果插件完成，没有自写 EQ、压缩、降噪或限幅。

## 运行

当前可运行版本：`out/MiniDAW.exe`。安装并配置 Aggregate Audio ASIO 驱动后启动程序；首次打开默认 12 条通道，自动选用 `Aggregate Audio`。不提供 WASAPI、DirectSound 或 Windows Wave 后备路径。未安装驱动时界面显示缺少驱动的提示。

Aggregate Audio 仍由独立的 `asio` 项目维护，不需要把驱动源码放进此仓库。`out/AggregateAudioConfig.exe` 是迁移时保留的配套配置程序；它用于配置已安装的驱动，不能代替驱动安装。

## 调音台

- 每条通道一个推子，集成立体声电平；左侧刻度 +12 dB 至 −∞，默认 0 dB。双击推子恢复 0 dB，底端为数字静音。
- 默认 12 条，通道阵列最右侧“＋”可继续添加，当前最多 32 条；支持横向滚动。
- 黑色输入栏选择 ASIO 单声道或立体声对，红色输出栏设置直接输出。默认不连接直接输出。
- 每条通道有 12 个蓝色 VST3 槽。单击切换启用，双击打开插件原生界面，空槽双击打开效果列表；右键可卸载。
- 扫描标准 VST3 路径，也可添加自定义目录；列表过滤乐器和没有音频输入/输出的插件。当前支持 64 位 VST3，不支持 VST2 DLL。
- 绿色区域默认两个发送旋钮。右键选择已有通道的主输入或插件侧链总线，拖动设置发送量，初始 −∞。第二个旋钮连接后自动出现第三个，后续依次增加；发送增多时整块通道阵列上下滚动。
- 推子上方“接收”下拉栏展示来源，菜单列出主输入及“槽号 · SC 总线编号 · 插件总线名称”。SC 1 对应插件输入总线索引 1，主输入索引为 0；只提供插件实际声明且启用的总线。

发送位于插件链和推子之后，接收混入目标主输入或指定插件的侧链。路由按拓扑顺序处理，禁止形成反馈环路。采样率和缓冲由 ASIO 驱动决定。目前没有插件延迟补偿、多轨录音或插件进程隔离。

关闭窗口或点击“后台驻留”会保留音频处理；双击托盘恢复，右键保存并退出。工程和插件状态保存于 `%APPDATA%/MiniDAW/session-v2.xml`，插件列表缓存为同目录的 `effects-vst3.xml`。迁移项目目录不会修改这些个人设置。

## 编译与测试

需要 Visual Studio 2022 C++ 工具链、Windows SDK、CMake 3.24+ 和 Git，目标架构 x64。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target mini_daw mini_daw_routing_test --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

也可执行 `./tools/build.ps1`，自动查找 Visual Studio 的 CMake，完成构建与测试，并更新 `out/MiniDAW.exe`；如果当前程序正在运行，会保留会话并把新构建保存为 `out/MiniDAW-next.exe`，退出当前程序后再替换。构建输出位于 `build/mini_daw_artefacts/Release/MiniDAW.exe`。

JUCE 8.0.12 固定提交 `29396c22c93392d6738e021b83196283d6e4d850`。迁移时保留了 `third_party/JUCE` 本地源码并忽略其 Git 提交；全新克隆缺少该目录时，CMake 会从官方仓库获取固定版本。此工程使用 JUCE 自带的 ASIO/VST3 宿主接口，不依赖原 `asio` 文件夹或外部 ASIO SDK 路径。

测试不打开麦克风。路由测试覆盖 0 dB、−∞、+12 dB、拓扑顺序、反馈环路和发送旋钮增长；宿主测试验证主输入发送、输出映射、静音及默认 12 通道。

额外验证命令：

```powershell
./out/MiniDAW.exe --verify-host C:/temp/host-report.xml "C:/Program Files/Common Files/VST3/FabFilter/FabFilter Pro-C 2.vst3"
./out/MiniDAW.exe --verify-asio C:/temp/asio-report.xml
./out/MiniDAW.exe --snapshot C:/temp/mixer.png
./out/MiniDAW.exe --snapshot C:/temp/mixer-end.png end
```

插件检查会发现、加载并处理插件，检查真实侧链总线、创建原生编辑器、获取状态和卸载。`--verify-asio` 短暂启动默认驱动并检查回调，无直接输出连接。`--snapshot` 不启动音频、不保存设置。迁移时保留的截图和报告位于 `out/verification`，真实硬件音质及延迟仍需试听验证。

## 目录

```text
src/                 调音台、ASIO/VST3 宿主、路由模型和验证入口
tests/               无硬件路由测试
tools/build.ps1      构建、测试及复制可执行文件
third_party/         依赖版本和许可说明、本地 JUCE 源码
out/                 可运行程序和验证结果（Git 忽略）
build/               当前目录重新生成的构建缓存（Git 忽略）
```

JUCE 模块采用 AGPLv3 或 JUCE 商业许可，详见 [依赖说明](third_party/README.md) 和 [JUCE 许可](third_party/JUCE-LICENSE.md)。插件自身的许可由对应厂商管理。
