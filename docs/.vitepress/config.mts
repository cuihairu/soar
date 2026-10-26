import { defineConfig } from 'vitepress'

// English pages live at the site root (X.md); Chinese pages live in the
// zh/ directory (zh/X.md) so VitePress official locales serve them at /zh.
export default defineConfig({
  base: '/soar/',
  head: [['link', { rel: 'icon', type: 'image/svg+xml', href: '/soar/logo.svg' }]],
  locales: {
    root: {
      label: 'English',
      lang: 'en',
      title: 'Soar',
      description:
        'Every format, set free — an open-source C++17 media player built on FFmpeg + SDL2',
      themeConfig: {
        logo: '/logo.svg',
        siteTitle: 'Soar',
        nav: [
          { text: 'Guide', link: '/build', activeMatch: '^/(build|mvp|licensing)/' },
          { text: 'Architecture', link: '/architecture', activeMatch: '^/architecture/' },
          { text: 'Prototypes', link: '/prototypes', activeMatch: '^/prototypes/' },
          { text: 'Development', link: '/coverage-notes', activeMatch: '^/coverage-notes/' }
        ],
        sidebar: {
          '/': [
            {
              text: 'Guide',
              items: [
                { text: 'Build', link: '/build' },
                { text: 'Scope and roadmap', link: '/mvp' },
                { text: 'Architecture overview', link: '/architecture' },
                { text: 'Prototypes', link: '/prototypes' }
              ]
            },
            {
              text: 'Compliance and quality',
              items: [
                { text: 'Licensing and compliance', link: '/licensing' },
                { text: 'Coverage notes', link: '/coverage-notes' }
              ]
            }
          ]
        },
        socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/soar' }],
        outline: { level: [2, 3], label: 'On this page' },
        docFooter: { prev: 'Previous', next: 'Next' },
        lastUpdated: {
          text: 'Last updated',
          formatOptions: { dateStyle: 'short', timeStyle: 'short' }
        }
      }
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      title: 'Soar',
      description: '万般格式，任其翱翔 —— 基于 FFmpeg + SDL2 的 C++17 开源媒体播放器',
      themeConfig: {
        logo: '/logo.svg',
        siteTitle: 'Soar',
        nav: [
          { text: '指南', link: '/zh/build', activeMatch: '^/zh/(build|mvp|licensing)/' },
          { text: '架构', link: '/zh/architecture', activeMatch: '^/zh/architecture/' },
          { text: '原型', link: '/zh/prototypes', activeMatch: '^/zh/prototypes/' },
          { text: '开发', link: '/zh/coverage-notes', activeMatch: '^/zh/coverage-notes/' }
        ],
        sidebar: {
          '/zh/': [
            {
              text: '指南',
              items: [
                { text: '构建', link: '/zh/build' },
                { text: '项目边界与路线图', link: '/zh/mvp' },
                { text: '架构总览', link: '/zh/architecture' },
                { text: '原型展示', link: '/zh/prototypes' }
              ]
            },
            {
              text: '合规与质量',
              items: [
                { text: '许可与合规', link: '/zh/licensing' },
                { text: '覆盖率口径与未覆盖项', link: '/zh/coverage-notes' }
              ]
            }
          ]
        },
        socialLinks: [{ icon: 'github', link: 'https://github.com/cuihairu/soar' }],
        outline: { level: [2, 3], label: '本页目录' },
        docFooter: { prev: '上一页', next: '下一页' },
        lastUpdated: {
          text: '最后更新',
          formatOptions: { dateStyle: 'short', timeStyle: 'short' }
        }
      }
    }
  }
})
