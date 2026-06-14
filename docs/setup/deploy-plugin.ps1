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

# OBS 部署目标路径（可同时部署到多个版本）
$OBS_PORTABLE_PATHS = @(
    "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\",
    "C:\Downloads\OBS-Studio-32.1.2-Windows-x64\obs-plugins\64bit\"
)

# 2. 备选：用户插件目录（推荐，无需管理员权限）
$OBS_USER_PLUGIN_PATH = "$env:APPDATA\obs-studio\obs-plugins\64bit"

# 3. 备选：已安装版本（需要管理员权限）
$OBS_INSTALLED_PATH = "C:\Program Files\obs-studio\obs-plugins\64bit"

# OBS 可执行文件路径（用于提示启动命令）
$OBS_EXE_PATH = "C:\Downloads\OBS-Studio-32.1.2-Windows-x64\bin\64bit\obs64.exe"

#endregion

# 获取项目根目录（脚本在 docs/setup，需要往上两级）
$ProjectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

if (-not (Test-Path (Join-Path $ProjectRoot "CMakeLists.txt")) -or
    -not (Test-Path (Join-Path $ProjectRoot "buildspec.json"))) {
    Write-Host "✗ 无法定位项目根目录：$ProjectRoot" -ForegroundColor Red
    Write-Host "请从仓库内的 docs/setup/deploy-plugin.ps1 运行此脚本。" -ForegroundColor Yellow
    exit 1
}

function Get-LocaleKeys {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        return @()
    }

    return Select-String -Path $Path -Pattern '^([^=]+)=' | ForEach-Object { $_.Matches[0].Groups[1].Value }
}

function Test-LocaleFile {
    param([string]$Path)

    $keys = @(Get-LocaleKeys $Path)
    $duplicates = $keys | Group-Object | Where-Object { $_.Count -gt 1 }
    if ($duplicates.Count -gt 0) {
        Write-Host "✗ locale 文件存在重复 key：$Path" -ForegroundColor Red
        foreach ($d in $duplicates) {
            Write-Host "  $($d.Name) ($($d.Count)x)" -ForegroundColor Yellow
        }
        return $false
    }

    return $true
}

function Test-LocaleParity {
    param([string]$LocaleDir)

    if (-not (Test-Path $LocaleDir)) {
        return $true
    }

    $enPath = Join-Path $LocaleDir "en-US.ini"
    if (-not (Test-Path $enPath)) {
        Write-Host "✗ 缺少基准 locale 文件：$enPath" -ForegroundColor Red
        return $false
    }

    if (-not (Test-LocaleFile $enPath)) {
        return $false
    }

    $enKeys = @(Get-LocaleKeys $enPath | Sort-Object -Unique)
    foreach ($localeFile in Get-ChildItem $LocaleDir -Filter "*.ini") {
        if ($localeFile.Name -eq "en-US.ini") {
            continue
        }

        if (-not (Test-LocaleFile $localeFile.FullName)) {
            return $false
        }

        $otherKeys = @(Get-LocaleKeys $localeFile.FullName | Sort-Object -Unique)
        $missing = @(Compare-Object $enKeys $otherKeys | Where-Object { $_.SideIndicator -eq '<=' } | ForEach-Object { $_.InputObject })
        $extra = @(Compare-Object $enKeys $otherKeys | Where-Object { $_.SideIndicator -eq '=>' } | ForEach-Object { $_.InputObject })

        if ($missing.Count -gt 0 -or $extra.Count -gt 0) {
            Write-Host "✗ locale key 集合不一致：$($localeFile.Name)" -ForegroundColor Red
            if ($missing.Count -gt 0) {
                Write-Host "  缺少 key:" -ForegroundColor Yellow
                $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
            }
            if ($extra.Count -gt 0) {
                Write-Host "  多余 key:" -ForegroundColor Yellow
                $extra | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
            }
            return $false
        }
    }

    return $true
}

# 从 buildspec.json 读取插件名称
$BuildSpecPath = Join-Path $ProjectRoot "buildspec.json"
if (Test-Path $BuildSpecPath) {
    $BuildSpec = Get-Content $BuildSpecPath | ConvertFrom-Json
    $PluginName = $BuildSpec.name
}
else {
    Write-Host "✗ buildspec.json not found at: $BuildSpecPath" -ForegroundColor Red
    exit 1
}

# 确定 OBS 插件目录（部署到所有存在的 Portable 路径，回退到用户/系统路径）
$DeployTargets = @()
foreach ($p in $OBS_PORTABLE_PATHS) {
    if (Test-Path $p) {
        $DeployTargets += $p
    }
}
if ($DeployTargets.Count -gt 0) {
    Write-Host "→ 找到 $($DeployTargets.Count) 个 OBS Portable 路径" -ForegroundColor Cyan
}
elseif (Test-Path $OBS_USER_PLUGIN_PATH) {
    $DeployTargets += $OBS_USER_PLUGIN_PATH
    Write-Host "→ 使用用户插件路径" -ForegroundColor Cyan
}
elseif (Test-Path $OBS_INSTALLED_PATH) {
    $DeployTargets += $OBS_INSTALLED_PATH
    Write-Host "→ 使用已安装 OBS 路径（可能需要管理员权限）" -ForegroundColor Yellow
}
else {
    Write-Host "✗ 未找到 OBS 插件目录！" -ForegroundColor Red
    Write-Host ""
    Write-Host "请修改脚本顶部的路径配置" -ForegroundColor Yellow
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

$RepoDataSource = Join-Path $ProjectRoot "data"
$RepoLocaleDir = Join-Path $RepoDataSource "locale"
if (-not (Test-LocaleParity $RepoLocaleDir)) {
    Write-Host "" 
    Write-Host "请修复 data/locale 下的 key 后再部署。" -ForegroundColor Yellow
    exit 1
}

try {
    foreach ($ObsPluginDir in $DeployTargets) {
        # 确保目标目录存在
        if (-not (Test-Path $ObsPluginDir)) {
            Write-Host "→ 创建插件目录：$ObsPluginDir" -ForegroundColor Cyan
            New-Item -Path $ObsPluginDir -ItemType Directory -Force | Out-Null
        }
    
        Copy-Item $PluginDll -Destination $ObsPluginDir -Force
        Write-Host "✓ 插件 DLL 部署成功！" -ForegroundColor Green
        Write-Host "  From: $PluginDll" -ForegroundColor Gray
        Write-Host "  To:   $ObsPluginDir" -ForegroundColor Gray

        # 部署 data 文件（locale 等）。
        #
        # 先复制 CMake 生成的 rundir 数据，再用仓库 data/ 覆盖一次。
        # 这样只修改 locale 文件后，不需要重新构建也可以直接运行本脚本
        # 部署最新翻译；DLL 仍然来自指定构建配置。
        $BuiltDataSource = Join-Path $ProjectRoot "build_x64\rundir\$BuildConfig\$PluginName"
        # OBS data 目录：与 obs-plugins 同级的 data/obs-plugins/<plugin-name>/
        $ObsDataDir = Join-Path (Split-Path (Split-Path $ObsPluginDir)) "data\obs-plugins\$PluginName"
        if ((Test-Path $BuiltDataSource) -or (Test-Path $RepoDataSource)) {
            if (-not (Test-Path $ObsDataDir)) {
                New-Item -Path $ObsDataDir -ItemType Directory -Force | Out-Null
            }

            if (Test-Path $BuiltDataSource) {
                Copy-Item "$BuiltDataSource\*" -Destination $ObsDataDir -Recurse -Force
                Write-Host "✓ 插件构建数据部署成功！" -ForegroundColor Green
                Write-Host "  From: $BuiltDataSource" -ForegroundColor Gray
                Write-Host "  To:   $ObsDataDir" -ForegroundColor Gray
            }

            if (Test-Path $RepoDataSource) {
                Copy-Item "$RepoDataSource\*" -Destination $ObsDataDir -Recurse -Force
                Write-Host "✓ 插件源码数据部署成功！" -ForegroundColor Green
                Write-Host "  From: $RepoDataSource" -ForegroundColor Gray
                Write-Host "  To:   $ObsDataDir" -ForegroundColor Gray
            }

            Write-Host "✓ 插件数据部署成功！" -ForegroundColor Green
            Write-Host "  To:   $ObsDataDir" -ForegroundColor Gray
        }
        else {
            Write-Host "⚠ 未找到插件数据目录，跳过 data 部署" -ForegroundColor Yellow
        }
        Write-Host ""
    }
    
    if (Test-Path $OBS_EXE_PATH) {
        Write-Host "现在可以启动 OBS 测试插件：" -ForegroundColor Cyan
        Write-Host "  $OBS_EXE_PATH" -ForegroundColor White
    }
    else {
        Write-Host "提示：重启 OBS Studio 以加载插件" -ForegroundColor Cyan
    }
}
catch {
    Write-Host "✗ 部署失败：$_" -ForegroundColor Red
    exit 1
}
