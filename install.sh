#!/bin/sh
# Soar 一键安装（Linux / macOS）
#
#   curl -fsSL https://raw.githubusercontent.com/cuihairu/soar/main/install.sh | sh
#
# 从滚动 nightly Release（每日构建，固定 tag nightly）下载当前平台的
# zip，解包安装到 ~/.local/bin（可用环境变量覆盖），Linux 包还把
# 捆绑的 lib/ 运行时库装到同目录，验证 soar --version。
# 重复执行即升级到最新 nightly。下载不需要任何凭据（公开仓库的
# Release assets 匿名可下）。
#
# 平台覆盖与每日构建矩阵一致：linux-x64 / linux-arm64 / macos-arm64。
# macOS Intel 与 Windows arm64 没有可用的免费 CI runner，本脚本遇到
# 会明确报错退出，不会猜一个包装上。
#
# 环境变量：
#   SOAR_INSTALL_DIR   安装目录（默认 $HOME/.local/bin）
#   SOAR_INSTALL_BASE  下载基址（默认官方 nightly Release，可指向镜像）
#
# 边界（如实）：soar 是桌面播放器，没有常驻服务形态，本脚本不注册
# 任何系统服务；nightly 未做代码签名，macOS 首次运行需 xattr -cr。

set -eu

REPO_DEFAULT="cuihairu/soar"
BASE_URL="${SOAR_INSTALL_BASE:-https://github.com/${SOAR_INSTALL_REPO:-$REPO_DEFAULT}/releases/download/nightly}"
INSTALL_DIR="${SOAR_INSTALL_DIR:-$HOME/.local/bin}"

say() { printf '%s\n' "$*"; }
die() { printf 'install.sh: 错误: %s\n' "$*" >&2; exit 1; }

# ---- 依赖的命令，缺哪个报哪个 ------------------------------------
need() { command -v "$1" >/dev/null 2>&1 || die "缺少命令 '$1'，请先安装后重试"; }

need "uname"
if ! command -v unzip >/dev/null 2>&1 && ! command -v python3 >/dev/null 2>&1; then
  die "解包需要 unzip 或 python3（任一即可），两者都没找到"
fi
if command -v curl >/dev/null 2>&1; then
  FETCH="curl -fL --retry 3 -o"
elif command -v wget >/dev/null 2>&1; then
  FETCH="wget -q -O"
else
  die "下载需要 curl 或 wget（任一即可），两者都没找到"
fi

# ---- 操作系统 ----------------------------------------------------
case "$(uname -s)" in
  Linux) OS_TAG="linux" ;;
  Darwin) OS_TAG="macos" ;;
  MINGW* | MSYS* | CYGWIN*) die "这是 Windows 环境，请用 PowerShell 一键安装: irm https://raw.githubusercontent.com/${SOAR_INSTALL_REPO:-$REPO_DEFAULT}/main/install.ps1 | iex" ;;
  *) die "不认识的操作系统: $(uname -s)。支持 Linux 与 macOS" ;;
esac

# ---- CPU 架构 ----------------------------------------------------
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64 | amd64)
    [ "$OS_TAG" = "linux" ] || die "macOS Intel (x86_64) 目前没有每日构建产物（免费 CI runner 不含 macOS Intel）。可用平台: linux-x64 / linux-arm64 / macos-arm64"
    ASSET="soar-linux-x64.zip"
    ;;
  aarch64 | arm64)
    ASSET="soar-${OS_TAG}-arm64.zip"
    ;;
  *)
    die "不认识的 CPU 架构: $ARCH。每日构建覆盖: linux-x64 / linux-arm64 / macos-arm64"
    ;;
esac

# ---- 下载（匿名）与解包 ------------------------------------------
TMPDIR_DL="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_DL"' EXIT
ZIP_PATH="$TMPDIR_DL/$ASSET"

say "==> 下载 $ASSET（滚动 nightly，每日构建）"
# shellcheck disable=SC2086
$FETCH "$ZIP_PATH" "$BASE_URL/$ASSET" ||
  die "下载失败: $BASE_URL/$ASSET 。检查网络；若仓库未发布过 nightly Release，先在 Actions 里手动触发 Daily Build"
[ -s "$ZIP_PATH" ] || die "下载内容为空: $BASE_URL/$ASSET"

say "==> 解包"
unzip -oq "$ZIP_PATH" -d "$TMPDIR_DL/x" 2>/dev/null ||
  python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
    "$ZIP_PATH" "$TMPDIR_DL/x" ||
  die "解包失败（zip 损坏？重新触发一次 Daily Build 再试）"
[ -f "$TMPDIR_DL/x/soar" ] || die "包内没有 soar 可执行文件（包结构变了？）"

# ---- 安装（重跑即升级：同名覆盖，先落临时名再 mv 保持原子） ------
mkdir -p "$INSTALL_DIR"
DEST="$INSTALL_DIR/soar"
OLD_VERSION=""
[ -x "$DEST" ] && OLD_VERSION="$("$DEST" --version 2>/dev/null || true)"
cp "$TMPDIR_DL/x/soar" "$DEST.tmp.$$"
chmod 755 "$DEST.tmp.$$"
mv -f "$DEST.tmp.$$" "$DEST"
# Linux 包随附捆绑运行时库（lib/ 与 soar 同目录，相对定位）。
# 升级时整体替换 lib/，不让旧 nightly 的库与新二进制混搭。
if [ -d "$TMPDIR_DL/x/lib" ]; then
  say "==> 安装捆绑运行时库 (lib/)"
  rm -rf "${INSTALL_DIR:?}/lib.new.$$"
  cp -R "$TMPDIR_DL/x/lib" "${INSTALL_DIR:?}/lib.new.$$"
  rm -rf "${INSTALL_DIR:?}/lib"
  mv "${INSTALL_DIR:?}/lib.new.$$" "${INSTALL_DIR:?}/lib"
fi
# macOS 未签名：清隔离属性让 Gatekeeper 放行（无属性时静默跳过）
[ "$OS_TAG" = "macos" ] && xattr -cr "$DEST" 2>/dev/null || true

# ---- PATH --------------------------------------------------------
case ":$PATH:" in
  *":$INSTALL_DIR:"*) : ;;
  *)
    for RC in "$HOME/.profile" $([ "$OS_TAG" = "macos" ] && printf '%s' "$HOME/.zshrc" || printf '%s' "$HOME/.bashrc"); do
      if [ -f "$RC" ] && ! grep -qF "$INSTALL_DIR" "$RC" 2>/dev/null; then
        # $PATH 故意留到新 shell 启动时展开（写入的是字面行）
        # shellcheck disable=SC2016
        printf '\n# added by soar install.sh\nexport PATH="%s:$PATH"\n' "$INSTALL_DIR" >> "$RC"
        say "==> 已把 $INSTALL_DIR 加入 PATH（写入 $RC；新开终端生效）"
      fi
    done
    ;;
esac

# ---- 验证：--version 打印版本即安装成功 --------------------------
# 验证失败时临时目录还没被 trap 清掉——把包内 PLATFORM-NOTES.txt
# 一并打出来，报错自包含（用户不用再去翻已解压删除的 zip）。
say "==> 验证"
if ! VERSION="$("$DEST" --version 2>/dev/null)"; then
  if [ -f "$TMPDIR_DL/x/PLATFORM-NOTES.txt" ]; then
    printf '\n包内 PLATFORM-NOTES.txt:\n' >&2
    cat "$TMPDIR_DL/x/PLATFORM-NOTES.txt" >&2
    printf '\n' >&2
  fi
  die "安装后验证失败: $DEST --version 没有正常退出（系统 glibc/libstdc++ 版本过低？见上方包内说明）"
fi
say "$VERSION"
if [ -n "$OLD_VERSION" ]; then
  say "==> 升级完成: $OLD_VERSION -> $VERSION ($DEST)"
else
  say "==> 安装完成: $VERSION ($DEST)"
fi
say "    播放: $DEST --backend=ffmpeg <媒体文件>"
say "    注意: Linux 运行时库已随包捆绑（$INSTALL_DIR/lib），仅依赖系统 glibc/libstdc++"
