# 配置效果

> 播放器每个用户可见的开关：**off 帧**、**on 帧**、**切换过程动图**。本页是生成证据而非手册——
> 每一帧都由驱动成品二进制在独占 Xvfb 显示上采集。

| | |
| --- | --- |
| **夹具** | `scripts/generate_test_media.sh` 媒体：160×120 `testsrc` 图样 + 正弦音 + 三 cue 的 srt（`Hello` / `World` / `Two Lines`）。画面上的大数字是图样自带的计数器，属视频内容而非字幕。 |
| **被测构建** | `build-system/soar`（IMGUI 开），`SOAR_UI_BITMAP_FONT=1`——走查钉定配置 |
| **窗口** | 960×540，SDL 居中于 1280×800 `Xvfb`；静帧裁到窗口 |
| **流水线** | `scripts/gen_config_gifs.sh [toggle \| all]`——可重跑，配置变更即全量重生成，兼作配置效果回归检查（每个 on/off 对必须仍有差异）。 |

## 清单

| # | 开关 | 位置 | Off | On | 动图 |
| --- | --- | --- | --- | --- | --- |
| 1 | 外观（暗 ↔ 亮） | `y`，设置 → 外观 | [帧](/config-effects/theme_off.png) | [帧](/config-effects/theme_on.png) | [动图](/config-effects/theme.gif) |
| 2 | 字幕可见 | `v`，OSC Sub 组合 | [帧](/config-effects/subtitles_off.png) | [帧](/config-effects/subtitles_on.png) | [动图](/config-effects/subtitles.gif) |
| 3 | 静音 | `m`，OSC | [帧](/config-effects/mute_off.png) | [帧](/config-effects/mute_on.png) | [动图](/config-effects/mute.gif) |
| 4 | 播放速度 | `.`，`,`，设置 → 播放 | [帧](/config-effects/rate_off.png) | [帧](/config-effects/rate_on.png) | [动图](/config-effects/rate.gif) |
| 5 | 全屏 | `f` | [帧](/config-effects/fullscreen_off.png) | [帧](/config-effects/fullscreen_on.png) | [动图](/config-effects/fullscreen.gif) |
| 6 | 记忆上次媒体 | 设置 → 通用 | [帧](/config-effects/switch_remember_off.png) | [帧](/config-effects/switch_remember_on.png) | [动图](/config-effects/switch_remember.gif) |
| 7 | ESC 退出全屏 | 设置 → 通用 | [帧](/config-effects/switch_esc_off.png) | [帧](/config-effects/switch_esc_on.png) | [动图](/config-effects/switch_esc.gif) |
| 8 | 随机队列 | 设置 → 播放 | [帧](/config-effects/switch_shuffle_off.png) | [帧](/config-effects/switch_shuffle_on.png) | [动图](/config-effects/switch_shuffle.gif) |
| 9 | 显示字幕 | 设置 → 字幕 | [帧](/config-effects/switch_subs_show_off.png) | [帧](/config-effects/switch_subs_show_on.png) | [动图](/config-effects/switch_subs_show.gif) |
| 10 | 自动显示字幕 | 设置 → 字幕 | [帧](/config-effects/switch_subs_autoshow_off.png) | [帧](/config-effects/switch_subs_autoshow_on.png) | [动图](/config-effects/switch_subs_autoshow.gif) |
| 11 | 自动下载字幕 | 设置 → 字幕 | [帧](/config-effects/switch_subs_autodl_off.png) | [帧](/config-effects/switch_subs_autodl_on.png) | [动图](/config-effects/switch_subs_autodl.gif) |
| 12 | `--headless` | CLI 参数 | [帧](/config-effects/cli_headless_off.png) | [帧](/config-effects/cli_headless_on.png) | [动图](/config-effects/cli_headless.gif) |
| 13 | `--no-shortcuts` | CLI 参数 | [帧](/config-effects/cli_no_shortcuts_off.png) | [帧](/config-effects/cli_no_shortcuts_on.png) | [动图](/config-effects/cli_no_shortcuts.gif) |

有可见效果但本流水线未采集的参数列在[文末](#无动图的参数)并注明理由。

## 外观

`y` 切换主题；设置 → 外观同效。外壳（状态条、OSC、设置面）换色；视频画布与其上字幕属内容层，两主题皆保持暗底。

| Off（暗） | On（亮） | 动图 |
| --- | --- | --- |
| ![](/config-effects/theme_off.png) | ![](/config-effects/theme_on.png) | ![](/config-effects/theme.gif) |

## 字幕可见

`v` 切换所选字幕轨的字幕。读帧前需知两条夹具行为：

- 夹具的 srt **不自动选中**——打开事件记 `selected(video=0,audio=1,sub=-1)`——故流水线先从 OSC 字幕组合选轨；
- 字幕只在**播放中的帧**绘制。暂停帧会让渲染器提前退出，所以流水线在播放中等待 cue 而非暂停在 cue 上。

流水线将媒体降到 0.5×（`,` 两次），使四秒 cue（`Two Lines`，媒体时 4–6 s）足够长，能容纳两张静帧而六秒短片不至播完。

| Off（字幕隐） | On（字幕显） | 动图 |
| --- | --- | --- |
| ![](/config-effects/subtitles_off.png) | ![](/config-effects/subtitles_on.png) | ![](/config-effects/subtitles.gif) |

## 静音

`m` 切换静音。静音时芯片显示 `Unmute`，音量条降到零。

| Off（未静音） | On（静音） | 动图 |
| --- | --- | --- |
| ![](/config-effects/mute_off.png) | ![](/config-effects/mute_on.png) | ![](/config-effects/mute.gif) |

## 播放速度

`.`，`,` 在六个档位（0.5×、0.75×、1.0×、1.25×、1.5×、2.0×）间步进；设置 → 播放循环同列表。此处芯片 1× → 1.25×；尾部 `x` 被组合宽度裁掉——即 2026-11-21 UI 审核记的 P3 外观项。

| Off（1×） | On（1.25×） | 动图 |
| --- | --- | --- |
| ![](/config-effects/rate_off.png) | ![](/config-effects/rate_on.png) | ![](/config-effects/rate.gif) |

## 全屏

`f` 进全屏；全屏下 `Escape` 回窗口（ESC 退出全屏开关关掉时则退出程序）。

| Off（窗口） | On（全屏） | 动图 |
| --- | --- | --- |
| ![](/config-effects/fullscreen_off.png) | ![](/config-effects/fullscreen_on.png) | ![](/config-effects/fullscreen.gif) |

## 设置 → 通用

两个通用开关改的是行为而非画面，故可见效果即药丸本身。

**记忆上次媒体**——启动时续播上次位置。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_remember_off.png) | ![](/config-effects/switch_remember_on.png) | ![](/config-effects/switch_remember.gif) |

**ESC 退出全屏**——行上文案即快捷键实义：全屏下 ESC 回窗口；否则 ESC 退出程序。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_esc_off.png) | ![](/config-effects/switch_esc_on.png) | ![](/config-effects/switch_esc.gif) |

## 设置 → 播放

**随机队列**——重排队列而非顺序播放。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_shuffle_off.png) | ![](/config-effects/switch_shuffle_on.png) | ![](/config-effects/switch_shuffle.gif) |

## 设置 → 字幕

三个开关，第二行的可见性与 `v` 同。

**显示字幕**——关闭时渲染器提前退出。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_subs_show_off.png) | ![](/config-effects/switch_subs_show_on.png) | ![](/config-effects/switch_subs_show.gif) |

**自动显示字幕**——打开时自动选中并显示字幕轨。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_subs_autoshow_off.png) | ![](/config-effects/switch_subs_autoshow_on.png) | ![](/config-effects/switch_subs_autoshow.gif) |

**自动下载字幕**——文件打开时从字幕端点取 sidecar 字幕。

| Off | On | 动图 |
| --- | --- | --- |
| ![](/config-effects/switch_subs_autodl_off.png) | ![](/config-effects/switch_subs_autodl_on.png) | ![](/config-effects/switch_subs_autodl.gif) |

## CLI 参数

**`--headless`**——无窗口运行。on 帧刻意是空桌面裁剪：没有窗口出现。

| Off（有窗） | On（无窗） | 动图 |
| --- | --- | --- |
| ![](/config-effects/cli_headless_off.png) | ![](/config-effects/cli_headless_on.png) | ![](/config-effects/cli_headless.gif) |

**`--no-shortcuts`**——整个键盘层关闭。两张静帧是同一次 `m` 按下、带与不带该参数：普通 run 芯片读 `Unmute`，带参数保持 `Mute`。两次 run 皆暂停在 ~1 s 使控件后帧面几乎一致；此对为两次 run 对比，本页其余每个动图即该参数的「off 态」。

| Off（参数关：`m` 静音） | On（参数开：`m` 无效） | 动图 |
| --- | --- | --- |
| ![](/config-effects/cli_no_shortcuts_off.png) | ![](/config-effects/cli_no_shortcuts_on.png) | ![](/config-effects/cli_no_shortcuts.gif) |

## 无动图的参数

| 参数 | 为何无动图 |
| --- | --- |
| `--cast-transcode` | 其效果是*投屏中*的转码路径；未接接收端前本地窗口无变化，cast 探针是更重的独立夹具。 |
| `--open-dialog` | 唤起系统文件选择器，属原生对话框面，本应用内流水线不驱动。 |
| `--no-resume` | 需先种入续播位置，且其唯一可见效果是起始位置时钟而非窗口状态。 |
| `--gui` | 无媒体源启动时强制开窗；默认启动本就开窗，故无独立 on 态。 |

`--help`、`--version`、`--torrent-list` 打印到终端后退出；无窗口状态可框。