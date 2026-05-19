# OBS Advanced Multiview - 开发环境配置指南

本文档记录开发环境的配置步骤和当前状态。

## 📋 当前配置状态

### ✅ 已完成
- [x] 修改 CMakePresets.json，启用 Qt 和 Frontend API 支持
- [x] 创建 VS Code 工作区配置文件（.vscode/）
  - settings.json - CMake 和格式化工具配置
  - launch.json - 调试器配置（附加到 OBS）
  - tasks.json - 自动部署任务
  - extensions.json - 推荐扩展列表
- [x] 创建插件部署脚本（deploy-plugin.ps1）
- [x] 打开 PowerShell Profile 以添加代理配置

### ⏸️ 需要手动完成的步骤

#### 1. 配置 PowerShell Profile（已打开 Notepad）
请在已打开的 Notepad 窗口中添加以下内容，然后保存：

```powershell
# 设置代理环境变量
$env:HTTP_PROXY = "http://127.0.0.1:31333"
$env:HTTPS_PROXY = "http://127.0.0.1:31333"
```

保存后关闭 Notepad，然后在 PowerShell 中运行：
```powershell
. $PROFILE  # 重新加载 profile
```

#### 2. 安装 Visual Studio 2022 Community
**这是接下来最重要的步骤！**

1. 下载地址：https://visualstudio.microsoft.com/zh-hans/downloads/
2. 运行安装程序
3. 选择工作负载："使用 C++ 的桌面开发"
4. 在右侧确认勾选以下组件：
   - ✅ MSVC v143 - VS 2022 C++ x64/x86 生成工具（最新版本）
   - ✅ Windows 11 SDK (10.0.22621.0 或更高)
   - ✅ 用于 Windows 的 C++ CMake 工具
   - ✅ C++ Clang 编译器（用于代码格式化）
5. 安装大小约 6-8 GB，耗时视网络情况而定
6. 安装完成后**重启 PowerShell**

#### 3. 验证 Visual Studio 安装
重启 PowerShell 后运行：
```powershell
cmake --version
# 应该显示：cmake version 3.28.x 或更高
```

如果显示版本号，则安装成功！

#### 4. 安装 VS Code 推荐扩展
VS Code 会自动提示安装推荐扩展，点击"安装全部"即可。

或者手动安装：  
- C/C++ (ms-vscode.cpptools)
- C/C++ Extension Pack (ms-vscode.cpptools-extension-pack)
- CMake Tools (ms-vscode.cmake-tools)
- CMake (twxs.cmake)
- Clang-Format (xaver.clang-format)

---

## 🚀 安装完成后的构建步骤

当 Visual Studio 2022 安装完成后，运行以下命令：

### 1. 配置项目（下载依赖）
```powershell
cmake --preset windows-x64
```
这将下载：
- OBS Studio 31.1.1 源码（约 230 MB）
- 预构建依赖（约 100 MB）
- Qt6 依赖（约 500 MB）
- 总下载时间：5-15 分钟

### 2. 构建项目
```powershell
# 构建 Debug 版本（用于调试）
cmake --build build_x64 --config Debug

# 构建 RelWithDebInfo 版本（日常开发）
cmake --build build_x64 --config RelWithDebInfo
```

### 3. 部署并测试插件
```powershell
# 部署 Debug 版本到 OBS
.\deploy-plugin.ps1

# 启动 OBS 测试
C:\Downloads\OBS-Studio-31.1.1-Windows-x64\bin\64bit\obs64.exe
```

### 4. 使用 VS Code 调试
- 在 VS Code 中打开项目
- 在 src/plugin-main.cpp 或其他 C++ 源文件中设置断点
- 按 F5 启动调试
- OBS 会自动启动并附加调试器

---

## 📁 已创建的文件

```
.vscode/
  ├── settings.json      # CMake 和格式化配置
  ├── launch.json        # 调试器配置
  ├── tasks.json         # 自动部署任务
  └── extensions.json    # 推荐扩展
deploy-plugin.ps1        # 插件部署脚本
CMakePresets.json        # 已修改：启用 Qt 和 Frontend API
```

---

## 🔧 OBS 测试环境

- **OBS 路径**：`C:\Downloads\OBS-Studio-31.1.1-Windows-x64`
- **OBS 版本**：31.1.1 (portable)
- **插件目录**：`C:\Downloads\OBS-Studio-31.1.1-Windows-x64\obs-plugins\64bit\`
- **OBS 配置**：存储在 `%APPDATA%\obs-studio`

---

## ❓ 遇到问题？

### CMake 未找到或退出码 = 1

**症状**：在 VS Code 或普通 PowerShell 中运行 `cmake --version` 提示命令不存在或退点码 1，但在 "Developer PowerShell for VS 2022" 中可以正常运行。

**原因**：VS2022 安装的 CMake 路径未添加到系统 PATH 环境变量。

**解决方法**：

#### 方法一：自动配置（推荐）

1. 打开 **"Developer PowerShell for VS 2022"**（从开始菜单 → Visual Studio 2022 文件夹中找到）
2. 切换到项目目录：
   ```powershell
   cd C:\Users\oldking139\Documents\Repos\Github\OBS-Advanced-Multiview
   ```
3. 运行配置脚本：
   ```powershell
   .\setup-cmake-path-simple.ps1
   ```
4. 脚本会自动检测 CMake 路径并询问是否添加到 PATH，选择 `y`
5. **关闭所有 PowerShell 和 VS Code 窗口**
6. **重新打开 VS Code**
7. 在新终端中验证：`cmake --version`

#### 方法二：手动配置

1. 在 "Developer PowerShell for VS 2022" 中运行：
   ```powershell
   (Get-Command cmake).Source
   # 会显示类似：C:\Program Files\Microsoft Visual Studio\2022\Community\...\cmake.exe
   ```
2. 复制显示的路径（不包括 cmake.exe，只要目录部分）
3. Win + R 输入：`sysdm.cpl`
4. 高级 → 环境变量
5. 用户变量 → 找到 `Path` → 编辑 → 新建
6. 粘贴复制的 CMake 目录路径
7. 确定保存
8. **重启所有终端和 VS Code**

### CMake 未找到
- 确保 Visual Studio 2022 已安装并重启 PowerShell
- 检查是否安装了"用于 Windows 的 C++ CMake 工具"组件

### 下载依赖失败
- 确认代理配置已生效：`echo $env:HTTP_PROXY`
- 检查代理服务是否在 127.0.0.1:31333 运行

### 构建错误
- 检查 Visual Studio 是否安装了所有必需组件
- 查看 build_x64/ 目录下的 CMakeOutput.log

### 插件未加载
- 确认 DLL 已复制到正确的 OBS 插件目录
- 查看 OBS 日志：帮助 → 日志文件 → 查看当前日志

---

## 📝 下一步

当开发环境配置完成后，可以开始：
1. 熟悉 OBS 插件 API 文档
2. 查看项目计划：[plan.md](plan.md)
3. 查看 UI 设计：[docs/ui-ascii-wireframes.md](docs/ui-ascii-wireframes.md)
4. 开始实现第一阶段功能

---

**提示**：将此文件保存为书签，方便随时查阅配置步骤！
