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

- Debug: `cmake --preset default && cmake --build --preset default`
- Release: `cmake --preset release && cmake --build --preset release`

输出目录：`build/`

## 3) 运行

- 窗口播放：`./build/soar --backend=ffmpeg <path-or-url>`
- CLI 冒烟（无窗口）：`./build/soar --headless --backend=ffmpeg <path-or-url>`
- 核心演示（无多媒体依赖）：`./build/soar --backend=null <path-or-url>`

选项说明：

- `--headless`：不开窗口，执行打开 → 播放 → Seek → 暂停 → 停止后退出，用于冒烟测试
- `--backend=`：选择后端（`ffmpeg`、`null`），默认 `null`；请求的后端不可用时回退并提示

## 4) 测试

```bash
ctest --preset default
```

单元测试以 `NullBackend` 为测试替身，不依赖 FFmpeg/SDL2；可用 `SOAR_BUILD_TESTS=OFF` 关闭测试构建。

