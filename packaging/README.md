# Mini DAW Setup

Windows 10/11 x64 安装包提供组件列表：Mini DAW 主程序必选，Aggregate Audio
ASIO 可选。首次安装默认推荐两项；检测到已有 Aggregate Audio 时默认仅安装
Mini DAW。开始菜单和桌面快捷方式、卸载入口与离线 Visual C++ x64 运行库
均包含在安装包中。

Aggregate Audio 使用 `asio` 项目已生成的完整安装包，校验发布清单中的
SHA256 后原样嵌入，保留其驱动注册、升级文件占用检查及独立卸载逻辑。
卸载 Mini DAW 不删除共享驱动、运行库或个人设置。

```powershell
.\packaging\build-installer.ps1 -AsioProject 'C:\Projects\AggregateAudio-asio'
```

默认编译当前 Mini DAW Release，只构建主程序，不运行测试。使用 `-SkipBuild`
可打包已有的 Release 构建；`-ApplicationExe` 可明确指定现成的 x64 EXE。
可选参数还有 `-Compiler`、`-RuntimeInstaller` 和 `-CMakePath`。

默认从 ASIO 项目的 `dist/package-manifest.json` 读取发布版本和文件校验值，
并使用该项目缓存的 Inno Setup 7.1.0 编译器和 Visual C++ 运行库。编译器和
运行库的签名会在打包前检查。此脚本仅读取 ASIO 项目，不修改其文件。

输出位于 `dist/MiniDAW-<版本>-x64-Setup.exe`，同时生成 SHA256、打包清单和
安装说明。打包脚本不会运行生成的安装包或修改本机 ASIO 注册。
