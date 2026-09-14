# CMake 配置失败 - 找不到 VS 2022 工具链

## 问题描述

CMake 报错：`无法找到 Visual Studio 2022 的生成工具（平台工具集="v143"）`

错误原因：CMake 检测到了错误的 Visual Studio 版本（VS 18 而非 VS 2022）或无法找到 MSVC v143 构建工具。

## 解决方案

### 方案一：使用 Developer PowerShell（推荐）⭐

> ⚠️ **版本说明**：根据你安装的 Visual Studio 版本选择对应的 Developer PowerShell
> - **VS 2022**：搜索 "Developer PowerShell for VS 2022"
> - **VS 2026**：搜索 "Developer PowerShell for VS 2026"（当前项目配置）

1. **打开 Developer PowerShell**
   - 按 Win 键，搜索对应版本的 Developer PowerShell
   - 或者从开始菜单 → Visual Studio 文件夹中找到

2. **切换到项目目录**
   ```powershell
   cd C:\Users\oldking139\Documents\Repos\Github\OBS-Advanced-Multiview
   ```

3. **运行配置脚本**
   ```powershell
   .\docs\setup\configure-cmake.ps1
   ```

4. **等待下载完成**（约 5-15 分钟，下载 800 MB 依赖）

5. **如果成功**，继续构建：
   ```powershell
   cmake --build build_x64 --config Debug
   ```

### 方案二：检查 Visual Studio 组件安装

1. **打开 Visual Studio Installer**
   - Win 键搜索 "Visual Studio Installer"

2. **点击 "修改" 你安装的 Visual Studio 版本**

3. **确认已安装以下组件**：
   - ✅ **MSVC C++ x64/x86 生成工具（最新版本）** ← 这是关键！
     - VS 2022：MSVC v143
     - VS 2026：MSVC v180+
   - ✅ Windows 11 SDK (10.0.22621.0 或更高)
   - ✅ 用于 Windows 的 C++ CMake 工具
   - ✅ 适用于 Windows 的 C++ Clang 工具

4. **如果未勾选 MSVC 工具集**：
   - 勾选后点击 "修改"
   - 等待安装完成
   - 在对应版本的 Developer PowerShell 中重新运行 `.\docs\setup\configure-cmake.ps1`

5. **版本不匹配问题**：
   - 如果安装的是 VS 2022，但 [CMakePresets.json](../../CMakePresets.json) 配置的是 VS 2026
   - 修改 CMakePresets.json 第 59 行为：`"generator": "Visual Studio 17 2022"`

### 方案三：清理并重新配置

如果之前的配置失败、或某次构建是在临时 git worktree 里做的，会在**依赖缓存**里留下错误的绝对路径。典型症状：重配置时在 `_setup_obs_studio` / `cmake/common/buildspec_common.cmake` 处报
`The current CMakeCache.txt directory ... is different than the directory ... where CMakeCache.txt was created`。
此时只清 `build_x64` 不够——污染在**从源码构建的 obs 依赖**缓存里，必须一并清：

```powershell
# 在对应版本的 Developer PowerShell for VS 中运行
# （提交默认预设为 VS 2022；本地若覆盖为 VS 2026，则用 VS 2026 的 Developer PowerShell）：
Remove-Item .deps\obs-studio-31.1.1\build_x64 -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item build_x64 -Recurse -Force -ErrorAction SilentlyContinue
.\docs\setup\configure-cmake.ps1
```

obs 源码已在本地（`.deps/obs-studio-31.1.1/`），不会重新下载，只会重配置 + 重编 obs 依赖（约 10-15 分钟）。

## 验证环境

在 Developer PowerShell 中运行：

```powershell
# 检查是否在正确的环境中
$env:VSINSTALLDIR
# VS 2022 应显示：C:\Program Files\Microsoft Visual Studio\2022\Community\
# VS 2026 应显示：C:\Program Files\Microsoft Visual Studio\2026\Preview\ 或 Community\

# 检查 CMake 版本
cmake --version
# 应该显示：cmake version 3.28.x 或更高

# 检查 MSVC 工具集版本
$env:VCToolsVersion
# VS 2022 应显示：14.3x.xxxxx（v143）
# VS 2026 应显示：14.5x.xxxxx（v180+）

# 检查 MSBuild
where.exe msbuild
# 应该显示对应 VS 版本的
cmake --version
# 应该显示：cmake version 3.28.x 或更高

# 检查 MSBuild
where.exe msbuild
# 应该显示 VS 2022 路径
```

## 如果仍然失败

请提供以下信息：

1. Developer PowerShell for VS 2022 中运行 `$env:VSINSTALLDIR` 的输出
2. Visual Studio Installer 中显示的已安装组件截图
3. `.\docs\setup\configure-cmake.ps1` 的完整错误输出

---

下一步：成功配置后，继续查看 [SETUP.md](SETUP.md) 进行构建和部署。
