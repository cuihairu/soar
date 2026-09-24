import { defineConfig } from 'vitepress'

// The docs live next to the source as plain Markdown (build.md, mvp.md,
// licensing.md, coverage-notes.md). VitePress wraps them with this shell;
// /logo.svg comes from docs/public/ and doubles as the site favicon.
export default defineConfig({
  lang: 'zh-CN',
  title: 'Soar',
  description: '万般格式，任其翱翔 —— 用 C++23 从零打造的万能格式开源播放器',
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/logo.svg' }]],
  themeConfig: {
    logo: '/logo.svg',
    siteTitle: 'Soar',
    nav: [
      { text: '指南', link: '/build', activeMatch: '/^/(build|mvp|licensing)/' },
      { text: '开发', link: '/coverage-notes', activeMatch: '/coverage-notes/' }
    ],
    sidebar: {
      '/': [
        {
          text: '指南',
          items: [
            { text: '构建', link: '/build' },
            { text: '项目边界与路线图', link: '/mvp' }
          ]
        },
        {
          text: '合规与质量',
          items: [
            { text: '许可与合规', link: '/licensing' },
            { text: '覆盖率口径与未覆盖项', link: '/coverage-notes' }
          ]
        }
      ]
    },
    socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/soar' }],
    outline: { level: [2, 3], label: '本页目录' },
    docFooter: { prev: '上一页', next: '下一页' },
    lastUpdated: {
      text: '最后更新',
      formatOptions: {
        dateStyle: 'short',
        timeStyle: 'short'
      }
    }
  }
})
