# 覆盖率口径与未覆盖项清单

> 约定：本文记录 CI coverage job 的统计口径、当前水位、门禁阈值，以及**如实定性为不可达或版本依赖**的未覆盖项。目标是让“覆盖率不回退”有据可查，而不是追求字面 100%。

## 1. 统计口径

CI 的 coverage job（Ubuntu，GCC `--coverage` + gcovr）统计 `src/` + `include/`：

- gcov 原始分支记录约 30% 是 libstdc++ 内联代码的异常处理（EH）边，数字无意义，因此分支统计开 `--exclude-throw-branches` 过滤；
- 解码线程会让 gcov 的分支计数变负（GCC bug #68080），用 `--gcov-ignore-parse-errors negative_hits.warn_once_per_file` 忽略；行数据不受影响；
- 多线程计数噪声：忽略负值后个别行计数偶发归零，覆盖率对比时 ±1-2 行抖动**不是回归**。

FFmpeg 后端只在装了 libav* dev 头的环境编译，所以这个 Linux job 是 `ffmpeg_backend.cpp` 覆盖数字的唯一来源。

## 2. 当前水位与门禁

| 维度 | 实测（ac2e347） | 门禁（`--fail-under-*`） |
|---|---|---|
| 行 | 90.5%（1274/1407） | 90.1%（容 ~6 行抖动） |
| 分支 | 77.4%（943/1219） | 76.8%（容 ~7 分支抖动） |

分文件行覆盖：`ffmpeg_backend.cpp` 92%（1001/1088）、`null_backend.cpp` 100%、`player.cpp` 100%、`main.cpp` 67%（SDL 窗口交互块）。

分文件分支覆盖：`ffmpeg_backend.cpp` 80%（715/891）、`null_backend.cpp` 79%、`player.cpp` 90%、`main.cpp` 56%。

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
- **seek 内部失败的 Error 事件（732-733 行）**：需要 `avformat_seek_file` 在可 seek 的本地文件上失败——它对合法位置总是成功；不可 seek 的流在 `seek()` 更早的分支就被直接处理，走不到这里。注入自定义 AVIO 才能命中，超出测试基建范围。
- **stop 中断 read 的时序分支（39/42 行）**：需要在解码线程阻塞在读调用时精确打断，时序敏感，flake 风险大于覆盖收益。
- **main.cpp 的 SDL 窗口交互块（67% 行 / 56% 分支）**：SDL_Init 失败路径（161-163）已被坏驱动用例覆盖；剩余缺口（窗口创建、渲染循环、纹理更新）需要真实窗口系统与人工交互，无头 CI 无法覆盖。

### 3.5 测试基建教训：媒体 fixture 必须逐字节确定性

多段 h264 TS 流（分辨率/像素格式变化测试）最初用 `cat` 裸拼接字节：每段的 TS 连续性计数器在接缝处重开，demuxer 间歇性报 `Packet corrupt` 丢包——**同样的字节在同一个 CI 的不同 job 一个过一个挂**（runs 36005020299：build/asan 过、coverage 挂）。改用 concat demuxer + `-c copy` 重新封装后时间戳与计数器连续，解码全程零警告。凡 fixture 生成，交付前用 `ffmpeg -v warning -i <file> -f null -` 验到零输出为止。

测试断言也不得依赖实时解码速度：sanitizer/coverage 插桩让解码慢数倍，轮询窗口要么配 `setRate` 解除墙钟节流，要么按最慢构建留足余量。

## 4. 结论

行 90.5% 与分支 77.4% 是**当前代码库在“不删防御代码、不写假用例、不做进程污染”前提下的真实上限**。凑到字面 100% 只能：删掉防御分支、mock 掉被测库、或写不断言的假用例——三者都违背项目铁律。新增可测路径时按既有模式补测（真实文件、真实失败契约），并把门禁阈值随实测水位上抬。

可测路径的一个实用模式：**open 只为选中的流建 decoder**，因此“默认轨健康、非默认轨损坏”的容器能正常打开，把损坏的影响推到 selectTrack 时刻——双 AAC 轨只 patch 第二轨 CodecID（A_AAC→A_XXX）即可分别覆盖 open 失败与 selectTrack 失败两种契约（Error 态+事件 vs fail+旧 decoder 保留）。
