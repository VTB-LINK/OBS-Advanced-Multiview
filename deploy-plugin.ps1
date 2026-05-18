# 部署插件到 OBS portable 版本
# 使用方法：
#   .\deploy-plugin.ps1              # 部署 Debug 版本
#   .\deploy-plugin.ps1 RelWithDebInfo  # 部署 RelWithDebInfo 版本

param(
    [string]$BuildConfig = "Debug"
)

# 从 buildspec.json 读取插件名称
$BuildSpecPath = "$PSScriptRoot\buildspec.json"
if (Test-Path $BuildSpecPath) {
    $BuildSpec = Get-Content $BuildSpecPath | ConvertFrom-Json
    $PluginName = $BuildSpec.name
}
else {
    Write-Host "✗ buildspec.json not found" -ForegroundColor Red
    exit 1
}

$PluginDll = "$PSScriptRoot\build_x64\$BuildConfig\$PluginName.dll"
$ObsPluginDir = "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\"

if (-not (Test-Path $PluginDll)) {
    Write-Host "✗ Plugin DLL not found: $PluginDll" -ForegroundColor Red
    Write-Host "  Please build the plugin first:" -ForegroundColor Yellow
    Write-Host "    cmake --build build_x64 --config $BuildConfig" -ForegroundColor Yellow
    exit 1
}

if (-not (Test-Path $ObsPluginDir)) {
    Write-Host "✗ OBS plugin directory not found: $ObsPluginDir" -ForegroundColor Red
    exit 1
}

try {
    Copy-Item $PluginDll -Destination $ObsPluginDir -Force
    Write-Host "✓ Plugin DLL deployed successfully" -ForegroundColor Green
    Write-Host "  From: $PluginDll" -ForegroundColor Gray
    Write-Host "  To:   $ObsPluginDir" -ForegroundColor Gray

    # Deploy data files (locale etc.)
    $DataSource = "$PSScriptRoot\build_x64\rundir\$BuildConfig\$PluginName"
    $ObsDataDir = "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\data\obs-plugins\$PluginName"
    if (Test-Path $DataSource) {
        if (-not (Test-Path $ObsDataDir)) {
            New-Item -Path $ObsDataDir -ItemType Directory -Force | Out-Null
        }
        Copy-Item "$DataSource\*" -Destination $ObsDataDir -Recurse -Force
        Write-Host "✓ Plugin data deployed successfully" -ForegroundColor Green
        Write-Host "  From: $DataSource" -ForegroundColor Gray
        Write-Host "  To:   $ObsDataDir" -ForegroundColor Gray
    }
    else {
        Write-Host "⚠ Plugin data directory not found, skipping data deploy" -ForegroundColor Yellow
    }

    Write-Host ""
    Write-Host "You can now start OBS to test the plugin:" -ForegroundColor Cyan
    Write-Host "  C:\Downloads\OBS-Studio-31.1.1-Windows-x64\bin\64bit\obs64.exe" -ForegroundColor Cyan
}
catch {
    Write-Host "✗ Failed to deploy plugin: $_" -ForegroundColor Red
    exit 1
}
