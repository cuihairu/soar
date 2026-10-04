#!/usr/bin/env bash
# Soar Linux 安装包（deb / rpm）：从同源 zip 产物二次打包，包内布局与
# zip 解包即用完全一致（单一事实源，两边永不漂移；memex 同款流程）。
# 用法：package-linux-pkg.sh <deb|rpm> <x64|arm64> <zip 路径> <输出目录> [版本x.y.z]
# 产物（固定资产名，nightly Release 直链取件）：
#   x64  → soar-linux-x64.deb  / soar-linux-x64.rpm
#   arm64 → soar-linux-arm64.deb / soar-linux-arm64.rpm
# 布局：/opt/soar/（zip 内容原样：soar + lib/ 运行时闭包 + 说明）+
#       /usr/bin/soar（入口包装）+ /usr/share/applications/soar.desktop
#       + hicolor 图标（Icon=soar 的解析来源，assets/icons 同源栅格）。
# 自包含口径：运行库全在 /opt/soar/lib（RUNPATH=$ORIGIN/lib），不声明
# 外部 Depends——目标机只需 glibc/libstdc++ 底线（与 zip 产物同一要求，
# 见包内 PLATFORM-NOTES.txt）。
set -euo pipefail
# 载荷树的目录/文件权限落在包里：mkdtemp 的 775/664 不该进产物
umask 022

FMT="$1"
ARCH="$2"
ZIP="$3"
OUT="$4"
# 版本：优先参数（daily-build 从根 CMakeLists 提取）
VERSION="${5:-0.1.0}"
DATE="$(date -u +%Y%m%d)"

case "$FMT:$ARCH" in
  deb:x64|deb:arm64|rpm:x64|rpm:arm64) ;;
  *) echo "错误：用法 package-linux-pkg.sh <deb|rpm> <x64|arm64> <zip> <输出目录> [版本]" >&2; exit 2 ;;
esac
[ -f "$ZIP" ] || { echo "错误：zip 产物不存在：$ZIP" >&2; exit 2; }
mkdir -p "$OUT"

DESCRIPTION="Soar 媒体播放器（nightly）——FFmpeg 后端 / SDL2 窗口 / 字幕与 P2P 流播，运行库随包自包含。"

# deb/rpm 架构名映射
case "$ARCH" in
  x64)   DEB_ARCH=amd64;  RPM_ARCH=x86_64 ;;
  arm64) DEB_ARCH=arm64;  RPM_ARCH=aarch64 ;;
esac

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

# —— 载荷树（deb/rpm 共用）——
PAYLOAD="$STAGE/payload"
mkdir -p "$PAYLOAD/opt/soar" "$PAYLOAD/usr/bin"
unzip -q "$ZIP" -d "$PAYLOAD/opt/soar"
# zip 的 exec 位依赖打包端保留；安装包里必须有
chmod 755 "$PAYLOAD/opt/soar/soar"

cat > "$PAYLOAD/usr/bin/soar" <<'EOF'
#!/bin/sh
# 入口包装：转发到随包自包含的 /opt/soar（RUNPATH=$ORIGIN/lib 自解析）
exec /opt/soar/soar "$@"
EOF
chmod 755 "$PAYLOAD/usr/bin/soar"

mkdir -p "$PAYLOAD/usr/share/applications"
# 图标 = 仓库内 assets/icons（assets/logo.svg 的栅格化产物，与 Windows
# soar.ico 同源）。脚本在 scripts/ 下，仓库根向上一级。
ICON_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/icons"
for s in 16 32 48 64 128 256; do
  install -Dm644 "$ICON_SRC/soar-$s.png" \
    "$PAYLOAD/usr/share/icons/hicolor/${s}x${s}/apps/soar.png"
done
cat > "$PAYLOAD/usr/share/applications/soar.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Soar
Comment=媒体播放器（nightly，运行库随包自包含）
GenericName=Media Player
Exec=soar
Icon=soar
Terminal=false
Categories=AudioVideo;Video;Player;
StartupWMClass=soar
StartupNotify=true
EOF

ASSET="soar-linux-$ARCH"

if [ "$FMT" = deb ]; then
  mkdir -p "$PAYLOAD/DEBIAN"
  cat > "$PAYLOAD/DEBIAN/control" <<EOF
Package: soar
Version: ${VERSION}+${DATE}
Section: video
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: cuihairu <cuihairu@users.noreply.github.com>
Description: ${DESCRIPTION}
EOF
  # --root-owner-group：非 root 构建也能落 root:root 属主（CI 免 fakeroot）
  dpkg-deb --root-owner-group --build "$PAYLOAD" "$OUT/$ASSET.deb"
  echo "$ASSET.deb"
  exit 0
fi

# —— rpm ——
# spec 的 %install 从暂存载荷树拷入 buildroot（无源码编译动作）
SPEC="$STAGE/soar.spec"
cat > "$SPEC" <<EOF
Name: soar
Version: ${VERSION}.${DATE}
Release: 1
Summary: Soar media player (nightly)
License: Apache-2.0
URL: https://github.com/cuihairu/soar
BuildArch: ${RPM_ARCH}
# 自包含口径（与 deb 侧手写 control 无 Depends 对齐）：不扫描捆绑库
# ELF 自动生成 soname 依赖——运行库随包在 /opt/soar/lib，目标机只需
# glibc/libstdc++ 底线
AutoReq: no
AutoProv: no
%description
${DESCRIPTION}

%prep

%build

%install
cp -a "$PAYLOAD/opt" "%{buildroot}/opt"
cp -a "$PAYLOAD/usr" "%{buildroot}/usr"

%files
/opt/soar
/usr/bin/soar
/usr/share/applications/soar.desktop
/usr/share/icons/hicolor/*/apps/soar.png
%changelog
EOF

# _topdir 收进暂存：不污染 ~/rpmbuild；--target 定死架构
RPMTOP="$STAGE/rpmbuild"
mkdir -p "$RPMTOP/BUILD" "$RPMTOP/RPMS" "$RPMTOP/SOURCES" "$RPMTOP/SPECS"
rpmbuild -bb --target "$RPM_ARCH" \
  --define "_topdir $RPMTOP" \
  "$SPEC" >/dev/null
cp "$RPMTOP/RPMS/$RPM_ARCH/soar-"*.rpm "$OUT/$ASSET.rpm"
echo "$ASSET.rpm"
