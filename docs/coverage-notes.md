# 覆盖率口径与未覆盖项清单

> 约定：本文记录 CI coverage job 的统计口径、当前水位、门禁阈值，以及**如实定性为不可达或版本依赖**的未覆盖项。目标是让“覆盖率不回退”有据可查，而不是追求字面 100%。

## 1. 统计口径

CI 的 coverage job（Ubuntu，GCC `--coverage` + gcovr）统计 `src/` + `include/`：

- gcov 原始分支记录约 30% 是 libstdc++ 内联代码的异常处理（EH）边，数字无意义，因此分支统计开 `--exclude-throw-branches` 过滤；
- 解码线程会让 gcov 的分支计数变负（GCC bug #68080），用 `--gcov-ignore-parse-errors negative_hits.warn_once_per_file` 忽略；行数据不受影响；
- 多线程计数噪声：忽略负值后个别行计数偶发归零，覆盖率对比时 ±1-2 行抖动**不是回归**。

FFmpeg 后端只在装了 libav* dev 头的环境编译，所以这个 Linux job 是 `ffmpeg_backend.cpp` 覆盖数字的唯一来源。

## 2. 当前水位与门禁

| 维度 | 实测（7d9f85a） | 门禁（`--fail-under-*`） |
|---|---|---|
| 行 | 89.8%（1264/1407） | 89.4%（容 ~6 行抖动） |
| 分支 | 75.9%（925/1219） | 75.0%（容 ~10 分支抖动） |

分文件分支覆盖：`ffmpeg_backend.cpp` 78%（702/891）、`null_backend.cpp` 79%、`player.cpp` 90%、`main.cpp` 53%（SDL 窗口交互块）。

门禁随实测水位同步上抬：测试合入、数字上涨后，把阈值抬到“实测值下方留出抖动余量”的位置，让回退被 CI 自动拦截。

## 3. 未覆盖项定性（如实记录，不写假覆盖）

### 3.1 结构性不可达（由语言/库实现决定）

- **libstdc++ SSO（短字符串优化）内联分支**：字面量短串使长串路径结构性不可达（`null_backend.cpp` 29/47、`player.cpp` 66）。
- **内联/行号归属噪声（约 50 行）**：gcov 把内联库代码的执行记到邻近源码行（函数出口 `}`、调用密集行），两侧语义已覆盖但仍报 missing。

### 3.2 防御性代码（注入即污染）

- **OOM 分支（约 41 行）**：`av_mallocz` / `av_frame_alloc` / `SDL_AllocAudioStream` 等失败路径，只能用 interpose/mock 注入失败，会污染真实库行为，与“测真实链路”矛盾。
- **SDL 音频设备打开失败（8 行）**：`ensureOpen` 把声道数钳制到 1-8（`ffmpeg_backend.cpp:147`）、频率有 `SDL_AUDIO_ALLOW_FREQUENCY_CHANGE`，合法参数下 `SDL_OpenAudioDevice` 只有在无音频设备时才失败；dummy 驱动永不失败。构造真实拒绝（如 16 声道文件）会被 clamp 化解。

### 3.3 版本依赖行为（随 FFmpeg 版本翻转，断言不钉阶段）

经验来源：runs 35941543676、35942391836 两次翻车。**凡“损坏文件在 demux/decoder 内部走哪条失败路径”的断言都是版本依赖的**，只有“open 失败 + Error 态 + 可恢复”的契约跨版本稳定。本地（FFmpeg 8）与 CI（FFmpeg 6.1）行为差异实例：

- **解码器拒收阶段**：容器声称 mpeg4、payload 是 h264 时，8 在 `receive_frame` 报错（触发 1246 行 fatal），6.1 在 `send_packet` 就拒收（走 1235/1236 行 continue，自然播到 EOF）。测试断言只要求“终结性”（Ended 或 Error，不挂死、可恢复）。
- **截断容错阶段**：512 字节截断的 mkv，8 接受 demuxer open、失败于 find_stream_info（1032 行），6.1 在 demuxer open 就拒绝。断言只钉“open 路径失败”。

### 3.4 场景不可构造（成本/稳定性不成比例）

- **`av_read_frame` 失败（1226 行）**：本地文件的 demuxer 把一切结构损坏宽容化为 EOF；网络流断开需要起本地服务并中途 kill，端口与 CI 稳定性风险不成比例。
- **stop 中断 read 的时序分支（39/42 行）**：需要在解码线程阻塞在读调用时精确打断，时序敏感，flake 风险大于覆盖收益。
- **main.cpp 的 SDL 窗口交互块（53% 覆盖）**：需要真实窗口系统与人工交互，无头 CI 无法覆盖。

## 4. 结论

行 89.8% 与分支 75.9% 是**当前代码库在“不删防御代码、不写假用例、不做进程污染”前提下的真实上限**。凑到字面 100% 只能：删掉防御分支、mock 掉被测库、或写不断言的假用例——三者都违背项目铁律。新增可测路径时按既有模式补测（真实文件、真实失败契约），并把门禁阈值随实测水位上抬。
