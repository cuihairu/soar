# Soar HarmonyOS（骨架批，低优先级填空）

按既定口径：**只写代码；本机无 DevEco Studio / hvigor 工具链，本批未经
编译验证**（语法与工程结构按 DevEco Studio API 12 模板形态手写，评审后
在 DevEco 或后续 CI 上构建）。不卡主线。

## 当前状态（如实）

| 层 | 状态 | 说明 |
|---|---|---|
| 应用工程壳 | ⬚ 源码就绪、未编译 | `AppScope` + `entry` 模块（`EntryAbility` + `Index` 页面）。页面为 UI 骨架：品牌位、状态文案、「打开文件」动作占位。 |
| 播放内核 | ❌ 未开始 | 两条路线待评审：① 系统 `AVPlayer`（`@ohos.multimedia.media`）作后端——最快出可用播放，但解码面受系统能力约束；② `soar_core`（C++17）作 native 模块（NDK CMake + FFmpeg 交叉编译）——对齐桌面解码面，工程量大。此决策影响接口面，留用户拍板后再动。 |
| 构建验证 | ❌ 未开始 | 无工具链；后续批接入（DevEco 命令行 `hvigorw assembleHap` 或自托管 runner）。 |

## 结构

```
AppScope/app.json5                 应用级配置（bundleName/版本/图标）
entry/src/main/module.json5        模块与 ability 声明（EntryAbility，home 入口）
entry/src/main/ets/
  entryability/EntryAbility.ets    生命周期：加载 pages/Index
  pages/Index.ets                  播放页 UI 骨架（状态 + 控制按钮占位）
entry/src/main/resources/          string/color/媒体图标（1×1 中性占位）
build-profile.json5 / hvigorfile.ts / oh-package.json5   工程与模块构建
```

## 说明

- 图标为 1×1 中性占位 PNG（非 logo），文案不夸大功能。
- `Index.ets` 的按钮只有本地状态反馈，不假装播放能力；接线注释指向
  上述两条内核路线。
