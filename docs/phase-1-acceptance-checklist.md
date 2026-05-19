# Phase 1 验收清单

> 本文档用于将 Milestone 0~3 已完成内容变成可复测、可交接、可回归的事实。
> 进入 Milestone 4 之前，应先确认本清单中的关键项均已验证。

---

## 1.1 插件加载与基础生命周期

- [x] OBS 31.1.1 可以加载插件
- [ ] OBS 32.0 可以加载插件
- [x] OBS 32.1 可以加载插件
- [x] 插件加载失败时有可诊断日志
- [x] 插件卸载时不崩溃
- [x] OBS 退出时不崩溃
- [x] OBS 退出时所有 MultiviewWindow 被安全关闭
- [x] OBS 退出时所有 source 引用被释放
- [x] OBS 退出时配置能安全保存
- [x] 重复打开管理 / 设置 Dialog 不产生失控窗口
- [x] 重复打开同一个 Multiview 实例时聚焦已有窗口，而不是创建第二个同 UUID 窗口

## 1.2 管理 / 设置 Dialog

- [x] OBS 顶部菜单 工具 -> OBS Advanced Multiview 可打开管理 / 设置 Dialog
- [x] Instances tab 正常显示
- [x] Settings tab 正常显示
- [x] Multiview 实例列表正常显示
- [x] 空状态正常显示
- [x] 新建 Multiview 实例正常
- [x] 重命名 Multiview 实例正常
- [x] 重命名后已打开窗口标题同步更新
- [x] 克隆 Multiview 实例生成新的 UUID
- [x] 删除 Multiview 实例前有确认
- [x] 删除 Multiview 实例不影响其他实例
- [x] 删除已打开实例时行为安全，不崩溃
- ~~[ ] folder grouping 正常~~ **已移除**
- ~~[ ] 新建 folder 正常~~ **已移除**
- ~~[ ] rename folder 正常~~ **已移除**
- ~~[ ] move to folder 正常~~ **已移除**
- ~~[ ] 删除 folder 时不会误删不相关实例~~ **已移除**
- [x] 多选行为符合预期（Rename/Clone 在多选时禁用）
- [x] context menu 只在管理 / 设置 Dialog 中提供 New / Clone / Rename / Delete 等实例管理操作

## 1.3 配置系统

- [x] 插件使用 OBS plugin_config 机制
- [x] 配置目录为插件自身目录，不写入插件安装目录
- [x] 配置文件按场景集合隔离
- [x] 默认配置可自动创建
- [x] 配置文件不存在时正常 fallback
- [x] 配置 JSON 损坏时不导致 OBS 崩溃
- [x] 配置字段缺失时安全 fallback
- [x] 保存失败时不破坏旧配置（atomic save with .tmp/.bak）
- [x] 场景集合切换后能加载对应配置
- [x] 场景集合改名后行为符合当前设计
- [x] Multiview 实例重启后恢复
- [x] layout 重启后恢复
- [x] cell assignments 重启后恢复（坐标寻址，legacy 自动迁移）
- [x] global gutter 重启后恢复
- [x] per-instance gutter / inherit global gutter 状态重启后恢复
- [x] Layout Preset 数据结构保留，不要求完整 UI

## 1.4 布局引擎与编辑网格

- [x] 支持 1x1
- [x] 支持 2x2
- [x] 支持 4x4
- [x] 支持 5x5
- [x] 支持 10x10
- [x] rows 限制在 1 到 10
- [x] columns 限制在 1 到 10
- [x] gutter / border 限制在 0 到 50 px
- [x] gutter / border 是 cell 上下左右之间的间距 / 边缘
- [x] span 可以创建
- [x] span 可以取消
- [x] Reset All 可以清除 span
- [x] span 不允许越界
- [x] span 不允许重叠（新增：完全包含时允许吸收合并）
- [x] 无效 span 不导致崩溃
- [x] rows / columns 缩小时，越界 span 被安全处理
- [x] viewport 极小尺寸时不崩溃
- [x] rect 不出现负尺寸
- [x] 规则网格显示正确
- [x] span 后显示正确
- [x] zone separators 显示正确
- [x] 已打开的当前 MultiviewWindow 能动态应用布局变化，无需重启 OBS

## 1.5 Hit-test 与右键菜单

- [x] 已有信号 cell hit-test 正确
- [x] 空 cell hit-test 正确
- [x] gutter / border hit-test 正确
- [x] gutter / border 覆盖 cell 上下左右之间的间距 / 边缘
- [x] 右键已有信号 cell 显示：Fullscreen / Always on Top / Change Source / Clear Cell / Edit Grid / Global Settings / Close
- [x] 右键空 cell 显示：Fullscreen / Always on Top / Add Source / Edit Grid / Global Settings / Close
- [x] 右键 gutter / border 不显示 cell 级操作
- [x] MultiviewWindow 右键菜单中不出现：Open / Focus / Clone / Delete instance / Copy UUID

## 1.6 MultiviewWindow

- [x] 可从管理 / 设置 Dialog 打开 MultiviewWindow
- [x] 可关闭 MultiviewWindow
- [x] 关闭窗口不删除 Multiview 实例
- [x] 可全屏
- [x] 可退出全屏
- [x] 可窗口置顶
- [x] 可取消窗口置顶
- [x] 切换置顶后仍能继续渲染
- [x] 置顶切换引起 native window 重建时不渲染到失效 HWND（Windows: 已改用 SetWindowPos 无需重建）
- [x] resize 后画面正确更新
- [x] 高 DPI 下鼠标命中和渲染比例正确
- [x] 最小化 / 恢复不崩溃
- [x] 多显示器移动不崩溃
- [x] PRVW 无 Studio Mode 时 fallback 行为正常（黄色指示条）
- [x] PGM 显示正常
- [x] Scene 显示正常
- [x] Source 显示正常
- [x] 空 cell 显示正常
- [x] Source 未 ready 或尺寸为 0 时显示安全状态，不崩溃

## 1.7 Source Picker 与 OBS 内部信号

- [x] Source Picker 使用列表 + 搜索
- [x] Source Picker 可选择 PGM
- [x] Source Picker 可选择 PRVW
- [x] Source Picker 可选择 Scene
- [x] Source Picker 可选择 Source
- [x] 添加信号后当前窗口立即更新
- [x] 更换信号后当前窗口立即更新
- [x] 清空 cell 后当前窗口立即更新
- [x] 保存网格信号后重启恢复（自动保存，坐标寻址）
- [x] 保存网格信号不是画面更新前置条件
- [x] Scene 删除后不崩溃
- [x] Source 删除后不崩溃
- [x] Source rename 后不崩溃
- [x] 同一个 Source 被多个 cell 使用时引用释放正确
- [x] Window Capture / Display Capture 关闭后黄色边框释放正确
- [x] 多窗口关闭后 source showing 引用释放正确

## 1.8 CI / 构建 / 格式

- [x] Debug 构建成功
- [x] RelWithDebInfo 构建成功
- [x] Release 构建成功
- [x] clang-format 检查通过
- [x] GitHub Actions 最新主线构建有成功记录
- [ ] tag 构建有成功记录
- [x] Windows 构建通过
- [ ] Linux 构建通过（CI 配置存在，未验证运行时）
- [ ] macOS 构建状态明确（CI 配置存在，未验证运行时）
