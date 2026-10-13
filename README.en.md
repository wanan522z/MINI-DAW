# Mini DAW

[简体中文](README.md) | [Detailed Chinese guide](docs/usage.zh-CN.md)

A real-time audio mixer for Windows 10 / 11 x64, with ASIO, 64-bit VST2 / VST3 effects, and Windows system audio loopback.

**powered by WanAn522z** · [Project](https://github.com/wanan522z/MINI-DAW) · [Aggregate Audio ASIO](https://github.com/wanan522z/AggregateAudio-asio)

Mini DAW handles channel routing, plugin processing, and live mixing. EQ, compression, noise reduction, and other sound processing come from the effects you load. The Aggregate Audio driver is maintained separately; this application builds independently.

## Features

- 12 channels by default, up to 32; mono / stereo I/O, 12 effect slots per channel, faders, meters, and mute.
- 64-bit VST2 DLL and VST3 effects, with background scanning in separate processes, cached results, and duplicate-version grouping.
- Main-input sends, plugin sidechains, and Pre-FX / Post-FX taps, with host routing feedback detection.
- Drag channels, plugins, and sends to reorder them; copy plugins between channels; copy and color channels; undo / redo the last 32 host edits.
- Loopback channels capture Windows outputs or combine an ASIO port's host output with mapped Windows audio. System capture follows the selected endpoint's master volume and mute.
- Separate ASIO and Windows source groups; recognized outputs occupied by ASIO are excluded from the Windows group.
- Retain device names and bindings when disconnected, and reconnect them automatically when the same device returns.
- `.minidaw` presets store plugin state, routing, device bindings, and layout, with a recent-preset list.
- Windows touch / pen support, smooth scrolling, resizable channel strips, background processing, and a system tray entry.

## Install and run

Requires Windows 10 / 11 x64 and an available 64-bit ASIO driver. Aggregate Audio is the default; other installed ASIO drivers are supported.

The generated installer is `dist/MiniDAW-<version>-x64-Setup.exe`. Its component list includes the required application and optional Aggregate Audio ASIO, plus the offline Microsoft Visual C++ x64 runtime.

1. Install Mini DAW. Select Aggregate Audio as well if you need an ASIO driver.
2. Select device channels, sample rate, and buffer size in the ASIO control panel, then save.
3. Choose the driver in Mini DAW settings, assign channel inputs and outputs, and load effects.
4. Connect sends to other channels or plugin sidechains, then save your project or preset.

When Aggregate Audio is already installed, Setup selects only the application by default. Select the driver component manually to upgrade it; the current accompanying fix is **v21**. Close applications using the driver before installing or upgrading. The two applications have independent uninstall entries; uninstalling Mini DAW preserves the shared driver, runtime, and personal settings.

### Windows system audio

Use a channel's context menu to convert it to a loopback channel, then choose an input source:

- **ASIO devices:** the host's output for that port, combined with Windows loopback according to the loopback settings.
- **Windows outputs:** capture a Windows playback endpoint that supports shared access. For a single-instance endpoint occupied by ASIO, deselect it in the ASIO panel before using it as a Windows output.

Loopback settings allow manual endpoint and channel mapping. If Windows capture already contains host audio, disable the option to merge host output to avoid duplication. To add host audio to a direct Windows source, send the relevant host channels into the loopback channel.

### Touch gestures

| Control | Gesture |
| --- | --- |
| Channel title, empty strip area, plugin slot, Load Preset button | Hold for about 650 ms to open its menu |
| Small send knob at the bottom | Hold for about 350 ms to open the menu; keep holding until about 850 ms to close it and begin floating drag |
| Small send knob at the bottom | Drag immediately to adjust volume; double-tap to reset to 0 dB |
| Channel title | Double-tap to rename; drag to swap or insert channels |

Small contact jitter is tolerated. A second touch cancels a pending hold. Mouse send knobs retain right-click menus and floating drag after a hold of about 550 ms.

## Build from source

Requires Visual Studio 2022 with Desktop development with C++, the Windows SDK, CMake 3.24+, and Git. Build for x64.

JUCE 8.0.12 is pinned to commit `29396c22c93392d6738e021b83196283d6e4d850`. CMake fetches that version when a local `third_party/JUCE/` checkout is absent.

VST2 hosting also requires lawfully obtained SDK interface headers: `pluginterfaces/vst2.x/aeffect.h` and `aeffectx.h`. Place them under `vendor/vst2-sdk/`, or set `MINIDAW_VST2_SDK` to the SDK root. These headers are not included in this repository.

```powershell
git clone https://github.com/wanan522z/MINI-DAW.git
cd MINI-DAW
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=OFF -DMINIDAW_VST2_SDK="C:/SDKs/vst2-sdk"
cmake --build build --config Release --target mini_daw --parallel 4
```

The executable is `build/mini_daw_artefacts/Release/MiniDAW.exe`. Omit `-DMINIDAW_VST2_SDK` when using the default SDK directory.

### Optional tests

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release --target mini_daw mini_daw_routing_test --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

`tools/build.ps1` builds, runs tests, and copies the application to `out/MiniDAW.exe`. If that file is in use, it writes `out/MiniDAW-next.exe`. Hardware behavior and sound quality still require validation on the target devices.

## Build Setup

First prepare an installer and `dist/package-manifest.json` from the separate [Aggregate Audio](https://github.com/wanan522z/AggregateAudio-asio) project, then run:

```powershell
.\packaging\build-installer.ps1 -AsioProject "C:\Projects\AggregateAudio-asio"
```

The script verifies the ASIO installer's SHA256 and the signatures of Inno Setup 7.1.0 and the Microsoft runtime, builds Mini DAW, and generates Setup. It does not run tests or launch the installer. Use `-SkipBuild` for an existing build. See the [packaging guide](packaging/README.md) for parameters and installation behavior.

Generated executables, installers, build caches, personal presets, and local SDK files are excluded from the source repository by `.gitignore`.

## Settings, logs, and limitations

- Projects, plugin caches, and recent presets: `%APPDATA%\MiniDAW\`.
- Host audio errors: `%APPDATA%\MiniDAW\audio-errors.log`.
- Aggregate Audio settings and v21 driver error logs: `%LOCALAPPDATA%\AggregateASIO\`.
- Device occupation fix notes: [Saving ASIO settings while a device is busy](docs/asio-save-device-busy.md) (Chinese).

ASIO is the main audio engine. WASAPI is used only for Windows loopback capture, and is not a fallback engine. Loopback adds capture latency. Hardware direct monitoring, DSP mixing, and exclusive playback capture depend on the device driver. Sharing a single-instance device between ASIO and Windows is also driver-dependent.

Plugin delay compensation, multitrack audio recording, and process isolation for live plugin processing are not implemented. Native plugin parameter undo is handled by each plugin.

## Layout and dependencies

```text
src/          Mixer, host, routing, Windows loopback, plugin scanning
assets/       Icons and toolbar resources
tests/        Routing regression tests
docs/         Usage guide and fix notes
tools/        Build script and icon generator
packaging/    Setup sources, build script, and installation notes
third_party/  JUCE version information and licenses
vendor/       Local VST2 SDK instructions
```

JUCE offers AGPLv3 and commercial licensing. See [third_party/README.md](third_party/README.md), the [JUCE license](third_party/JUCE-LICENSE.md), and [packaging notices](packaging/THIRD-PARTY-NOTICES.txt). The VST2 SDK, ASIO drivers, and effect plugins are subject to their providers' licenses.
