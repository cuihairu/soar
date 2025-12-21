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

- `./build/soar <path-or-url>`

