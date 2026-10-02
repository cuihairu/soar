<script setup>
// 界面预览走马灯：应用真实截图（docs/public/screenshots/，Xvfb 下对
// 运行中的窗口截取）+ 原型图轨道（docs/public/prototypes/，设计稿）。
// 分组（播放/字幕翻译/设置/移动端）走顶部 tab，组内轮播：自动播放
// 4.5s、箭头、圆点，悬停暂停离开续播；图片统一 16:9 装框（边框用主题
// 变量），亮暗双主题下都清晰。
//
// 原型图是「投放即生效」：预期文件名预先登记在各组里，src 指向
// docs/public/prototypes/<file>；文件未到时加载失败，降级渲染占位卡
// （虚线框 + 待供图说明 + 投放路径），绝不显示破图。图放进去、下次
// 构建部署后 200 命中，同一张卡自动换成真图——组件零改动。
import { onMounted, onUnmounted, ref } from 'vue'
import { withBase } from 'vitepress'

// 真实截图条目（type 缺省为截图）
const shot = (src, alt, caption) => ({
  src: withBase('/screenshots/' + src),
  alt,
  caption,
})
// 原型图条目：file 落 docs/public/prototypes/，未到则占位
const proto = (file, title, caption) => ({
  proto: file,
  src: withBase('/prototypes/' + file),
  alt: title,
  caption,
})

const groups = [
  {
    key: 'playback',
    label: '播放',
    items: [
      shot(
        'player-styled-subtitle.png',
        '主操控屏：内嵌 ASS 字幕样式渲染',
        '主操控屏 —— 内嵌 ASS/SSA 字幕经 libass 样式渲染，字体、颜色、定位随脚本',
      ),
      shot(
        'player-playlist.png',
        'Playlist 播放队列浮层',
        '播放队列 —— 打开的文件排成队列，行内切换、一键移除，Loop/Shuffle 就地开关',
      ),
      shot(
        'player-media-info.png',
        'Media Info 媒体信息浮层',
        '媒体信息 —— 解码状态、轨道清单（视频/音频/字幕带语言与编码）一览',
      ),
      shot(
        'player-shortcuts.png',
        'Shortcuts 快捷键清单',
        '快捷键 —— H 呼出全清单：seek、音量、音轨/字幕切换、A-B loop、截图一屏可查',
      ),
      proto(
        'desktop-main.png',
        '桌面主界面原型稿',
        '桌面主界面（原型）—— 设计稿待供图，放入 docs/public/prototypes/desktop-main.png 后本卡自动换成真图',
      ),
    ],
  },
  {
    key: 'translate',
    label: '字幕翻译',
    items: [
      shot(
        'player-translate-toast.png',
        '内嵌轨翻译完成提示与译文字幕',
        '内嵌轨翻译 —— 导出 → 批量送 OpenAI 兼容端点 → 译文本自动挂为新字幕轨',
      ),
      shot(
        'player-translated-canvas.png',
        '译文字幕轨播放画面',
        '译文字幕播放中 —— 时间轴与原轨一致，原字幕轨不受影响',
      ),
      proto(
        'translate-panel.png',
        '字幕翻译面板原型稿',
        '字幕翻译面板（原型）—— 设计稿待供图，放入 docs/public/prototypes/translate-panel.png 后本卡自动换成真图',
      ),
    ],
  },
  {
    key: 'settings',
    label: '设置',
    items: [
      shot(
        'player-subtitle-settings.png',
        'Subtitle Settings 浮层与字幕轨下拉',
        'Subtitle Settings —— 轨道下拉直接切换，[translate] 行一键送译',
      ),
      proto(
        'settings.png',
        '设置面板原型稿',
        '设置面板（原型）—— 设计稿待供图，放入 docs/public/prototypes/settings.png 后本卡自动换成真图',
      ),
    ],
  },
  {
    key: 'mobile',
    label: '移动端',
    items: [
      proto(
        'mobile.png',
        '移动端原型稿',
        '移动端（原型）—— 设计稿待供图，放入 docs/public/prototypes/mobile.png 后本卡自动换成真图',
      ),
    ],
  },
]

const group = ref(0)
const current = ref(0)
// 加载失败登记：同一次页面浏览内不重复对缺失原型图发请求
const failed = ref({})
let timer = null

const items = () => groups[group.value].items

function go(i) {
  current.value = (i + items().length) % items().length
}
function next() {
  go(current.value + 1)
}
function prev() {
  go(current.value - 1)
}
function switchGroup(gi) {
  if (gi === group.value) return
  group.value = gi
  current.value = 0
}
function onError(s) {
  failed.value = { ...failed.value, [s.src]: true }
}
function visible(s) {
  return !s.proto || !failed.value[s.src]
}
// SSR 阶段就发出的原型图 404 会在 hydration 挂上监听器之前落地，error
// 事件丢失、卡片停在破图上（切组后的动态 img 才走得到 @error）。所以
// 挂载后对每个原型条目主动探测一次；命中的 404 与页面缓存合并成一次
// 请求，失败即把该条目钉成占位卡。
function probePrototypes() {
  for (const g of groups) {
    for (const s of g.items) {
      if (!s.proto) continue
      const img = new Image()
      img.onerror = () => onError(s)
      img.src = s.src
    }
  }
}
function pause() {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}
function resume() {
  pause()
  timer = setInterval(next, 4500)
}

onMounted(() => {
  probePrototypes()
  resume()
})
onUnmounted(pause)
</script>

<template>
  <section class="shots" aria-label="界面预览">
    <div class="vp-doc">
      <h2 id="界面预览">界面预览</h2>
    </div>
    <div
      class="tabs"
      role="tablist"
      aria-label="界面预览分组"
    >
      <button
        v-for="(g, gi) in groups"
        :key="g.key"
        role="tab"
        :aria-selected="gi === group"
        :class="{ on: gi === group }"
        @click="switchGroup(gi)"
      >
        {{ g.label }}<span class="n">{{ g.items.length }}</span>
      </button>
    </div>
    <div
      class="frame"
      @mouseenter="pause"
      @mouseleave="resume"
    >
      <div
        :key="groups[group].key"
        class="stage"
      >
        <div
          v-for="(s, i) in groups[group].items"
          :key="s.src"
          class="slide"
          :class="{ on: i === current }"
        >
          <img
            v-if="visible(s)"
            :src="s.src"
            :alt="s.alt"
            :loading="i === 0 ? 'eager' : 'lazy'"
            draggable="false"
            @error="onError(s)"
          >
          <!-- 原型图未到：占位卡（放入 docs/public/prototypes/ 即换成真图） -->
          <div
            v-else
            class="missing"
          >
            <svg
              viewBox="0 0 24 24"
              fill="none"
              stroke="currentColor"
              stroke-width="1.5"
              stroke-linecap="round"
              stroke-linejoin="round"
              aria-hidden="true"
            >
              <rect
                x="3"
                y="3"
                width="18"
                height="18"
                rx="2"
              />
              <path d="M3 16l5-5 4 4 3-3 6 6" />
              <circle
                cx="9"
                cy="9"
                r="1.4"
              />
            </svg>
            <p class="m-title">
              原型图待供图
            </p>
            <p class="m-desc">
              {{ s.alt }}
            </p>
            <code>docs/public/prototypes/{{ s.proto }}</code>
          </div>
        </div>
      </div>
      <template v-if="groups[group].items.length > 1">
        <button
          class="arrow left"
          aria-label="上一张"
          @click="prev"
        >
          ‹
        </button>
        <button
          class="arrow right"
          aria-label="下一张"
          @click="next"
        >
          ›
        </button>
        <div class="dots">
          <button
            v-for="(s, i) in groups[group].items"
            :key="i"
            :class="{ on: i === current }"
            :aria-label="`第 ${i + 1} 张`"
            @click="go(i)"
          />
        </div>
      </template>
    </div>
    <p class="caption">
      {{ groups[group].items[current].caption }}
    </p>
  </section>
</template>

<style scoped>
.shots {
  max-width: 1152px;
  margin: 0 auto;
  padding: 0 24px 12px;
}
.shots h2 {
  border-top: none;
  margin-top: 8px;
  font-size: 24px;
}
.tabs {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin: 0 0 10px;
}
.tabs button {
  padding: 5px 14px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 999px;
  background: var(--vp-c-bg-soft);
  color: var(--vp-c-text-2);
  font-size: 13px;
  line-height: 1.4;
  cursor: pointer;
  transition:
    color 0.2s ease,
    border-color 0.2s ease,
    background 0.2s ease;
}
.tabs button:hover {
  color: var(--vp-c-text-1);
  border-color: var(--vp-c-brand-1);
}
.tabs button.on {
  background: var(--vp-c-brand-1);
  border-color: var(--vp-c-brand-1);
  color: var(--vp-c-white, #fff);
}
.tabs .n {
  margin-left: 6px;
  font-size: 11px;
  opacity: 0.75;
}
.frame {
  position: relative;
  border: 1px solid var(--vp-c-divider);
  border-radius: 12px;
  background: var(--vp-c-bg-soft);
  overflow: hidden;
}
.stage {
  position: relative;
  aspect-ratio: 16 / 9;
}
.slide {
  position: absolute;
  inset: 0;
  opacity: 0;
  transition: opacity 0.45s ease;
}
.slide.on {
  opacity: 1;
}
.slide img {
  width: 100%;
  height: 100%;
  object-fit: contain;
}
/* 原型占位卡：与截图同框（16:9），虚线边界 + 居中说明 */
.missing {
  box-sizing: border-box;
  width: 100%;
  height: 100%;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 24px;
  border: 1.5px dashed var(--vp-c-divider);
  border-radius: 8px;
  background: var(--vp-c-bg-mute, var(--vp-c-bg-soft));
  color: var(--vp-c-text-2);
  text-align: center;
}
.missing svg {
  width: 44px;
  height: 44px;
  opacity: 0.55;
}
.missing .m-title {
  margin: 2px 0 0;
  font-size: 15px;
  font-weight: 600;
  color: var(--vp-c-text-1);
}
.missing .m-desc {
  margin: 0;
  font-size: 13px;
  max-width: 460px;
}
.missing code {
  font-size: 11.5px;
  padding: 2px 8px;
  border-radius: 6px;
  background: var(--vp-c-bg-soft);
  border: 1px solid var(--vp-c-divider);
  color: var(--vp-c-text-3);
}
.arrow {
  position: absolute;
  top: 50%;
  transform: translateY(-50%);
  width: 40px;
  height: 40px;
  border: 1px solid var(--vp-c-divider);
  border-radius: 50%;
  background: var(--vp-c-bg);
  color: var(--vp-c-text-1);
  font-size: 22px;
  line-height: 1;
  cursor: pointer;
  opacity: 0.85;
}
.arrow:hover {
  border-color: var(--vp-c-brand-1);
  color: var(--vp-c-brand-1);
  opacity: 1;
}
.arrow.left {
  left: 12px;
}
.arrow.right {
  right: 12px;
}
.dots {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 10px;
  display: flex;
  justify-content: center;
  gap: 8px;
}
.dots button {
  width: 9px;
  height: 9px;
  padding: 0;
  border: none;
  border-radius: 50%;
  background: var(--vp-c-text-3);
  opacity: 0.45;
  cursor: pointer;
}
.dots button.on {
  background: var(--vp-c-brand-1);
  opacity: 1;
}
.caption {
  margin: 10px 4px 0;
  font-size: 14px;
  color: var(--vp-c-text-2);
  text-align: center;
}
</style>
