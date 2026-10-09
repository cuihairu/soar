#!/usr/bin/env bash
# Soar Linux AppImage：从同源 zip 产物二次打包（与 deb/rpm 同一单一
# 事实源），包内布局与 zip 解包即用完全一致（single source of truth，
# 永不漂移）。
# 用法：package-linux-appimage.sh <x64|arm64> <zip 路径> <输出目录>
# 产物（固定资产名，nightly Release 直链取件）：
#   x64  → soar-linux-x64.AppImage
#   arm64 → soar-linux-arm64.AppImage
# 结构：AppDir 根 = zip 内容原样（soar + lib/ 运行时闭包 + 说明）+
#       AppRun（exec $HERE/soar）+ soar.desktop + 图标（soar.png/.DirIcon）。
#       二进制 RUNPATH=$ORIGIN/lib 在 zip 打包时已打好，soar 与 lib/ 在
#       AppDir 根保持同目录即可自解析，无需二次 patchelf。
# 压缩：gzip——AppImageKit type-2 runtime 的最大兼容面；xz/zstd 省的
#       几 MB 换来跨 runtime 年代的兼容风险，nightly 渠道不差这点体积。
# runtime：AppImageKit continuous 官方分发的 runtime-<arch>。它是唯一
#       的联网依赖；坏了会立刻现形——CI 真跑 --version 与裸容器冒烟
#       都是功能门，不进不出残缺包。
set -euo pipefail
# 载荷树的目录/文件权限落在产物里：mkdtemp 的 775/664 不该进 AppImage
umask 022

ARCH="$1"
ZIP="$2"
OUT="$3"

case "$ARCH" in
  x64|arm64) ;;
  *) echo "错误：用法 package-linux-appimage.sh <x64|arm64> <zip> <输出目录>" >&2; exit 2 ;;
esac
[ -f "$ZIP" ] || { echo "错误：zip 产物不存在：$ZIP" >&2; exit 2; }
mkdir -p "$OUT"
command -v curl >/dev/null 2>&1 || { echo "错误：需要 curl 下载 AppImageKit runtime" >&2; exit 2; }
command -v mksquashfs >/dev/null 2>&1 || { echo "错误：需要 mksquashfs（squashfs-tools）" >&2; exit 2; }

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# AppImageKit runtime（squashfs 镜像前置拼接的类型 2 runtime）。arm64
# runner 上是 aarch64 原生镜像，跑 arm64 runtime 与产物形态一致。
case "$ARCH" in
  x64)   RUNTIME=runtime-x86_64 ;;
  arm64) RUNTIME=runtime-aarch64 ;;
esac
curl -fsSL --retry 3 -o "$STAGE/$RUNTIME" \
  "https://github.com/AppImage/AppImageKit/releases/download/continuous/$RUNTIME"

# —— 载荷树：zip 内容原样落进 AppDir 根（与 deb/rpm 的 /opt/soar 同
# 单一事实源）——
APPDIR="$STAGE/soar.AppDir"
mkdir -p "$APPDIR"
unzip -q "$ZIP" -d "$APPDIR"
# zip 的 exec 位依赖打包端保留；AppImage 里必须有
chmod 755 "$APPDIR/soar"

cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# AppImage 入口：转发到随包自包含的 soar（RUNPATH=$ORIGIN/lib 自解析）
HERE="$(dirname "$(readlink -f "$0")")"
exec "${HERE}/soar" "$@"
EOF
chmod 755 "$APPDIR/AppRun"

# .desktop 与图标口径与 deb/rpm 安装包同源（assets/icons 是 logo.svg
# 的栅格化产物，与 Windows soar.ico 同源）。脚本在 scripts/ 下，
# 仓库根向上一级。
ICON_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/icons"
install -Dm644 "$ICON_SRC/soar-256.png" "$APPDIR/soar.png"
cp -L "$APPDIR/soar.png" "$APPDIR/.DirIcon"
cat > "$APPDIR/soar.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Soar
Comment=媒体播放器（nightly，运行库随包自包含）
GenericName=Media Player
Exec=soar --gui
Icon=soar
Terminal=false
Categories=AudioVideo;Video;Player;
MimeType=video/*;audio/*;application/x-bittorrent;x-scheme-handler/magnet;
StartupWMClass=soar
StartupNotify=true
EOF

ASSET="soar-linux-$ARCH.AppImage"

# squashfs 镜像（全 root 属主：AppImage 的运行时契约）+ runtime 前置
# 拼接 = 可执行单文件。顺序不可反（runtime 必须在前，ELF 头才能被
# 内核/loader 接住；紧跟其后的 8 字节是 squashfs 超级块签名）。
SQIMG="$STAGE/sqfs.img"
mksquashfs "$APPDIR" "$SQIMG" -root-owned -noappend -comp gzip -no-progress
cat "$STAGE/$RUNTIME" > "$OUT/$ASSET"
cat "$SQIMG" >> "$OUT/$ASSET"
chmod 755 "$OUT/$ASSET"
ls -la "$OUT/$ASSET"
echo "$ASSET"
