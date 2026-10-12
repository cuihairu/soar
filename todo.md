# todo

## 投屏接线批·二期（2026-10-12，「一期」范围里明确排除的产品入口接线兑现）

范围：窗口投屏浮层 + CLI `--cast`。一期组件（`src/cast/`）不动，桥改绑/
恢复语义不动；pause 动作、GENA、发送端转码仍不在本批。

- [x] **快捷键 + 入口**：`ShortcutAction::Cast`（默认 `t`，进 defaults 表
      /名字 round-trip/查表三处既有用例矩阵）。
- [x] **窗口投屏浮层**（`player_window.cpp` cast 区，~180 行）：`t` 开合
      520x420 居中浮层——无源说明、未 serving 提示、会话相位渲染
      （Casting 带位置读出 `on_position` 原子回调 / Failed / Stopped）、
      Stop casting / Search again 按钮（SSDP 3s 突发一次性防重入，后台
      线程发现 + 互斥结果表，完成线程回收）、renderer 行 Selectable
      （AllowOverlap 修 ImGui 全宽吞 SameLine 点击）、toast 反馈
      （成功/失败/无应答/已停）。
- [x] **CLI `--cast=<descURL|auto>`**：headless 直接驱动 CastSession
      （失败 exit 1，错误串已在组件层带「cast: 」前缀时不重复加）；窗口
      模式预置 arm，等桥 Serving 后自启——direct 直连 / auto 一次 3s 发
      现突发→首个应答者或「No DLNA renderer answered」toast；`--cast`
      无 torrent 源在开任何 socket 前拒绝。
- [x] **测试面**：headless 4 用例 29 断言（python 假 DLNA renderer 夹具
      ok/noav/silent 三模式 + 拒连端口；断言 SetURI 收到改写后的新桥口
      URL、renderer 实拉桥 URL 206、Play/Stop 全握手、无 AVTransport/
      拒连/无源拒绝矩阵）+ X11 六发走查 27 断言（直连全握手 + 搜索三连
      点覆盖防重入/线程回收/Stop 按钮、auto 突发、404 描述失败相位、
      静默 renderer 失联相位、无源文案、无 info-hash magnet 等 serving
      挂起 + Escape 干净退出）；非 X11 与 no-imgui 树 MESSAGE 跳过。
- [x] **顺手修一处真实数据竞态**：`FFmpegBackend::open()` 对 `hwdec_`
      的解锁写撞 `close()` 锁内 `clear()`（tsan-o0 带夹具确定性命中，
      -O1 时序躲开）——arm 移入 decode_mutex_ 锁内（arm 先于
      setupDecoders 的既序不变），三连跑绿。
- [x] **门禁**：六树 13/13 全绿（system / tsan -O1 / tsan -O0 / asan 均
      无 X11 对齐 CI，no-imgui + cov3 带 X11），gcovr 行 **95.3%
      （8017/8411）**、支 **85.2%（8660/10168）**，对 95.0/84.6 余量
      ~28 行/~59 支（> 抖动带）。

### 如实说明

- 覆盖诚实账：cast 区域可测 172/191 行，余 16 行三类定性——SSDP responder
  需要组播应答，走查环境脆弱不上（组件层 test_cast.cpp 已盖发现与 SOAP）；
  startCastTo 的行点击守卫两行（「未 serving」toast / already-casting）
  只在点中已发现 renderer 行时可达，同需 responder；Discovering/Idle 相
  位文案四行结构不可达——`start()` 同步跑在 UI 线程，`cast_active_` 只在
  Play 成功后才赋值，浮层永远画不到这两个相位（防御性 switch 臂）。
- X11 走查依赖 python3-xlib + 私有 Xvfb（`70+pid%25`），按钮坐标按
  SOAR_UI_BITMAP_FONT=1 探测（缩放 TTF 会 reflow 浮层）。
- **真 SSDP responder 的端到端未验证**（无真实设备、本机组播环境脆弱）：
  窗口/CLI 产品面发现走「无应答」分支验证，发现→SOAP 的真链路由组件层
  用例覆盖；接真实电视前的设备矩阵仍是挂账。
- gcovr 门禁 filter 两坑：`--filter` 必须用目录形（`src`/`include`，CI
  同款）——手写正则 `.*/include/.*` 会把 `third_party/libtorrent/include`
  卷进来（分母 8411→66146 的假读数，45% 假红）；系统头 `no_working_dir_
  found` 噪声用 `--gcov-ignore-errors=no_working_dir_found` 吸收。
- 二 commit：c1a75b5（hwdec 竞态修）/ 61ab95d（接线 + 测试）。

## 投屏实现批·一期（2026-10-11 点单开工，「投屏协议选型」节末尾实现批要点兑现）

范围（照要点，不得扩面）：SSDP 发现 + 设备描述 XML 解析 + SOAP 控制
（SetAVTransportURI/Play/Seek/Stop）自实现组件（控制点，零新第三方依赖；
GENA 不做改轮询 GetPositionInfo）、桥会话期 LAN 改绑/恢复、接收端不支持
格式显式报错（不做发送端转码）、纯逻辑单测 + 内嵌最小 MediaRenderer 冒烟。
不在本批：窗口/CLI 产品入口接线（另立批）、pause 动作、renderer/DMS 角色、
GENA 事件、转码。

- [x] **C1 纯逻辑组件**：`src/cast/ssdp.{h,cpp}`（M-SEARCH 构造 golden+超时
      钳制、应答解析——ST/USN/LOCATION 三查、NOTIFY 拒收、大小写/裸 LF 容
      错）、`src/cast/device_xml.{h,cpp}`（设备描述解析含实体解码、相对
      controlURL 按 RFC3986 引用合并、URL 拆分含 IPv6 括号形、AVTransport
      解析含空 controlURL 跳过取下一个）、`src/cast/soap.{h,cpp}`（四动作
      请求构造 golden——Content-Length 与报体互证、DIDL 线上双重转义、
      fault/GetPositionInfo 解析拒空指针出参、时钟互转 H:MM:SS[.毫秒]、
      HTTP 状态提取）。
- [x] **C2 桥 LAN 改绑**：`TorrentStream::exposeToLan(peer_host)` /
      `restoreLoopback()`——换代监听 socket（新临时端口，旧已受理连接不受
      扰：retire 旗标+先退旧线程再起新线程无窗口）、playbackUrl() 重读生
      效；默认恒 127.0.0.1，会话结束/失败即恢复；`localIpForPeer` UDP
      connect 探路。用例进 test_p2p.cpp：暴露前 LAN 拒连→暴露后 LAN
      GET 206/旧回环口拒连→恢复后回环 206/LAN 拒连→stop 后两端口均
      「not serving」；错误路径（未 serving、无路由本机地址）。
- [x] **C3 会话组件**：`src/cast/renderer.{h,cpp}`（SSDP 组播发现、描述
      拉取、SOAP POST、非阻塞 connect 超时——握手后必须恢复阻塞模式，
      否则 SO_RCVTIMEO 不生效 recv 恒 EAGAIN，实测踩坑）+
      `src/cast/cast_session.{h,cpp}`（发现→改绑→SetURI→Play→
      GetPositionInfo 轮询→Stop→恢复；启动宽限期内位置不前进=显式报错
      「接收端多半解不了该源，本构建无发送端转码」；NOT_IMPLEMENTED 时钟
      =「未知」不判卡死；SOAP fault 原文透出；连续轮询失联显式失败；
      stop 幂等，seek 非致命）。
- [x] **C4 测试面 + 门禁收编 + 文档**：`tests/test_cast.cpp` 28 用例/431
      断言（纯逻辑全平台 + 内嵌最小 MediaRenderer 进程内冒烟 POSIX-only，
      同 p2p 套件口径：发现→起会话全驱动、fault 三动作、stall/deaf/
      no-clock、传输边角 spill/no-length/flood-head/not-http/巨长度、
      noav/坏控制地址/404 轮询/快速失败矩阵）、`soar_cast_tests` 目标、
      桥改绑用例进 test_p2p.cpp、fullgate 绿（13/13 套件；行
      95.2%=7837/8228、支 84.9%=8448/9947，对门禁 95.0/84.6 真实余量
      ~20 行/~32 支，按原始分子分母核；cast 28 用例本地全树绿（Debug/
      tsan/asan/no-imgui，p2p 20 用例同口径））、mvp.md §3/README v1.0
      行/todo 落账。十一 commit d5c4631（桥改绑）/1059876（cast 组件+
      测试+文档）/01e3713（tsan 修）/78a6c5d（asan 字面量长度修）/
      2700b34（MSVC min/max 括号+LAN 探路走默认路由）/868405d（macOS
      port-0 探路修）/19f9569（空主机名拒走 resolver+活组播用例
      apple 门控）/5abad61（数值优先探路+发现 socket 非阻塞）/
      dcfb886（排障面包屑一轮定位）/3c9bc73/ded50fd（Darwin 监听
      socket 关停序双修）已 push，CI run 38184694410 七腿全绿
      （build×3/tsan/asan/coverage/no-imgui）。

### 如实说明

- src/cast 的 socket 层按 p2p 先例自含 winsock 包装（Windows 构建腿要
  编译通过——6 个源文件本地 mingw -fsyntax-only 干净），但**活 socket
  冒烟用例按 p2p 套件口径 POSIX-only**——Windows 上组播/绑卡行为未在本
  批验证，纯逻辑用例全平台跑；test_cast.cpp 的 Windows 分支本地无法
  mingw 编验（系统 doctest 头拉 glibc），CI MSVC 腿已核绿。
- macOS CI 连红收口成一条平台教训（runs 38170618582→38182398534 面包
  屑一轮定位）：**Darwin 对监听 socket 的 shutdown() 是 no-op**，唤醒
  阻塞 accept 只能靠 close()——产品 `rebindListener`（3c9bc73）与测试
  FakeRenderer 析构（ded50fd）同踩，两处都改成与 stop() 同款的
  shutdown→close→join 序。配套三个 macOS 环境差一并落地：探路 UDP
  connect 要真实端口（port 0 在 macOS 恒报 0.0.0.0）、localIpForPeer
  数值优先（macOS 对空/坏主机名 getaddrinfo 走搜索域挂起，IP 一律先
  inet_pton）、发现 socket 全程非阻塞 poll 驱动。两个活组播发现用例
  `!defined(__APPLE__)` 门控维持（macOS CI VM 对 lo0 组播 M-SEARCH 不
  应答），纯逻辑+直连定位的会话用例全平台跑。
- 覆盖率口径：gcov 把 winsock #ifdef 分支计为不可达死支（p2p 同负担），
  门禁余量按 gcovr 门禁同旗标（--exclude-throw-branches 等）原始分子
  分母核，未用聚合法。
- 产品入口（窗口投屏浮层/CLI `--cast`）不在本批：组件经测试直接驱动，
  「接收端不支持格式显式报错」以组件错误串落地，用户可见提示随接线批。
- 本地桥改绑的 LAN 暴露窗只在会话期（SetAVTransportURI 前、Stop/失败后
  即回 127.0.0.1）；「接收端自解码」的行为判定是轮询位置宽限窗不动即显
  式报错，真实接收端解码能力矩阵未在本批验证（无真实设备）。

## 投屏协议选型（2026-10-11 拍板收口，本批只出结论零实现码）

挂账②收口。范围：四候选（AirPlay / Chromecast / DLNA/UPnP AV / Miracast）×
五维对比 + 单一推荐；实现另立批（要点见本节末尾）。

- [x] **结论：DLNA/UPnP AV 先行，soar 作控制点驱动接收端 MediaRenderer**

| 维度 | AirPlay | Chromecast (CASTV2) | **DLNA/UPnP AV（选它）** | Miracast |
|---|---|---|---|---|
| 协议开放度 | 封闭：Apple 私有无官方规范，现存实现全靠反向工程（shairport-sync/pyatv），协议随系统版本漂移；AirPlay 2 配对加密（SRP6a/Ed25519/Curve25519）+ MFi 认证，专利/授权灰区 | 半开放：CASTV2 帧格式与 protobuf schema 公开流传、Google 不阻第三方发送端（pychromecast/go-chromecast 长期存在）；媒体通道只支持 URL（接收端自行拉流），行为随 Google 接收器固件变 | **完全开放**：UPnP DeviceArchitecture + AVTransport 公开免费标准（SSDP 组播 + HTTP 拉 XML 设备描述 + SOAP 控制动作 SetAVTransportURI/Play/Seek/Stop），无认证授权要求；DLNA 组织 2017 解散只停了认证，规范与设备实现仍在 | 标准需 Wi-Fi Alliance 成员资格获取；Linux 侧实现（miraclecast）长期不成熟停摆 |
| 接收端生态 | Apple 硬件 + 近年智能电视内置 AirPlay 2（厂商获 Apple 授权）；第三方接收端要么 MFi 要么反向工程 | 大：Chromecast/Google TV 硬件 + 内置 built-in 的电视；2024 起 Chromecast 产品线停售（换 Google TV Streamer），但 built-in 仍在电视里普遍存在 | **最大**：2010 年代以来几乎所有智能电视/机顶盒/游戏机/AV 功放出厂内置 renderer，2026 实核 Samsung Tizen / LG webOS 新机仍随附（如实：属 legacy 功能，厂商设置埋深、个别新款可能移除——这是 Chromecast 留作第二候选的原因） | 智能电视 Screen Share 普遍存在，但那是屏幕镜像不是媒体投屏 |
| 发送端覆盖（iOS/Android/桌面） | iOS/macOS 原生最好；Android/Windows/Linux 无官方发送端 | Android/Chrome 原生；桌面第三方发送端库成熟 | **全平台等价**：纯网络协议无平台 API 依赖，桌面三平台同一套代码；iOS/Android 现成控制点 app（BubbleUPnP 等）佐证该形态 | Windows/Android 原生；macOS/Linux 基本不可行 |
| 实现复杂度 | 高：mDNS + 反向工程报文 + 配对加密，镜像流与 DRM 内容另有 FairPlay 约束 | 中：mDNS 发现 + TLS + protobuf CASTV2 + LOAD 消息；需引入 TLS 与 protobuf（wire format 字段少可手写） | **最低**：几条 SSDP 报文 + HTTP 拉 XML + SOAP over HTTP POST，纯 socket 手写零新依赖；GENA 事件订阅可选（第一期轮询 GetPositionInfo 即可） | 极高且错类：Wi-Fi Direct 链路 + 实时 H.264 屏幕编码 + HDCP，是镜像管线不是 URL 投屏 |
| soar 栈契合度（SDL/libtorrent/本地桥） | 差：加解密/授权栈全新，URL 模式受 FairPlay 与格式限制纠缠 | 好：URL-cast 模型与本地 HTTP 桥同构，但 TLS/protobuf 是新依赖 | **最好**：renderer 自带 Range 地来 GET 桥 URL——P4 的 `TorrentStream http://127.0.0.1:<port>/` 投屏会话期改绑 LAN 口即成（Range 三形态 206/416 已有），控制面纯 UDP/HTTP 与 p2p/http 同栈零新依赖 | 差：Wi-Fi Direct 管理在 soar 桌面栈中完全不存在 |

### 依据（为何 DLNA 先行）

1. 完全开放标准，零授权/加密/专利风险——AirPlay 的反向工程+专利灰区、
   Chromecast 的私有接收器行为不进第一批。
2. 接收端存量最大：几乎全部智能电视/机顶盒/游戏机出厂即有 renderer，
   接收端无需用户装任何 app。
3. 实现复杂度最低且零新第三方依赖：SSDP+XML+SOAP 自实现，符合仓库
   「核心抽象不动、最小依赖」口径（P4 决策先例）。
4. 栈契合最好：「接收端来拉 URL」与既有本地 HTTP 桥是同一原语——桥
   会话期改绑 LAN 口即可，播放链路零改动。

次选与排除：Chromecast 留作第二批候选（URL-cast 与桥同构，增量成本在
TLS+protobuf 与接收器格式白名单）；AirPlay 排除（授权/配对加密/反向
工程漂移风险，收益不抵）；Miracast 排除（屏幕镜像非媒体投屏，类别
不符）。另如实记录：FUTO 的 FCast 是新兴完全开放协议（零授权、本地
直连、自定义接收端），但接收端生态目前过小（主要 FUTO 自家 receiver
与 GrayJay），不作首选，留意其生态增长。

### 实现批要点（另立批，本批零实现码）

- 安全边界：桥默认仍 127.0.0.1-only，仅投屏会话期临时绑局域网口，
  会话结束即恢复——LAN 暴露窗口最小化。
- 接收端自解码：不支持的容器/编码显式报错提示，第一实现批不做发送端
  转码（FFmpeg 转码留作后续可选）。
- 组件自实现 SSDP/SOAP/XML，零新第三方依赖；soar 只做控制点，不做
  renderer/DMS 角色；GENA 事件订阅不做（第一期轮询）。
- 测试面：纯逻辑单测（SSDP 报文构造/解析、SOAP 请求、设备 XML 解析）
  + 测试内嵌最小 MediaRenderer 冒烟（同 P2 内嵌 RTSP 服务器先例）。

## 快捷键可配置批（2026-10-11 登记，ui-design.md §3.2「可配置留待后续」收口）

范围：键位映射组件（默认表 + 配置文件重绑 + 全局开关）、窗口接线、帮助浮层
实时键位列、CLI 开关、X11 走查用例。不在本批：设置面板 GUI（仍不做，手工
编辑配置文件即本批交付形态）、按键录制器。

- [x] **S1 键位映射组件**：`src/app/ui/shortcuts.{h,cpp}`（soar_app_ui 目标，
      纯逻辑无 SDL，同 ui_state/dir_scan/library 契约；自带 `kKey*` 键码表，
      player_window.cpp 71 条 `static_assert` 对 SDL 逐值钉死——漂移编译期
      失败）。`ShortcutMap`：出厂默认表（即 §3.2 表）、行式配置重绑（行替换
      动作默认绑定、单字符区间、shift/ctrl/alt 前缀、空值解绑、`shortcuts
      = off` 全局开关）、查找规则（无修饰绑定无视修饰键=复刻原 switch；带
      修饰绑定同键优先；同键两行后行胜出=后面的行拿走键）、typo 行跳过
      不解绑。测试 `tests/test_shortcuts.cpp`（16 用例）+ `soar_shortcut_tests`。
- [x] **S2 窗口接线**：`handleEvent` 硬编码 switch 换 `bindingFor()` 查表
      （34 动作全覆盖，行为逐键等价——percent_seek 用命中序号×10%，自定义
      键集按序映射）；帮助浮层键位列改由当前绑定实时渲染（默认配置下与
      原静态表逐字节一致，X11 标签哈希用例不受扰）；`--shortcuts=FILE` /
      `--no-shortcuts` CLI 开关（后者优先级高于文件内 `shortcuts = on`）。
- [x] **S3 X11 走查用例**：注入脚本加 `rebind` 模式（旧键 4 连按窗口必
      留存 → rc=7；新键风暴 20s 内退出 → rc=0），用例
      「windowed run honors a rebound quit key from the shortcuts config」
      （`quit = x`：q 留存、x 退出；SDL_CreateRenderer 失败环境跳过）。
      本地 build-cov3 实跑 7 个无后端 X11 用例全绿（含 HUD 走查/库浮层/
      续播）。排障插曲：手工探针曾误用 build-system 树的二进制——该树
      `SOAR_ENABLE_IMGUI=OFF`（无 ImGui 的裸回退循环只硬编码 Escape），
      字母键无反应是回退路径使然而非映射缺陷；套件树 build-cov3
      IMGUI=ON，用例真跑即过。
- [x] **S4 全量门禁 + 提交推送**：本地 fullgate 绿（12/12 套件；行
      95.3%=7056/7406、支 85.0%=7530/8861，对门禁 95.0/84.6 的真实余量
      ~20 行/~34 支，按原始分子分母核）；三 commit fc89dc0（组件）/
      dfac800（接线+CLI+文档）/76c637d（rebind 用例补 no-imgui 门控）已
      push，CI run 38134638705 七腿全绿（build×3/tsan/asan/coverage/
      no-imgui）。如实记录：dfac800 那轮 no-imgui 腿红——rebind 用例没
      门控裸回退构建（该循环不走映射表，字母键按设计无效），本地双树
      （build-cov3 实跑 + build-noimgui 跳过）验证后 76c637d 补
      `SOAR_CLI_HAS_IMGUI` 门控转绿。

### 如实说明

- 默认行为零变化是验收线：出厂默认表逐键复刻原硬编码 switch（含
  shift+q 退出、shift+← 1s、ctrl+o 文件夹），帮助浮层默认文案逐字节不变。
- 键码表两处错值（DELETE 127 ASCII 段、MENU 0x40000076）由 S2 的
  static_assert 网络编译期坐实后修正——钉死网络正是为这类漂移而设。
- 重绑只接管应用级快捷键：浮层内部的按键（如播放队列浮层的 `x` 删行）
  不走映射表，把某个动作绑到那个键会两处都触发——属用户自选冲突，与
  设置面板的按键录制器同类问题，本批不做消解。

## 媒体库批（2026-10-10 登记，mvp.md §3「媒体库与刮削、历史/同步、多端一致性」）

范围：媒体库核心（监视文件夹 + 扫描 + 持久化）、历史/断点续播、窗口库浮层。
不在本批：HDR/真机硬解（卡硬件）、投屏、插件体系、ASR 语音翻译（用户令排除）；
网络刮削（TMDB 等外部服务）与多端同步（需传输协议决策）留后续批，本批刮削
只覆盖离线 NFO 边车（M4，视余量）。

- [x] **M1 库组件**：`src/app/ui/library.{h,cpp}`（soar_app_ui 目标，纯逻辑无
      SDL/ImGui，同 ui_state/dir_scan 契约）——`MediaLibrary`：监视文件夹
      增删、递归扫描（复用 `listMediaFiles` + stat，合并/剪枝语义）、
      历史 API（recordOpen/recordProgress/clearPosition/removeEntry）、
      原子持久化（tmp+rename，行格式，容错解析）、`defaultLibraryPath()`
      （XDG state 同 recent.txt）、`resumePositionMs()` 纯策略函数。
      测试 `tests/test_library.cpp` + `soar_library_tests` 目标。
- [x] **M2 窗口历史接线**：`WindowUiConfig::library` 指针（main.cpp 持有实例，
      headless 不碰状态——沿用 RecentStore 既有契约）；构造器 load + 初始源
      断点续播 seek；recordOpen 接线；播放中 5s 节流 recordProgress +
      Ended 清位；析构器最终记录+保存；openSource/openPlaylistEntry 续播
      seek+toast；torrent 桥 URL 跳过历史；`--no-resume` CLI 开关。
      脚本化窗口用例：预置 library.txt 断言续播位置事件。
- [x] **M3 库浮层 UI**：`b` 键 Overlay::Library——监视文件夹区（增：复用
      DirPick「监视此文件夹」模式；删）、Rescan、条目列表（名/时长/续播
      标记）、点击即播（带续播 seek）；帮助浮层补 `b` 行；ui-design.md
      §1.6 键位表补 `B 媒体库`。脚本化用例：浮层开+条目点击+监视文件夹流。
- [x] **M4（视余量）离线 NFO 刮削**：`<名>.nfo` 边车 title/plot 解析
      （扫描时读取，内存缓存不落持久化——边车文件是唯一真相），库浮层行
      标题优先显示刮削名、悬停提示带 plot。网络刮削与多端同步另立批。

### 如实说明（本批边界）

- 历史记录只走窗口会话（headless 保持无状态，与 RecentStore 既有契约一致；
  所有窗口用例已按 XDG_STATE_HOME 隔离，库文件同路径同隔离）。
- 续播策略：position ≥ 5s 且 duration 已知且 position ≤ duration−5s 才续播；
  Ended 清位（看完即忘）。torrent 桥 URL 每会话换端口，不进历史。
- 扫描语义：递归；条目按路径排序；监视文件夹内消失的文件被剪枝，非监视
  路径（直接打开的 URL/文件）的条目永不被扫描剪枝。
- 存储键分隔符一律归一为 '/'（Windows 折叠 '\'，POSIX 恒等——'\' 在
  POSIX 可以是文件名的一部分）：argv/对话框/扫描/手工编辑四种来源同形，
  同一文件不因写法差异裂成两行历史（2026-10-11 收口，Windows CI 实红
  test_library 分隔符断言坐实的库内缺口，非仅断言放宽）。

## 安装包格式补全（2026-10-04 登记，本批收口）

用户需求：soar 安装包缺格式——

- [x] ① Linux 补 **.deb 和 .rpm**（同二进制一起进 nightly，元数据/
      图标/.desktop 齐）——`scripts/package-linux-pkg.sh` 从同源 zip
      二次打包（单一事实源），/opt/soar 布局 + /usr/bin/soar +
      .desktop + hicolor 图标；CI 真装走查（dpkg -i → --version →
      dpkg -r；rpm -ivh → --version → rpm -e）
- [x] ② Windows 补**安装包 setup.exe**（Inno Setup，参照
      falcon/memex 形态：装目录/开始菜单/桌面图标/卸载器，窗口不带
      终端黑框——快捷方式指向 GUI 子系统的 soarw.exe），
      `packaging/windows/soar.iss`；zip 照旧保留
- [x] ③ 全部进滚动 nightly Release + SHA256 清单
      （SHA256SUMS.txt 随 Release 发布）
- [x] ④ Linux 补 **AppImage**（上批如实回报的缺口，2026-10-10 单开
      一批收口）——`scripts/package-linux-appimage.sh` 从同源 zip
      二次打包（单一事实源，AppDir 根 = zip 内容原样，RUNPATH 零
      改写），AppImageKit continuous runtime 前置拼接 + squashfs
      （gzip，最大兼容面）；CI 走查三启动面：FUSE2 直跑（
      libfuse2t64）+ APPIMAGE_EXTRACT_AND_RUN + --appimage-extract，
      另进裸容器冒烟（无 FUSE，验证包自含 runtime+载荷）；
      x64/arm64 双架构进 nightly 与 SHA256 清单
- [x] 并案收口：安装包里带上依赖 DLL（闭包捆绑，装完就能跑——
      缺 DLL 缺陷见 BUGS.md #1，黑框闪退见 BUGS.md #2）

### 如实说明

- ①②③ 交付时 nightly 实际只有 zip（AppImage 从未打过），缺口如实
  回报未擅自扩面；④ AppImage 于 2026-10-10 按上批约定单开一批补齐。
- Linux deb/rpm 不声明 Depends（运行库随包在 /opt/soar/lib，自包含
  口径与 zip 一致；目标机只需 glibc/libstdc++ 底线，同包内
  PLATFORM-NOTES.txt）。AppImage 载荷与 zip 同源，底线相同；AppImage
  直跑需系统 FUSE2（libfuse.so.2，缺则走 APPIMAGE_EXTRACT_AND_RUN
  免安装运行面）。

## 挂账项拍板（2026-10-10）

- **Welcome 页 / DPI 三档 / 亮色四页复审：不追（关闭）**。DPI 125%/150%
  需真机（runner 桌面仅 100%，RDP/多会话接管风险评估后已放弃盲写）；
  Welcome 页为 Inno 内置英文页（限制已登记，自定义文案仅 Tasks/Run
  两短行已实现）；亮色四页 v2.1 已随 e5f9bf6 发货并有截图证据链，
  无新重设计待复审。
- **鸿蒙内核路线：维持留用户拍板**（055ecdb 记录 + README 登记 + 一律
  后置令继续生效），本轮不改码。
- BUGS #3（Windows 真机复测）/ BUGS #4（watchdog 挂账，根因位
  player_window.cpp 退出路径 stop/join 次序）维持挂账，留待专门批。
