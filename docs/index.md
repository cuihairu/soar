---
layout: home

hero:
  name: Soar
  text: Every format, set free
  tagline: An open-source C++17 media player built on FFmpeg + SDL2 — a unified playback abstraction plus a desktop UI, with decoding delegated to FFmpeg
  image:
    src: /logo.svg
    alt: Soar
  actions:
    - theme: brand
      text: Build guide
      link: /build
    - theme: alt
      text: Scope and roadmap
      link: /mvp
    - theme: alt
      text: GitHub
      link: https://github.com/cuihairu/soar

features:
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><rect x="3" y="3" width="8" height="8" rx="1.5"/><rect x="13" y="13" width="8" height="8" rx="1.5"/><path d="M11 7h4a2 2 0 0 1 2 2v4"/><path d="M7 11v4a2 2 0 0 0 2 2h4"/></svg>'
    title: One playback core
    details: The playback API and event model depend on no specific multimedia library; the UI is decoupled from the playback core.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M9 3v5"/><path d="M15 3v5"/><path d="M6 8h12v3a6 6 0 0 1-6 6 6 6 0 0 1-6-6V8z"/><path d="M12 17v4"/></svg>'
    title: Pluggable backends
    details: FFmpeg, a system framework, or the fake backend used for tests — they all implement the same IBackend interface.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8z"/><path d="M14 3v5h5"/><path d="M9 14l2 2 4-4"/></svg>'
    title: Compliance first
    details: The project itself stays Apache-2.0; dependency choices respect the LGPL/GPL boundary.
  - icon: '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" width="24" height="24"><path d="M13 2L4 14h7l-1 8 9-12h-7l1-8z"/></svg>'
    title: Built-in test double
    details: NullBackend simulates full playback behavior as a pure state machine, so core API tests run in any CI environment.
---
