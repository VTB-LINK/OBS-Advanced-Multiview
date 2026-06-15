# Visual Studio 版本兼容性说明

## 当前配置

本项目当前配置为 **Visual Studio 2022**（为兼容 GitHub Actions CI/CD）。

## 版本对照表

| Visual Studio 版本 | CMakePresets.json Generator | MSVC 工具集 | Developer PowerShell |
|-------------------|----------------------------|------------|---------------------|
| **VS 2022** (当前) | `"Visual Studio 17 2022"` | v143 (v14.3x) | Developer PowerShell for VS 2022 |
| **VS 2026** | `"Visual Studio 18 2026"` | v180+ (v19.51+) | Developer PowerShell for VS 2026 |

## 如何切换到 VS 2026

如果你本地使用 Visual Studio 2026，需要进行以下修改：

### 1. 修改 CMakePresets.json

编辑项目根目录的 [CMakePresets.json](../../CMakePresets.json) 文件：

**第 59 行**（windows-x64 预设中的 generator）：
```json
// 从：
"generator": "Visual Studio 17 2022",

// 改为：
"generator": "Visual Studio 18 2026",
```

### 2. 确认 Visual Studio 组件

打开 **Visual Studio Installer**，确认已安装：
- ✅ MSVC v180+ - VS 2026 C++ x64/x86 生成工具（最新版本）
- ✅ Windows 11 SDK (10.0.22621.0 或更高)
- ✅ 用于 Windows 的 C++ CMake 工具
- ✅ C++ Clang 编译器

### 3. 使用对应的 Developer PowerShell

在 **Developer PowerShell for VS 2026** 中运行：
```powershell
cd 项目根目录

# 清理旧的构建缓存（如果之前用 VS 2022 配置过）
Remove-Item build_x64 -Recurse -Force -ErrorAction SilentlyContinue

# 重新配置
.\docs\setup\configure-cmake.ps1

# 构建
cmake --build build_x64 --config Debug
```

### 4. 更新文档引用

文档中提到的 "VS 2022" 对应改为 "VS 2026"。

## 验证配置

在 Developer PowerShell 中运行以下命令验证环境：

```powershell
# 检查 Visual Studio 安装路径
$env:VSINSTALLDIR
# VS 2022: C:\Program Files\Microsoft Visual Studio\2022\Community\
# VS 2026: C:\Program Files\Microsoft Visual Studio\2026\Preview\ (或 Community\)

# 检查 MSVC 工具集版本
$env:VCToolsVersion
# VS 2022: 14.3x.xxxxx（v143）
# VS 2026: 14.5x.xxxxx（v180+）

# 检查 CMake 版本
cmake --version
# 应显示 3.28.x 或更高
```

## 常见问题

### CMake 报错：找不到 Visual Studio

**症状**：`cmake --preset windows-x64` 报错找不到指定版本的 Visual Studio。

**原因**：CMakePresets.json 中配置的 generator 版本与实际安装的 VS 版本不匹配。

**解决**：按上述步骤 1 修改 CMakePresets.json 中的 generator。

### MSBuild 报错：找不到 v143 或 v180 平台工具集

**症状**：构建时报错 `MSB8020: The build tools for vXXX ... cannot be found`。

**原因**：未安装对应版本的 MSVC 工具集。

**解决**：
1. 打开 Visual Studio Installer
2. 点击"修改"
3. 勾选对应的 MSVC 工具集（v143 或 v180）
4. 等待安装完成
5. 重新运行构建

## 相关文档

- [SETUP.md](SETUP.md) - 完整的开发环境配置指南
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) - 常见问题排查
- [../../README.md](../../README.md) - 快速开始指南
