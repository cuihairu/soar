---
layout: home

hero:
  name: Soar
  text: 万般格式，任其翱翔
  tagline: 用 C++23 从零打造的万能格式开源播放器 —— 一套干净的核心播放抽象，配上 FFmpeg 后端
  image:
    src: /logo.svg
    alt: Soar
  actions:
    - theme: brand
      text: 构建指南
      link: /build
    - theme: alt
      text: 项目边界与路线图
      link: /mvp
    - theme: alt
      text: GitHub
      link: https://github.com/cuihairu/soar

features:
  - icon: 🧩
    title: 核心只有一份
    details: 播放 API 与事件模型不依赖任何具体多媒体库，UI 与播放内核解耦。
  - icon: 🔌
    title: 后端可插拔
    details: FFmpeg、系统框架、还是测试用的假后端，都实现同一个 IBackend 接口。
  - icon: 📄
    title: 合规先行
    details: 项目本体保持 Apache-2.0，依赖选型守住 LGPL/GPL 边界。
  - icon: ⚡
    title: 测试替身内置
    details: NullBackend 用纯状态机模拟完整播放行为，核心 API 的测试可在任何 CI 环境运行。
---
