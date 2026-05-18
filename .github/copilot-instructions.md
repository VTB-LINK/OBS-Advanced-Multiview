# Copilot Instructions for OBS Advanced Multiview

## 项目概述

这是一个 OBS Studio 插件项目，使用 C++、Qt 和 OBS Frontend API 开发。

## 关键文档位置

当用户询问项目相关问题时，优先参考以下文档：

1. **开发工作流** → [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md)
   - 构建流程
   - 提交前检查清单
   - 发布流程
2. **环境配置** → [docs/setup/README.md](docs/setup/README.md)
   - 首次配置步骤
   - VS 版本兼容性
3. **项目计划** → [plan.md](plan.md)
   - 功能规划
   - 里程碑

## 构建配置决策树

### 用户说"构建"或"编译"时

**询问用途**，然后选择对应配置：

- **正在开发/调试** → `Debug`

  ```powershell
  cmake --build build_x64 --config Debug
  ```

- **功能完成/准备测试** → `RelWithDebInfo` ⭐ **推荐**

  ```powershell
  cmake --build build_x64 --config RelWithDebInfo
  ```

- **准备发布** → `Release`
  ```powershell
  cmake --build build_x64 --config Release
  ```

### 用户说"功能完成了"或"准备提交"时

**必须执行完整检查流程**（参考 docs/DEVELOPMENT.md）：

```powershell
# 1. 构建所有主要配置
cmake --build build_x64 --config Debug
cmake --build build_x64 --config RelWithDebInfo

# 2. 部署测试
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo

# 3. 提醒用户在 OBS 中测试
# 4. 检查 git status
# 5. 确认提交前检查清单
```

## 常见任务快速参考

### 部署插件到 OBS

```powershell
.\docs\setup\deploy-plugin.ps1              # Debug 版本
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo  # RelWithDebInfo 版本
```

部署脚本会自动：

- 从 buildspec.json 读取插件名称
- 检测 OBS 安装路径（Portable/用户目录/系统安装）
- 复制 DLL 到正确位置

### Visual Studio 版本切换

当前配置：**Visual Studio 2026**

如需切换到 VS 2022：

1. 修改 `CMakePresets.json` 第 59 行
2. 详见 [docs/setup/VS-VERSION-COMPATIBILITY.md](docs/setup/VS-VERSION-COMPATIBILITY.md)

### 构建产物位置

- Debug: `build_x64/Debug/obs-advanced-multiview.dll`
- RelWithDebInfo: `build_x64/RelWithDebInfo/obs-advanced-multiview.dll` ⭐
- Release: `build_x64/Release/obs-advanced-multiview.dll`

## 代码风格和约定

- **格式化工具**: Clang-Format（VS Code 保存时自动）
- **命名约定**: 遵循 OBS 插件标准
- **日志记录**: 使用 OBS 日志 API（`blog(LOG_INFO, ...)`）

## 提交前必做检查

⚠️ **在创建 commit 之前，必须提醒用户**：

1. [ ] Debug 和 RelWithDebInfo 都成功构建
2. [ ] 在 OBS 中测试了修改的功能
3. [ ] 代码已格式化
4. [ ] 没有临时调试代码
5. [ ] git status 确认没有意外文件

详见：[docs/DEVELOPMENT.md#提交前检查清单](docs/DEVELOPMENT.md#提交前检查清单)

## 技术栈

- **语言**: C++17
- **构建系统**: CMake 3.28+
- **UI 框架**: Qt 6
- **目标平台**: Windows (VS 2022/2026), macOS, Linux
- **OBS API**: OBS Studio 31.1.1+ Frontend API

## 项目架构

```
src/
  ├── plugin-main.cpp      # 插件入口点（工具菜单注册、配置路径初始化）
  ├── manager-dialog.hpp   # 管理 / 设置 Dialog
  ├── manager-dialog.cpp   # 管理 / 设置 Dialog 实现
  ├── plugin-support.c.in  # 支持函数（模板）
  └── plugin-support.h     # 头文件

docs/
  ├── DEVELOPMENT.md       # 开发工作流 ⭐
  ├── setup/               # 环境配置文档
  └── phase-1-*.md         # 开发任务分解

build_x64/               # 构建输出（Git 忽略）
  ├── Debug/
  ├── RelWithDebInfo/
  └── Release/
```

## 常见错误处理

### "CMake 找不到"

→ 引导查看：[docs/setup/TROUBLESHOOTING.md](docs/setup/TROUBLESHOOTING.md)

### "插件未加载"

→ 检查：

1. OBS 版本 >= 31.1.1
2. DLL 是否在正确目录
3. OBS 日志中的错误信息

### "构建失败 - 找不到 VS 工具链"

→ 引导查看：[docs/setup/VS-VERSION-COMPATIBILITY.md](docs/setup/VS-VERSION-COMPATIBILITY.md)

## 发布流程简要

当用户说"准备发布"时：

1. 确认版本号（buildspec.json）
2. 构建 Release 和 RelWithDebInfo
3. 创建分发包（参考 docs/setup/DISTRIBUTION.md）
4. 创建 Git tag
5. 创建 GitHub Release

详细步骤：[docs/DEVELOPMENT.md#发布流程](docs/DEVELOPMENT.md#发布流程)

## 调试建议

- **VS Code F5**: 自动构建 Debug、部署并附加调试器
- **断点调试**: 使用 Debug 配置
- **性能分析**: 必须使用 RelWithDebInfo 或 Release

## 重要提醒

1. ⚠️ **不要基于 Debug 版本评估性能** - 性能差异可能达到 10 倍
2. ⚠️ **日常开发推荐 RelWithDebInfo** - 性能接近 Release，仍可调试
3. ⚠️ **提交前必须构建并测试 RelWithDebInfo** - 避免仅在 Debug 下工作的代码

## 给 Copilot Agents 的行为指引

### 当用户开始新功能开发

建议使用 Debug 配置和 F5 调试工作流。

### 当用户说"完成了"

**自动检查**：

1. 提醒构建 RelWithDebInfo
2. 提醒部署测试
3. 展示提交前检查清单
4. 不要立即创建 commit

### 当用户提到"慢"或"性能"

立即询问：**当前使用的是哪个构建配置**？

- 如果是 Debug → 建议切换到 RelWithDebInfo 重新测试
- 如果是 RelWithDebInfo/Release → 才考虑性能优化

### 当用户要求"部署"

默认使用 RelWithDebInfo 配置，除非明确说明要 Debug。

## 文档更新原则

- 修改项目结构时，同步更新 README.md 和 DEVELOPMENT.md
- 添加新工作流时，更新 DEVELOPMENT.md
- 添加新脚本时，更新 docs/setup/README.md
