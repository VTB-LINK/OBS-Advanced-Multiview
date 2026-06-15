# OBS Advanced Multiview - 开发工作流指南

本文档说明完整的开发工作流程，确保代码质量和正确的构建流程。

## 📋 目录

- [开发环境准备](#开发环境准备)
- [日常开发流程](#日常开发流程)
- [构建配置说明](#构建配置说明)
- [提交前检查清单](#提交前检查清单)
- [发布流程](#发布流程)

---

## 开发环境准备

### 首次配置

1. **安装依赖**（详见 [setup/SETUP.md](setup/SETUP.md)）
   - Visual Studio 2022 或 2026
   - CMake 3.28+
   - OBS Studio 31.1.1+

2. **配置项目**
   ```powershell
   # 在 Developer PowerShell for VS 中运行
   cmake --preset windows-x64
   ```

3. **验证构建**
   ```powershell
   cmake --build build_x64 --config Debug
   ```

### VS Code 集成

按 **F5** 即可自动执行：
1. 构建 Debug 版本
2. 部署到 OBS
3. 启动 OBS 并附加调试器

---

## 日常开发流程

### 1. 功能开发阶段

```powershell
# 使用 Debug 配置进行开发
cmake --build build_x64 --config Debug

# 部署到 OBS
.\docs\setup\deploy-plugin.ps1

# 或在 VS Code 中按 F5 启动调试
```

**Debug 构建特点**：
- 包含完整调试符号（.pdb）
- 未优化，便于断点调试
- DLL 大小约 54 KB

### 2. 本地测试阶段

功能开发完成后，**必须**测试 RelWithDebInfo 版本：

```powershell
# 构建 RelWithDebInfo 版本
cmake --build build_x64 --config RelWithDebInfo

# 部署测试
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo

# 启动 OBS 验证功能
C:\Downloads\OBS-Studio-31.1.1-Windows-x64\bin\64bit\obs64.exe
```

**RelWithDebInfo 特点**：
- 优化编译（接近生产性能）
- 仍包含调试符号（便于崩溃分析）
- DLL 大小约 12 KB
- **推荐用于日常开发和分发**

### 3. 代码质量检查

```powershell
# 运行 Clang-Format 格式化代码
# 在 VS Code 中：Alt+Shift+F 或保存时自动格式化

# 检查构建警告
cmake --build build_x64 --config RelWithDebInfo 2>&1 | Select-String "warning"
```

---

## 构建配置说明

### 配置对比

| 配置 | 用途 | DLL 大小 | 优化 | 调试符号 | 使用场景 |
|------|------|----------|------|----------|----------|
| **Debug** | 开发调试 | 54 KB | ❌ | ✅ | 断点调试、问题排查 |
| **RelWithDebInfo** | 日常开发/分发 | 12 KB | ✅ | ✅ | 性能测试、用户分发 |
| **Release** | 生产发布 | ~10 KB | ✅ | ❌ | 最终发布版本 |

### 何时使用哪个配置

#### Debug - 开发阶段
```powershell
cmake --build build_x64 --config Debug
```
**使用时机**：
- ✅ 编写新功能时
- ✅ 调试复杂问题
- ✅ 需要详细堆栈信息
- ❌ 不推荐分发给用户（性能差）

#### RelWithDebInfo - 验证阶段（推荐）
```powershell
cmake --build build_x64 --config RelWithDebInfo
```
**使用时机**：
- ✅ 功能开发完成后的验证
- ✅ 性能测试
- ✅ 用户反馈的问题复现
- ✅ **分发给测试用户**
- ✅ 提交代码前的最终测试

#### Release - 发布阶段
```powershell
cmake --build build_x64 --config Release
```
**使用时机**：
- ✅ 正式版本发布
- ✅ 公开分发
- ❌ 仅在发布时构建，日常开发用 RelWithDebInfo

---

## 提交前检查清单

在提交代码前，**必须**完成以下步骤：

### ✅ 完整检查流程

```powershell
# 1. 清理并重新构建所有配置
Remove-Item build_x64\CMakeCache.txt -ErrorAction SilentlyContinue
cmake --preset windows-x64

# 2. 构建 Debug 版本
cmake --build build_x64 --config Debug
if ($LASTEXITCODE -ne 0) {
    Write-Host "✗ Debug 构建失败！" -ForegroundColor Red
    exit 1
}

# 3. 构建 RelWithDebInfo 版本
cmake --build build_x64 --config RelWithDebInfo
if ($LASTEXITCODE -ne 0) {
    Write-Host "✗ RelWithDebInfo 构建失败！" -ForegroundColor Red
    exit 1
}

# 4. 部署并测试 RelWithDebInfo
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
# 手动启动 OBS 并验证所有功能

# 5. 检查 Git 状态
git status
# 确保没有意外的文件被修改或添加

# 6. 提交代码
git add .
git commit -m "功能描述"
git push origin master
```

### 📋 检查清单（人工确认）

在提交前确认：
- [ ] Debug 和 RelWithDebInfo 都能成功构建
- [ ] 在 OBS 中测试了所有修改的功能
- [ ] 没有编译警告（或已知警告已记录）
- [ ] 代码已格式化（Clang-Format）
- [ ] 没有调试用的 `printf`/`cout` 或临时代码
- [ ] .gitignore 正确排除了构建产物
- [ ] Commit message 清晰描述了修改内容

---

## 发布流程

### 准备发布版本

```powershell
# 1. 确保所有代码已提交
git status  # 应该是干净的

# 2. 更新版本号
# 编辑 buildspec.json 中的 version 字段

# 3. 构建 Release 版本
cmake --build build_x64 --config Release

# 4. 构建 RelWithDebInfo 版本（带符号）
cmake --build build_x64 --config RelWithDebInfo

# 5. 创建分发包（详见下文）
```

### 创建分发包

参考 [setup/DISTRIBUTION.md](setup/DISTRIBUTION.md) 创建分发包：

```powershell
$version = "1.0.0"  # 修改为实际版本号

# 最小分发包（仅 DLL，12 KB）
New-Item -Path "dist" -ItemType Directory -Force
Copy-Item "build_x64\Release\obs-advanced-multiview.dll" "dist\"
Compress-Archive -Path "dist\*" -DestinationPath "OBS-Advanced-Multiview-v$version.zip" -Force

# 带调试符号的分发包（推荐）
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.dll" "dist\"
Copy-Item "build_x64\RelWithDebInfo\obs-advanced-multiview.pdb" "dist\"
Compress-Archive -Path "dist\*" -DestinationPath "OBS-Advanced-Multiview-v$version-with-symbols.zip" -Force
```

### GitHub Release

```powershell
# 1. 创建 Git 标签
git tag v$version
git push origin v$version

# 2. 在 GitHub 上创建 Release
# - 上传 OBS-Advanced-Multiview-v$version.zip
# - 上传 OBS-Advanced-Multiview-v$version-with-symbols.zip
# - 填写 Release Notes
```

---

## 常见开发场景

### 场景 1：添加新功能

```powershell
# 1. 创建功能分支（可选）
git checkout -b feature/new-feature

# 2. 使用 Debug 配置开发
cmake --build build_x64 --config Debug
# 在 VS Code 中按 F5 调试

# 3. 功能完成后，测试 RelWithDebInfo
cmake --build build_x64 --config RelWithDebInfo
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo

# 4. 确认无问题后提交
git add .
git commit -m "功能：添加新功能描述"
git push origin feature/new-feature
```

### 场景 2：修复 Bug

```powershell
# 1. 复现问题（使用 Debug 或 RelWithDebInfo）
cmake --build build_x64 --config Debug
.\docs\setup\deploy-plugin.ps1

# 2. 使用断点定位问题（VS Code F5）

# 3. 修复后验证
cmake --build build_x64 --config RelWithDebInfo
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo

# 4. 提交修复
git commit -m "修复：问题描述和解决方案"
```

### 场景 3：性能优化

```powershell
# ⚠️ 性能优化必须使用 RelWithDebInfo 或 Release
cmake --build build_x64 --config RelWithDebInfo

# 使用性能分析工具（VS Profiler 或其他）
# 不要基于 Debug 版本做性能判断！
```

---

## 快速参考

### 常用命令

```powershell
# 配置项目
cmake --preset windows-x64

# 构建 Debug
cmake --build build_x64 --config Debug

# 构建 RelWithDebInfo（推荐日常使用）
cmake --build build_x64 --config RelWithDebInfo

# 构建 Release（仅发布时）
cmake --build build_x64 --config Release

# 部署到 OBS
.\docs\setup\deploy-plugin.ps1              # Debug
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo  # RelWithDebInfo

# 清理重建
Remove-Item build_x64 -Recurse -Force
cmake --preset windows-x64
cmake --build build_x64 --config RelWithDebInfo
```

### 目录结构

```
build_x64/
  ├── Debug/                     # Debug 构建产物
  │   └── obs-advanced-multiview.dll (+ PDB)
  ├── RelWithDebInfo/            # RelWithDebInfo 构建产物（推荐分发）
  │   └── obs-advanced-multiview.dll (+ PDB)
  └── Release/                   # Release 构建产物
      └── obs-advanced-multiview.dll
```

---

## 相关文档

- [开发环境配置](setup/README.md) - 首次配置指南
- [故障排除](setup/TROUBLESHOOTING.md) - 常见问题解决
- [插件分发](setup/DISTRIBUTION.md) - 分发包创建方法
- [VS 版本兼容性](setup/VS-VERSION-COMPATIBILITY.md) - VS 2022/2026 切换
- [项目计划](ROADMAP.md) - 功能规划和里程碑

---

## 给 AI Agents 的提示

**当用户说"功能开发完成"或"准备提交"时**，请引导执行：

1. **构建验证**：
   ```powershell
   cmake --build build_x64 --config Debug
   cmake --build build_x64 --config RelWithDebInfo
   ```

2. **部署测试**：
   ```powershell
   .\docs\setup\deploy-plugin.ps1 RelWithDebInfo
   ```

3. **提交前检查**：
   - 检查所有构建配置是否成功
   - 确认插件在 OBS 中正常加载
   - 验证 git status 干净

4. **发布流程**（如果是发布版本）：
   - 构建 Release 配置
   - 创建分发包（参考 DISTRIBUTION.md）
   - 创建 Git tag 和 GitHub Release
