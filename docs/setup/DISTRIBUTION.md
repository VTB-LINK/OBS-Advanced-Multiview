# OBS Advanced Multiview - 插件分发指南

本文档说明如何准备插件进行分发，包括构建产物位置、文件说明和部署方式。

## 📦 构建产物位置

### 构建版本对比

| 构建类型 | DLL 大小 | PDB 大小 | 推荐用途 |
|---------|---------|---------|---------|
| **Debug** | 54 KB | 884 KB | 开发调试（包含调试符号） |
| **RelWithDebInfo** | 12 KB | 476 KB | 日常开发和分发（优化+符号） |
| **Release** | ~10 KB | — | 生产发布（完全优化，无符号） |

### 文件位置

#### Debug 版本
```
build_x64\Debug\
  ├── obs-advanced-multiview.dll    (54 KB)  ← 插件主文件
  ├── obs-advanced-multiview.pdb    (884 KB) ← 调试符号文件
  └── plugin-support.pdb            (60 KB)  ← 支持库符号（编译时使用）
```

#### RelWithDebInfo 版本（推荐分发）
```
build_x64\RelWithDebInfo\
  ├── obs-advanced-multiview.dll    (12 KB)  ← 插件主文件（优化版）
  ├── obs-advanced-multiview.pdb    (476 KB) ← 调试符号文件
  └── plugin-support.pdb            (60 KB)  ← 支持库符号（编译时使用）
```

## 📋 分发包准备

### 方案一：最小分发包（推荐）

**文件**：
- `obs-advanced-multiview.dll` (12 KB)

**优点**：
- 文件小，下载快
- 适合大多数用户

**缺点**：
- 用户遇到崩溃时无法提供详细错误信息

**打包命令**：
```powershell
# 创建分发目录
New-Item -Path "dist" -ItemType Directory -Force

# 复制 DLL
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" "dist\"

# 创建 ZIP（需要安装 7-Zip 或使用 Windows 内置压缩）
Compress-Archive -Path "dist\*" -DestinationPath "OBS-Advanced-Multiview-v1.0.0.zip" -Force
```

### 方案二：带调试符号的分发包

**文件**：
- `obs-advanced-multiview.dll` (12 KB)
- `obs-advanced-multiview.pdb` (476 KB)

**优点**：
- 用户崩溃时可以提供详细堆栈跟踪
- 便于收集错误报告和调试

**缺点**：
- 文件稍大（总共约 488 KB）

**打包命令**：
```powershell
New-Item -Path "dist" -ItemType Directory -Force
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" "dist\"
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.pdb" "dist\"
Compress-Archive -Path "dist\*" -DestinationPath "OBS-Advanced-Multiview-v1.0.0-with-symbols.zip" -Force
```

### 方案三：完整分发包（包含文档）

**文件结构**：
```
OBS-Advanced-Multiview-v1.0.0\
  ├── obs-advanced-multiview.dll        ← 插件文件
  ├── README.txt                        ← 安装说明
  ├── LICENSE.txt                       ← 许可证
  └── symbols\                          ← 可选：调试符号
      └── obs-advanced-multiview.pdb
```

**创建脚本**：
```powershell
$version = "1.0.0"
$distDir = "dist\OBS-Advanced-Multiview-v$version"

# 创建目录结构
New-Item -Path "$distDir" -ItemType Directory -Force
New-Item -Path "$distDir\symbols" -ItemType Directory -Force

# 复制文件
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" "$distDir\"
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.pdb" "$distDir\symbols\"
Copy-Item "LICENSE" "$distDir\LICENSE.txt"

# 创建 README.txt
@"
OBS Advanced Multiview Plugin v$version

安装说明：
1. 关闭 OBS Studio
2. 将 obs-advanced-multiview.dll 复制到以下目录之一：
   - OBS Portable: OBS-Studio\obs-plugins\64bit\
   - OBS 已安装版: C:\Program Files\obs-studio\obs-plugins\64bit\
3. 重新启动 OBS Studio
4. 插件应自动加载（查看 OBS 日志确认）

故障排除：
- 如果插件未加载，检查 OBS 版本是否为 31.1.1 或更高
- 查看 OBS 日志文件：%APPDATA%\obs-studio\logs\
- 反馈问题：https://github.com/VTB-LINK/OBS-Advanced-Multiview/issues

许可证：详见 LICENSE.txt
"@ | Out-File "$distDir\README.txt" -Encoding UTF8

# 打包
Compress-Archive -Path "$distDir" -DestinationPath "OBS-Advanced-Multiview-v$version-full.zip" -Force

Write-Host "✓ 分发包已创建：OBS-Advanced-Multiview-v$version-full.zip" -ForegroundColor Green
```

## 🚀 部署方式

### 用户手动部署

**OBS Portable 版本**：
```powershell
# 复制到 OBS Portable 目录
Copy-Item "obs-advanced-multiview.dll" `
          "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\"
```

**OBS 已安装版本**（需管理员权限）：
```powershell
# 以管理员身份运行 PowerShell
Copy-Item "obs-advanced-multiview.dll" `
          "C:\Program Files\obs-studio\obs-plugins\64bit\"
```

**通过 OBS Plugins 文件夹**（推荐）：
```powershell
# 复制到用户插件目录（无需管理员权限）
$pluginDir = "$env:APPDATA\obs-studio\obs-plugins\64bit"
New-Item -Path $pluginDir -ItemType Directory -Force
Copy-Item "obs-advanced-multiview.dll" $pluginDir
```

### 自动部署脚本

创建用户友好的安装脚本 `install.ps1`：

```powershell
Write-Host "=== OBS Advanced Multiview 插件安装器 ===" -ForegroundColor Cyan
Write-Host ""

# 检测 OBS 安装路径
$obsInstalled = "C:\Program Files\obs-studio\obs-plugins\64bit"
$obsPortable = "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit"
$obsUser = "$env:APPDATA\obs-studio\obs-plugins\64bit"

# 检查当前目录中的 DLL
$dllPath = ".\obs-advanced-multiview.dll"
if (-not (Test-Path $dllPath)) {
    Write-Host "✗ 错误：找不到 obs-advanced-multiview.dll" -ForegroundColor Red
    Write-Host "  请确保在正确的目录中运行此脚本。" -ForegroundColor Yellow
    pause
    exit 1
}

Write-Host "请选择安装位置：" -ForegroundColor Yellow
Write-Host "1. 用户插件目录（推荐，无需管理员权限）"
Write-Host "   $obsUser"
Write-Host "2. OBS 已安装版本（需要管理员权限）"
Write-Host "   $obsInstalled"
Write-Host "3. OBS Portable 版本"
Write-Host "   $obsPortable"
Write-Host ""

$choice = Read-Host "请输入选项 [1-3]"

switch ($choice) {
    "1" {
        $targetDir = $obsUser
        New-Item -Path $targetDir -ItemType Directory -Force | Out-Null
    }
    "2" {
        $targetDir = $obsInstalled
    }
    "3" {
        $targetDir = $obsPortable
    }
    default {
        Write-Host "✗ 无效选项" -ForegroundColor Red
        pause
        exit 1
    }
}

if (-not (Test-Path $targetDir)) {
    Write-Host "✗ 目标目录不存在：$targetDir" -ForegroundColor Red
    Write-Host "  请确认 OBS Studio 已正确安装。" -ForegroundColor Yellow
    pause
    exit 1
}

try {
    Copy-Item $dllPath $targetDir -Force
    Write-Host ""
    Write-Host "✓ 插件安装成功！" -ForegroundColor Green
    Write-Host "  位置：$targetDir" -ForegroundColor Gray
    Write-Host ""
    Write-Host "下一步：" -ForegroundColor Cyan
    Write-Host "1. 重启 OBS Studio" -ForegroundColor White
    Write-Host "2. 插件会自动加载" -ForegroundColor White
    Write-Host "3. 查看 OBS 日志确认加载状态" -ForegroundColor White
} catch {
    Write-Host "✗ 安装失败：$_" -ForegroundColor Red
    if ($choice -eq "2") {
        Write-Host "  提示：安装到系统目录需要管理员权限" -ForegroundColor Yellow
        Write-Host "  请右键选择"以管理员身份运行 PowerShell"后重试" -ForegroundColor Yellow
    }
    pause
    exit 1
}

pause
```

## ✅ 验证清单

分发前检查：
- [ ] 使用 RelWithDebInfo 或 Release 配置构建
- [ ] DLL 文件大小正常（约 12 KB）
- [ ] 在测试 OBS 环境中验证插件加载
- [ ] 包含 README.txt 安装说明
- [ ] 包含 LICENSE.txt 许可证文件
- [ ] 版本号正确（文件名、README 等）
- [ ] 创建 GitHub Release 并上传

## 📊 发布渠道

### GitHub Releases（推荐）

```bash
# 打标签
git tag v1.0.0
git push origin v1.0.0

# 在 GitHub 上创建 Release
# 上传：OBS-Advanced-Multiview-v1.0.0.zip
```

### OBS Resources/Project

将插件提交到 OBS Resources：
- 官网：https://obsproject.com/forum/resources/
- 需要注册账号并填写插件信息

## 相关文档

- [README.md](README.md) - 开发环境快速开始
- [SETUP.md](SETUP.md) - 完整配置指南
- [../../plan.md](../../plan.md) - 项目开发计划
