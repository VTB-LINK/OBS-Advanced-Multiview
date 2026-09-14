# 在对应版本的 Developer PowerShell for VS 中运行 CMake 配置
# （提交默认预设为 VS 2022；本地若覆盖 CMakePresets 为 VS 2026，则用 VS 2026 的 Developer PowerShell）
# 此脚本确保使用正确的 Visual Studio 环境
# 注意：此脚本位于 docs/setup/ 目录，会自动定位到项目根目录

Write-Host "=== OBS Advanced Multiview - CMake 配置 ===" -ForegroundColor Cyan
Write-Host ""

# 获取项目根目录（脚本在 docs/setup，需要往上两级）
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $ProjectRoot

# 设置代理
$env:HTTP_PROXY = "http://127.0.0.1:31333"
$env:HTTPS_PROXY = "http://127.0.0.1:31333"

Write-Host "✓ 代理已设置: $env:HTTP_PROXY" -ForegroundColor Green
Write-Host "✓ 工作目录: $ProjectRoot" -ForegroundColor Green
Write-Host ""

# 检查是否在 VS Developer PowerShell 中
if ($env:VSINSTALLDIR) {
    Write-Host "✓ 检测到 Visual Studio 环境: $env:VSINSTALLDIR" -ForegroundColor Green
} else {
    Write-Host "⚠ 警告: 未检测到 Visual Studio Developer 环境" -ForegroundColor Yellow
    Write-Host "  建议在对应版本的 'Developer PowerShell for VS' 中运行此脚本（默认 VS 2022；本地 VS 2026 覆盖则用 VS 2026 的）" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "开始配置 CMake..." -ForegroundColor Cyan
Write-Host "这将下载约 800 MB 的依赖（OBS Studio 源码 + Qt6 + 预构建库）" -ForegroundColor Gray
Write-Host "预计时间：5-15 分钟" -ForegroundColor Gray
Write-Host ""

# 清理旧的构建目录（如果配置失败过）
if (Test-Path "build_x64\CMakeCache.txt") {
    Write-Host "发现旧的 CMake 缓存，正在清理..." -ForegroundColor Yellow
    Remove-Item "build_x64\CMakeCache.txt" -Force -ErrorAction SilentlyContinue
    Remove-Item "build_x64\CMakeFiles" -Recurse -Force -ErrorAction SilentlyContinue
}

# 运行 CMake 配置
try {
    cmake --preset windows-x64
    
    if ($LASTEXITCODE -eq 0) {
        Write-Host ""
        Write-Host "✓ CMake 配置成功！" -ForegroundColor Green
        Write-Host ""
        Write-Host "接下来可以构建项目:" -ForegroundColor Cyan
        Write-Host "  cmake --build build_x64 --config Debug" -ForegroundColor White
        Write-Host "  cmake --build build_x64 --config RelWithDebInfo" -ForegroundColor White
    } else {
        Write-Host ""
        Write-Host "✗ CMake 配置失败 (退出码: $LASTEXITCODE)" -ForegroundColor Red
        Write-Host ""
        Write-Host "请确认:" -ForegroundColor Yellow
        Write-Host "1. 在对应版本的 'Developer PowerShell for VS' 中运行（默认 VS 2022 / 本地 VS 2026）" -ForegroundColor White
        Write-Host "2. Visual Studio 2022 已安装 'MSVC v143' 组件" -ForegroundColor White
        Write-Host "3. Windows 11 SDK (10.0.22621.0) 已安装" -ForegroundColor White
    }
} catch {
    Write-Host ""
    Write-Host "✗ 发生错误: $_" -ForegroundColor Red
}
