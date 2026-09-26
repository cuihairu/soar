---
layout: home

hero:
  name: Soar
  text: 万般格式，任其翱翔
  tagline: 用 C++17 从零打造的万能格式开源播放器 —— 一套干净的核心播放抽象，配上 FFmpeg 后端
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
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="3" width="8" height="8" rx="1.5"/><rect x="13" y="13" width="8" height="8" rx="1.5"/><path d="M11 7h4a2 2 0 0 1 2 2v4"/><path d="M7 11v4a2 2 0 0 0 2 2h4"/></svg>'
    title: 核心只有一份
    details: 播放 API 与事件模型不依赖任何具体多媒体库，UI 与播放内核解耦。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3v5"/><path d="M15 3v5"/><path d="M6 8h12v3a6 6 0 0 1-6 6 6 6 0 0 1-6-6V8z"/><path d="M12 17v4"/></svg>'
    title: 后端可插拔
    details: FFmpeg、系统框架、还是测试用的假后端，都实现同一个 IBackend 接口。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><path d="M9 14l2 2 4-4"/></svg>'
    title: 合规先行
    details: 项目本体保持 Apache-2.0，依赖选型守住 LGPL/GPL 边界。
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M13 2L4 14h7l-1 8 9-12h-7l1-8z"/></svg>'
    title: 测试替身内置
    details: NullBackend 用纯状态机模拟完整播放行为，核心 API 的测试可在任何 CI 环境运行。
---
