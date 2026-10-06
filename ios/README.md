# Soar iOS（骨架批，低优先级填空）

本目录是 Soar 的 iOS 层骨架。按既定口径：**只写代码、GitHub macOS runner
验证（push `ios/**` 变更自动触发 + workflow_dispatch 手动）、不占本地
资源、不卡主线**。

## 当前状态（如实）

| 层 | 状态 | 说明 |
|---|---|---|
| `SoarKit/`（Swift Package） | ✅ CI macOS 验证通过（run 37540311541，iOS build + macOS test 两步全绿） | 纯 Swift API 层：播放控制门面、状态/事件模型、`MediaEngine` 协议与 `NullEngine` 状态机镜像（与桌面 `NullBackend` 同语义）。不依赖任何 C++/FFmpeg。 |
| `Bridge/soar_bridge.h` | ⬚ 设计稿 | C 桥接口声明（Player 门面的 C ABI 映射）。**未接线**——声明仅供评审，不参与构建。 |
| 核心交叉编译 | ❌ 未开始 | `soar_core`（C++17）→ iOS xcframework（含 FFmpeg 后端的交叉工具链）是后续批；届时以 SwiftPM `binaryTarget` 接入 `MediaEngine` 的 `CxxEngine` 实现。 |
| App 壳（SwiftUI） | ❌ 未开始 | 待 xcframework 批之后（播放视图需要真帧输出）。 |

## 构建 / 测试（macOS runner 或任意装了 Xcode 的机器）

```bash
cd ios/SoarKit
xcodebuild build -scheme SoarKit -destination 'generic/platform=iOS'
xcodebuild test  -scheme SoarKit -destination 'platform=macOS'
```

CI：`.github/workflows/ios.yml`（macos-latest，build iOS + test macOS
两步；push 到 `ios/**` 自动触发，也可 `workflow_dispatch` 手动）。

## 设计要点

- `PlayerController` 是 Swift 侧的门面：状态与事件用 `AsyncStream` 暴露，
  控制方法同步返回；不依赖 SwiftUI（可以在任何 UI 栈下用）。
- `MediaEngine` 协议是内核抽象的 Swift 镜像（对应桌面 `IBackend` +
  `Player` 门面的合并面）：`NullEngine` 提供可测试的状态机（时间由
  `advance(by:)` 手动推进，测试不赌实时时钟——与仓库测试纪律一致）。
- 事件枚举与桌面端一一对应（StateChanged / MediaInfoChanged /
  PositionChanged / Error / BufferingStarted / BufferingEnded /
  DownloadProgress），语义注释直接引自 `include/soar/core/backend.h`。
