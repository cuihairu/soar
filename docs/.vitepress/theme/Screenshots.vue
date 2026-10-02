<script setup>
// 界面预览走马灯：应用真实截图（docs/public/screenshots/，Xvfb 下对
// 运行中的窗口截取）。自动播放 + 箭头 + 圆点；悬停暂停，离开续播。
// 图片统一 16:9 装框（边框用主题变量），亮暗主题下都清晰。
import { onMounted, onUnmounted, ref } from 'vue'
import { withBase } from 'vitepress'

const slides = [
  {
    src: withBase('/screenshots/player-styled-subtitle.png'),
    alt: '主操控屏：内嵌 ASS 字幕样式渲染',
    caption: '主操控屏 —— 内嵌 ASS/SSA 字幕经 libass 样式渲染，字体、颜色、定位随脚本',
  },
  {
    src: withBase('/screenshots/player-subtitle-settings.png'),
    alt: 'Subtitle Settings 浮层与字幕轨下拉',
    caption: 'Subtitle Settings —— 轨道下拉直接切换，[translate] 行一键送译',
  },
  {
    src: withBase('/screenshots/player-translate-toast.png'),
    alt: '内嵌轨翻译完成提示与译文字幕',
    caption: '内嵌轨翻译 —— 导出 → 批量送 OpenAI 兼容端点 → 译文本自动挂为新字幕轨',
  },
  {
    src: withBase('/screenshots/player-translated-canvas.png'),
    alt: '译文字幕轨播放画面',
    caption: '译文字幕播放中 —— 时间轴与原轨一致，原字幕轨不受影响',
  },
]

const current = ref(0)
let timer = null

function go(i) {
  current.value = (i + slides.length) % slides.length
}
function next() {
  go(current.value + 1)
}
function prev() {
  go(current.value - 1)
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

onMounted(resume)
onUnmounted(pause)
</script>

<template>
  <section class="shots" aria-label="界面预览">
    <div class="vp-doc">
      <h2 id="界面预览">界面预览</h2>
    </div>
    <div
      class="frame"
      @mouseenter="pause"
      @mouseleave="resume"
    >
      <div class="stage">
        <img
          v-for="(s, i) in slides"
          :key="s.src"
          :src="s.src"
          :alt="s.alt"
          :class="{ on: i === current }"
          :loading="i === 0 ? 'eager' : 'lazy'"
          draggable="false"
        >
      </div>
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
          v-for="(s, i) in slides"
          :key="i"
          :class="{ on: i === current }"
          :aria-label="`第 ${i + 1} 张`"
          @click="go(i)"
        />
      </div>
    </div>
    <p class="caption">
      {{ slides[current].caption }}
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
.stage img {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  object-fit: contain;
  opacity: 0;
  transition: opacity 0.45s ease;
}
.stage img.on {
  opacity: 1;
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
