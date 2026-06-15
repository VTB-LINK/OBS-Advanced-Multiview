# OBS Advanced Multiview - 开发环境配置文档

本目录包含开发环境配置的所有相关文档和脚本。

## 📚 文档索引

### 主要文档

- **[SETUP.md](SETUP.md)** — 完整的开发环境配置指南
  - Visual Studio 安装步骤
  - CMake 配置流程
  - 项目构建说明
  - VS Code 调试配置

- **[TROUBLESHOOTING.md](TROUBLESHOOTING.md)** — 常见问题排查指南
  - CMake 配置失败
  - Visual Studio 工具链问题
  - PATH 环境变量配置

- **[VS-VERSION-COMPATIBILITY.md](VS-VERSION-COMPATIBILITY.md)** — Visual Studio 版本兼容性说明
  - VS 2022 vs VS 2026 差异
  - 如何在版本间切换
  - CMakePresets.json 配置说明

- **[DISTRIBUTION.md](DISTRIBUTION.md)** — 插件分发指南
  - 构建产物说明
  - 分发包准备
  - 部署和安装方式

### 配置脚本

所有脚本均应在**项目根目录**运行（会自动处理路径）：

#### 主要脚本

- **[deploy-plugin.ps1](deploy-plugin.ps1)** — 插件部署脚本
  ```powershell
  # 从项目根目录运行
  .\docs\setup\deploy-plugin.ps1              # 部署 Debug 版本
  .\docs\setup\deploy-plugin.ps1 RelWithDebInfo  # 部署 RelWithDebInfo 版本
  ```

- **[configure-cmake.ps1](configure-cmake.ps1)** — CMake 配置脚本（需在 Developer PowerShell for VS 中运行）
  ```powershell
  # 从项目根目录运行
  .\docs\setup\configure-cmake.ps1
  ```

#### 辅助脚本

- **[setup-cmake-path-simple.ps1](setup-cmake-path-simple.ps1)** — 简化的 CMake PATH 配置工具
- **[setup-cmake-path.ps1](setup-cmake-path.ps1)** — 高级 CMake PATH 配置工具（自动搜索）

## 🚀 快速开始

### 1. 首次配置

1. 安装 Visual Studio 2022/2026（参考 [SETUP.md](SETUP.md)）
2. 配置 CMake PATH（在 Developer PowerShell 中运行）：
   ```powershell
   cd 项目根目录
   .\docs\setup\setup-cmake-path-simple.ps1
   ```
3. 重启 PowerShell 和 VS Code

### 2. 构建项目

```powershell
# 配置项目（首次运行，下载约 800 MB 依赖）
cmake --preset windows-x64

# 构建 Debug 版本
cmake --build build_x64 --config Debug

# 构建 RelWithDebInfo 版本
cmake --build build_x64 --config RelWithDebInfo
```

### 3. 部署和测试

```powershell
# 部署到 OBS
.\docs\setup\deploy-plugin.ps1

# 或在 VS Code 中按 F5 启动调试（会自动部署）
```

## 📋 环境要求

- **操作系统**: Windows 10/11
- **Visual Studio**: 2022 或 2026 (Community/Professional/Enterprise)
  - 工作负载：使用 C++ 的桌面开发
  - 必需组件：MSVC v143（VS 2022）或 v180（VS 2026）、Windows SDK、CMake 工具、Clang
  - ⚠️ **版本切换说明**：查看下方"工具链信息"章节
- **CMake**: 3.28 或更高（随 VS 安装）
- **Git**: 任何版本
- **OBS Studio**: 31.1.1 或更高（用于测试）

## 🔧 工具链信息

### ⚠️ Visual Studio 版本兼容性

**当前配置**使用 Visual Studio 2026：
- CMakePresets.json 第 59 行：`"generator": "Visual Studio 18 2026"`
- MSVC 工具集：v180+（v19.51+）
- Developer PowerShell：使用 "Developer PowerShell for VS 2026"

**如果使用 Visual Studio 2022**，需要修改 [CMakePresets.json](../../CMakePresets.json) 第 59 行：
```json
"generator": "Visual Studio 17 2022",  // 从 "Visual Studio 18 2026" 改为此
```
并使用对应的工具：
- MSVC 工具集：v143
- Developer PowerShell：使用 "Developer PowerShell for VS 2022"
- 文档中提到的 "VS 2026" 对应换成 "VS 2022"

### 系统要求

- Windows SDK: 10.0.22621.0+
- CMake: 3.28+（4.2.3+ 推荐）
- OBS Studio: 31.1.1+

## 📦 构建产物位置

构建完成后，插件文件位于以下目录：

### Debug 版本（用于调试）
```
build_x64\Debug\
  ├── obs-advanced-multiview.dll    (54 KB)  ← 插件主文件
  ├── obs-advanced-multiview.pdb    (884 KB) ← 调试符号文件
  └── plugin-support.pdb            (60 KB)  ← 支持库符号
```

### RelWithDebInfo 版本（日常开发/分发）
```
build_x64\RelWithDebInfo\
  ├── obs-advanced-multiview.dll    (12 KB)  ← 插件主文件（优化版）
  ├── obs-advanced-multiview.pdb    (476 KB) ← 调试符号文件
  └── plugin-support.pdb            (60 KB)  ← 支持库符号
```

### 手动部署或分发

**最小分发包**（仅需 DLL）：
- `build_x64\RelWithDebInfo\obs-advanced-multiview.dll`（12 KB）
- 复制到 OBS 的 `obs-plugins\64bit\` 目录

**带调试符号的分发包**（便于用户反馈崩溃信息）：
- `obs-advanced-multiview.dll`（12 KB）
- `obs-advanced-multiview.pdb`（476 KB）

**手动部署步骤**：
```powershell
# 复制到 OBS Portable
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" `
          "C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\"

# 或复制到已安装的 OBS（需管理员权限）
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" `
          "C:\Program Files\obs-studio\obs-plugins\64bit\"
```

## 📖 相关文档

- [../phase-1-development-breakdown.md](../phase-1-development-breakdown.md) — 第一阶段开发任务分解
- [../ui-ascii-wireframes.md](../ui-ascii-wireframes.md) — UI 设计线框图
- [../ROADMAP.md](../ROADMAP.md) — 完整项目开发计划
- [../../README.md](../../README.md) — 项目主 README

## ⚠️ 重要提示

1. **所有脚本应从项目根目录运行**（脚本会自动处理相对路径）
2. **首次配置需要在 Developer PowerShell for VS 中运行** `configure-cmake.ps1`
3. **构建产物不会提交到 Git**（已在 .gitignore 中排除）
4. **部署脚本会自动读取 buildspec.json 确定插件名称**

## 💡 提示

- 修改代码后编译测试的最快方式：
  ```powershell
  cmake --build build_x64 --config Debug && .\docs\setup\deploy-plugin.ps1
  ```
  
- 使用 VS Code 的 F5 调试功能可以自动部署并启动 OBS

- 如果遇到问题，查看 [TROUBLESHOOTING.md](TROUBLESHOOTING.md)

---

**上次更新**: 2026年5月17日  
**维护者**: GitHub Copilot
