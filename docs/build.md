# Build（C++17 + CMake + vcpkg）

## 前置

- CMake 3.20+
- C++17 编译器（Clang/GCC/MSVC）
- Ninja（推荐）
- vcpkg（建议用 manifest 模式）

## 1) 配置 vcpkg

设置环境变量 `VCPKG_ROOT` 指向你的 vcpkg 目录，例如：

- macOS/Linux: `export VCPKG_ROOT=/path/to/vcpkg`
- Windows (PowerShell): `$env:VCPKG_ROOT="C:\path\to\vcpkg"`

## 2) 构建

使用 CMake Presets：

- Debug（vcpkg）：`cmake --preset default && cmake --build --preset default`
- Release（vcpkg）：`cmake --preset release && cmake --build --preset release`
- Linux 系统包（不依赖 vcpkg，apt 安装 libsdl2-dev/libfmt-dev/libboost-dev/ffmpeg 开发头）：`cmake --preset linux-system && cmake --build --preset linux-system`，测试用 `ctest --preset linux-system`

libtorrent（P2P 下载内核，docs/mvp.md §5 P4）vendor 在 `third_party/libtorrent/`（钉 v2.0.15，BSD-3，见该目录 `VENDOR.md`），随构建一起编译，只需要 Boost 头（apt `libboost-dev`、vcpkg `boost-system`）；无 OpenSSL（`encryption=OFF` 走捆绑 SHA-1/SHA-256）。

输出目录：`build/`（linux-system 为 `build-system/`）

## 3) 运行

- 窗口播放：`./build/soar --backend=ffmpeg <path-or-url>`
- CLI 冒烟（无窗口）：`./build/soar --headless --backend=ffmpeg <path-or-url>`
- 核心演示（无多媒体依赖）：`./build/soar --backend=null <path-or-url>`

选项说明：

- `--headless`：不开窗口，执行打开 → 播放 → Seek → 暂停 → 停止后退出，用于冒烟测试
- `--backend=`：选择后端（`ffmpeg`、`null`），默认 `null`；请求的后端不可用时回退并提示
- `.torrent` 位置参数或 `magnet:` URI 即 P2P 源（docs/mvp.md §5 P4）：本地 libtorrent 会话顺序下载，桥接成 `http://127.0.0.1:<port>/` 交给普通播放链路；`--torrent-store=`（数据落盘目录，默认 `<tmp>/soar-torrent`）、`--torrent-peer=host:port`（直连种子，可重复）、`--torrent-index=N`（多文件种子选第几个文件）。magnet 取 `xt=urn:btih:`（40 位十六进制或 32 位 base32 均可），tracker 走 magnet 内 `tr=`、peer 可用 `x.pe=` 内置或 `--torrent-peer=` 直连，公网 DHT bootstrap 自动进行；swarm 送不来元数据 60 秒后显式报错退出
- 本地 P2P 走查工具：`./build/seed_torrent --file=<媒体> --out=<x.torrent> [--port=6881] [--rate-kb=N]`（限速模拟慢 peer），然后 `soar --torrent-peer=127.0.0.1:6881 <x.torrent>`，或等价 magnet：`soar 'magnet:?xt=urn:btih:<seeder 打印的 info hash>&x.pe=127.0.0.1:6881'`

## 4) 测试

```bash
ctest --preset default
```

单元测试以 `NullBackend` 为测试替身，不依赖 FFmpeg/SDL2；可用 `SOAR_BUILD_TESTS=OFF` 关闭测试构建。

## 5) 可选能力开关

CMake 选项（全部默认 `ON`，缺失时降级而不是构建失败）：

| 选项 | 作用 | 缺失时 |
|---|---|---|
| `SOAR_ENABLE_FFMPEG` | FFmpeg 后端（pkg-config 探测 libav*/sw*） | 只有 `NullBackend`，CLI 仍可用 |
| `SOAR_ENABLE_SDL2` | SDL2 窗口与音频（需 **SDL ≥ 2.0.18**） | 只构建 CLI（`--headless`） |
| `SOAR_ENABLE_IMGUI` | 窗口上的播放器 UI 叠加层（Dear ImGui v1.91.9b，`imgui_impl_sdlrenderer2`） | 退化为裸视频窗口（无控制栏） |
| `SOAR_BUILD_APP` / `SOAR_BUILD_TESTS` | 应用 / 测试目标 | — |

`SOAR_ENABLE_IMGUI` 需要在**配置阶段**能访问 GitHub：CMake 把 ImGui 以 `--depth 1` 克隆到 `<build>/_deps/imgui-src`（不进仓库、不进分发物）。无网络或无 git 时会打印提示并降级为裸窗口；`-DSOAR_ENABLE_IMGUI=OFF` 可完全跳过这次拉取。已克隆的目录会被后续配置复用。

