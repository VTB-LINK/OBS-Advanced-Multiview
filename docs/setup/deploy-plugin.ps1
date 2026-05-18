# 部署插件到 OBS
# 使用方法：
#   .\docs\setup\deploy-plugin.ps1              # 部署 Debug 版本
#   .\docs\setup\deploy-plugin.ps1 RelWithDebInfo  # 部署 RelWithDebInfo 版本
# 
# 注意：此脚本位于 docs/setup/ 目录，从项目根目录运行

param(
    [string]$BuildConfig = "Debug"
)

#region 配置区域 - 根据你的环境修改这里

# OBS 部署目标路径（按优先级尝试）
# 1. 首选：OBS Portable 版本（无需管理员权限）
$OBS_PORTABLE_PATH = "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\"

# 2. 备选：用户插件目录（推荐，无需管理员权限）
$OBS_USER_PLUGIN_PATH = "$env:APPDATA\obs-studio\obs-plugins\64bit"

# 3. 备选：已安装版本（需要管理员权限）
$OBS_INSTALLED_PATH = "C:\Program Files\obs-studio\obs-plugins\64bit"

# OBS 可执行文件路径（用于提示启动命令）
$OBS_EXE_PATH = "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\bin\64bit\obs64.exe"

#endregion

# 获取项目根目录（脚本在 docs/setup，需要往上两级）
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# 从 buildspec.json 读取插件名称
$BuildSpecPath = Join-Path $ProjectRoot "buildspec.json"
if (Test-Path $BuildSpecPath) {
    $BuildSpec = Get-Content $BuildSpecPath | ConvertFrom-Json
    $PluginName = $BuildSpec.name
} else {
    Write-Host "✗ buildspec.json not found at: $BuildSpecPath" -ForegroundColor Red
    exit 1
}

# 确定 OBS 插件目录（按优先级尝试）
$ObsPluginDir = $null
if (Test-Path $OBS_PORTABLE_PATH) {
    $ObsPluginDir = $OBS_PORTABLE_PATH
    Write-Host "→ 使用 OBS Portable 路径" -ForegroundColor Cyan
} elseif (Test-Path $OBS_USER_PLUGIN_PATH) {
    $ObsPluginDir = $OBS_USER_PLUGIN_PATH
    Write-Host "→ 使用用户插件路径" -ForegroundColor Cyan
} elseif (Test-Path $OBS_INSTALLED_PATH) {
    $ObsPluginDir = $OBS_INSTALLED_PATH
    Write-Host "→ 使用已安装 OBS 路径（可能需要管理员权限）" -ForegroundColor Yellow
} else {
    Write-Host "✗ 未找到 OBS 插件目录！" -ForegroundColor Red
    Write-Host ""
    Write-Host "请修改脚本顶部的路径配置：" -ForegroundColor Yellow
    Write-Host "  OBS_PORTABLE_PATH    = \"$OBS_PORTABLE_PATH\"" -ForegroundColor Gray
    Write-Host "  OBS_USER_PLUGIN_PATH = \"$OBS_USER_PLUGIN_PATH\"" -ForegroundColor Gray
    Write-Host "  OBS_INSTALLED_PATH   = \"$OBS_INSTALLED_PATH\"" -ForegroundColor Gray
    exit 1
}

$PluginDll = Join-Path $ProjectRoot "build_x64\$BuildConfig\$PluginName.dll"

if (-not (Test-Path $PluginDll)) {
    Write-Host "✗ 未找到插件 DLL：$PluginDll" -ForegroundColor Red
    Write-Host ""
    Write-Host "请先构建插件：" -ForegroundColor Yellow
    Write-Host "  cmake --build build_x64 --config $BuildConfig" -ForegroundColor White
    exit 1
}

try {
    # 确保目标目录存在（用户插件目录可能需要创建）
    if (-not (Test-Path $ObsPluginDir)) {
        Write-Host "→ 创建插件目录：$ObsPluginDir" -ForegroundColor Cyan
        New-Item -Path $ObsPluginDir -ItemType Directory -Force | Out-Null
    }
    
    Copy-Item $PluginDll -Destination $ObsPluginDir -Force
    Write-Host "✓ 插件 DLL 部署成功！" -ForegroundColor Green
    Write-Host "  From: $PluginDll" -ForegroundColor Gray
    Write-Host "  To:   $ObsPluginDir" -ForegroundColor Gray

    # 部署 data 文件（locale 等）
    $DataSource = Join-Path $ProjectRoot "build_x64\rundir\$BuildConfig\$PluginName"
    # OBS data 目录：与 obs-plugins 同级的 data/obs-plugins/<plugin-name>/
    $ObsDataDir = Join-Path (Split-Path (Split-Path $ObsPluginDir)) "data\obs-plugins\$PluginName"
    if (Test-Path $DataSource) {
        if (-not (Test-Path $ObsDataDir)) {
            New-Item -Path $ObsDataDir -ItemType Directory -Force | Out-Null
        }
        Copy-Item "$DataSource\*" -Destination $ObsDataDir -Recurse -Force
        Write-Host "✓ 插件数据部署成功！" -ForegroundColor Green
        Write-Host "  From: $DataSource" -ForegroundColor Gray
        Write-Host "  To:   $ObsDataDir" -ForegroundColor Gray
    } else {
        Write-Host "⚠ 未找到插件数据目录，跳过 data 部署" -ForegroundColor Yellow
    }

    Write-Host ""
    
    if (Test-Path $OBS_EXE_PATH) {
        Write-Host "现在可以启动 OBS 测试插件：" -ForegroundColor Cyan
        Write-Host "  $OBS_EXE_PATH" -ForegroundColor White
    } else {
        Write-Host "提示：重启 OBS Studio 以加载插件" -ForegroundColor Cyan
    }
} catch {
    Write-Host "✗ 部署失败：$_" -ForegroundColor Red
    if ($ObsPluginDir -eq $OBS_INSTALLED_PATH) {
        Write-Host ""
        Write-Host "提示：部署到系统目录需要管理员权限" -ForegroundColor Yellow
        Write-Host "  请右键选择 '以管理员身份运行 PowerShell' 后重试" -ForegroundColor Yellow
        Write-Host "  或者修改脚本使用用户插件目录（无需管理员）" -ForegroundColor Yellow
    }
    exit 1
}
