# Catwhale Pet

[English](README.md) | [简体中文](README_CN.md)

适用于 Linux Wayland 的透明桌面宠物。左键单击角色可以互动，按住左键拖动可以移动整个宠物窗口，右键单击可以打开菜单。点击角色周围的透明区域时，鼠标事件会交给下方的窗口。

Catwhale Pet 使用 C++20、Qt 6 和 FFmpeg。GNOME、KDE Plasma、Hyprland 和 niri 的使用方法见[桌面配置指南](docs/Desktop.md)。

## 运行

启用 Nix flakes 后，在项目目录中运行：

```bash
nix run
```

需要为桌面快捷键或开机自启指定可执行文件时，可以先构建：

```bash
nix build
./result/bin/catwhale-pet
```

程序在 Wayland 会话中使用原生 Wayland 后端。媒体联动通过 `playerctl` 读取播放器的本地 MPRIS 状态。

## 功能

- 透明动画、鼠标互动以及随时间变化的昼夜行为。
- 自定义皮肤，以及按指定时间范围循环播放动画片段。
- 媒体联动、本地代理面板和按需打开的硬件状态面板。
- 设置窗口、系统托盘控件（取决于桌面环境）和可选的 XDG 自启。
- niri 下可选的工作区跟随。

配置文件详见[配置说明](docs/configuration.md)。

## 命令

可在终端中执行这些命令，也可以绑定到桌面快捷键。再次运行程序时，命令会转发给已启动的实例。

| 命令 | 操作 |
| --- | --- |
| `catwhale-pet --toggle-menu` | 打开或关闭宠物菜单 |
| `catwhale-pet --open-settings` | 打开设置 |
| `catwhale-pet --toggle-pet` | 显示或隐藏宠物 |
| `catwhale-pet --quit` | 退出 |
| `catwhale-pet --version` | 显示版本 |

## 硬件加速

渲染优先使用 Vulkan，依次回退到 OpenGL 和软件渲染。FFmpeg 优先尝试 NVIDIA NVDEC，其次是 VA-API，最后用 libvpx 在 CPU 上解码 VP8 和 VP9。硬件解码会分别处理 WebM 的颜色和透明度视频流，并按时间戳匹配帧。

启动一个新实例时加上 `--diag`，可以查看平台以及实际使用的后端。使用 `--decode=cpu` 可以单独测试软件解码，使用 `--renderer=opengl` 可以尝试 OpenGL，使用 `--no-gpu` 可以同时启用软件渲染和软件解码。

## 开发

```bash
nix develop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/catwhale-pet
```

在 Nix 之外构建，需要 CMake、支持 C++20 的编译器、Qt 6.6 或更新版本（含 Qt Quick、Widgets 和 Wayland 平台插件），以及 FFmpeg 开发库（`libavformat`、`libavcodec`、`libavutil` 和 `libswscale`）。FFmpeg 需要支持 libvpx 才能在软件解码时保留透明度。NVDEC 和 VA-API 还需要相应的显卡驱动和 FFmpeg 支持。

源码目录和播放流程见[架构说明](docs/architecture.md)。

## 许可证与致谢

Catwhale Pet 采用 [MIT 许可证](LICENSE)。角色美术、动画和原始应用概念来自 [timevisitor/catwhale-pet](https://github.com/timevisitor/catwhale-pet)。

Qt 和 FFmpeg 是构建和运行时依赖，各自适用其许可证。详情见[第三方许可证说明](THIRD_PARTY_NOTICES.md)。
