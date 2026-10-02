// soar docs theme — 默认主题 + 品牌配色（custom.css 内容来自 logo 的 #d4237a）
// VitePress 会自动发现本文件，无需在 config.mts 里引用。
// 「界面预览」走马灯（Screenshots.vue）经 home 布局插槽
// home-features-before 挂在 hero 正下方、features 卡片正上方。
import { h } from 'vue'
import DefaultTheme from 'vitepress/theme'
import './custom.css'
import Screenshots from './Screenshots.vue'

export default {
  extends: DefaultTheme,
  Layout: () =>
    h(DefaultTheme.Layout, null, {
      'home-features-before': () => h(Screenshots),
    }),
}
