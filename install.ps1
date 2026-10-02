# Soar 一键安装（Windows PowerShell）
#
#   irm https://raw.githubusercontent.com/cuihairu/soar/main/install.ps1 | iex
#
# 从滚动 nightly Release（每日构建，固定 tag nightly）下载
# soar-windows-x64.zip，解包安装到用户目录（DLL 随包同目录，解压即用），
# 把安装目录加入用户 PATH，验证 soar --version。重复执行即升级。
# 下载不需要任何凭据（公开仓库的 Release assets 匿名可下）。
#
# 平台覆盖与每日构建矩阵一致：windows-x64。Windows arm64 没有可用的
# 免费 CI runner，脚本遇到会明确报错退出，不会猜一个包装上。
# 未做代码签名：SmartScreen 拦截时选"更多信息 -> 仍要运行"。
#
# 环境变量：
#   SOAR_INSTALL_DIR   安装目录（默认 %LOCALAPPDATA%\Programs\soar）

$ErrorActionPreference = "Stop"

$Repo   = "cuihairu/soar"
$Base   = if ($env:SOAR_INSTALL_BASE) { $env:SOAR_INSTALL_BASE }
          else { "https://github.com/$Repo/releases/download/nightly" }
$Dir    = if ($env:SOAR_INSTALL_DIR) { $env:SOAR_INSTALL_DIR }
          else { Join-Path $env:LOCALAPPDATA "Programs\soar" }

function Die([string]$Msg) {
    # 不调 exit：irm | iex 管道语境里 exit 会关掉用户整个会话；
    # Write-Error 在 Stop 偏好下即中止脚本（交互控制台保持打开，
    # 脚本文件语境下退出码同样是 1）。
    Write-Error "install.ps1: 错误: $Msg"
}

# ---- CPU 架构（每日构建只出 windows-x64） ------------------------
switch ($env:PROCESSOR_ARCHITECTURE) {
    "AMD64" { $Asset = "soar-windows-x64.zip" }
    "ARM64" {
        Die "Windows arm64 目前没有每日构建产物（免费 CI runner 不含 Windows arm64）。可用平台: windows-x64"
    }
    default {
        Die "不认识的 CPU 架构: $($env:PROCESSOR_ARCHITECTURE)。每日构建覆盖: windows-x64"
    }
}

# ---- 下载（匿名）与解包 ------------------------------------------
$Tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("soar-install-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $Tmp | Out-Null
try {
    $ZipPath = Join-Path $Tmp $Asset
    Write-Host "==> 下载 $Asset（滚动 nightly，每日构建）"
    try {
        Invoke-WebRequest -Uri "$Base/$Asset" -OutFile $ZipPath -UseBasicParsing
    } catch {
        Die "下载失败: $Base/$Asset 。检查网络；若仓库未发布过 nightly Release，先在 Actions 里手动触发 Daily Build"
    }
    if ((Get-Item $ZipPath).Length -eq 0) {
        Die "下载内容为空: $Base/$Asset"
    }

    Write-Host "==> 解包"
    try {
        Expand-Archive -Path $ZipPath -DestinationPath $Tmp\x -Force
    } catch {
        Die "解包失败（zip 损坏？重新触发一次 Daily Build 再试）: $_"
    }
    if (-not (Test-Path (Join-Path $Tmp\x "soar.exe"))) {
        Die "包内没有 soar.exe（包结构变了？）"
    }

    # ---- 安装（重跑即升级：同名覆盖） ----------------------------
    if (-not (Test-Path $Dir)) {
        New-Item -ItemType Directory -Path $Dir -Force | Out-Null
    }
    $Exe = Join-Path $Dir "soar.exe"
    $OldVersion = $null
    if (Test-Path $Exe) {
        $OldVersion = (& $Exe --version 2>$null) -join " "
    }
    Copy-Item (Join-Path $Tmp\x "soar.exe") $Exe -Force
    Get-ChildItem (Join-Path $Tmp\x "*.dll") | Copy-Item -Destination $Dir -Force

    # ---- PATH（用户级，幂等：已在则不动） ------------------------
    $UserPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if (($UserPath -split ";") -notcontains $Dir) {
        [Environment]::SetEnvironmentVariable("Path", "$Dir;$UserPath", "User")
        Write-Host "==> 已把 $Dir 加入用户 PATH（新开终端生效）"
    }

    # ---- 验证：--version 打印版本即安装成功 ----------------------
    Write-Host "==> 验证"
    $Version = (& $Exe --version 2>$null) -join " "
    if ($LASTEXITCODE -ne 0 -or -not $Version) {
        Die "安装后验证失败: soar.exe --version 没有正常退出"
    }
    Write-Host $Version
    if ($OldVersion) {
        Write-Host "==> 升级完成: $OldVersion -> $Version ($Exe)"
    } else {
        Write-Host "==> 安装完成: $Version ($Exe)"
    }
    Write-Host "    播放: $Exe --backend=ffmpeg <媒体文件>"
    Write-Host "    注意: 未做代码签名，SmartScreen 拦截时选'更多信息 -> 仍要运行'"
} finally {
    Remove-Item -Recurse -Force $Tmp -ErrorAction SilentlyContinue
}
