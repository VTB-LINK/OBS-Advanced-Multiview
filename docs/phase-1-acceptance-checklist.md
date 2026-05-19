# Phase 1 验收清单

> 本文档用于将 Milestone 0~3 已完成内容变成可复测、可交接、可回归的事实。
> 进入 Milestone 4 之前，应先确认本清单中的关键项均已验证。

---

## 1.1 插件加载与基础生命周期

- [ ] OBS 31.1.1 可以加载插件
- [ ] OBS 32.0 可以加载插件
- [ ] OBS 32.1 可以加载插件
- [ ] 插件加载失败时有可诊断日志
- [ ] 插件卸载时不崩溃
- [ ] OBS 退出时不崩溃
- [ ] OBS 退出时所有 MultiviewWindow 被安全关闭
- [ ] OBS 退出时所有 source 引用被释放
- [ ] OBS 退出时配置能安全保存
- [ ] 重复打开管理 / 设置 Dialog 不产生失控窗口
- [ ] 重复打开同一个 Multiview 实例时聚焦已有窗口，而不是创建第二个同 UUID 窗口

## 1.2 管理 / 设置 Dialog

- [ ] OBS 顶部菜单 工具 -> OBS Advanced Multiview 可打开管理 / 设置 Dialog
- [ ] Instances tab 正常显示
- [ ] Settings tab 正常显示
- [ ] Multiview 实例列表正常显示
- [ ] 空状态正常显示
- [ ] 新建 Multiview 实例正常
- [ ] 重命名 Multiview 实例正常
- [ ] 重命名后已打开窗口标题同步更新
- [ ] 克隆 Multiview 实例生成新的 UUID
- [ ] 删除 Multiview 实例前有确认
- [ ] 删除 Multiview 实例不影响其他实例
- [ ] 删除已打开实例时行为安全，不崩溃
- [ ] folder grouping 正常
- [ ] 新建 folder 正常
- [ ] rename folder 正常
- [ ] move to folder 正常
- [ ] 删除 folder 时不会误删不相关实例
- [ ] 多选行为符合预期
- [ ] context menu 只在管理 / 设置 Dialog 中提供 New / Clone / Rename / Move / Delete 等实例管理操作

## 1.3 配置系统

- [ ] 插件使用 OBS plugin_config 机制
- [ ] 配置目录为插件自身目录，不写入插件安装目录
- [ ] 配置文件按场景集合隔离
- [ ] 默认配置可自动创建
- [ ] 配置文件不存在时正常 fallback
- [ ] 配置 JSON 损坏时不导致 OBS 崩溃
- [ ] 配置字段缺失时安全 fallback
- [ ] 保存失败时不破坏旧配置
- [ ] 场景集合切换后能加载对应配置
- [ ] 场景集合改名后行为符合当前设计
- [ ] Multiview 实例重启后恢复
- [ ] layout 重启后恢复
- [ ] cell assignments 重启后恢复
- [ ] global gutter 重启后恢复
- [ ] per-instance gutter / inherit global gutter 状态重启后恢复
- [ ] Layout Preset 数据结构保留，不要求完整 UI

## 1.4 布局引擎与编辑网格

- [ ] 支持 1x1
- [ ] 支持 2x2
- [ ] 支持 4x4
- [ ] 支持 5x5
- [ ] 支持 10x10
- [ ] rows 限制在 1 到 10
- [ ] columns 限制在 1 到 10
- [ ] gutter / border 限制在 0 到 50 px
- [ ] gutter / border 是 cell 上下左右之间的间距 / 边缘
- [ ] span 可以创建
- [ ] span 可以取消
- [ ] Reset All 可以清除 span
- [ ] span 不允许越界
- [ ] span 不允许重叠
- [ ] 无效 span 不导致崩溃
- [ ] rows / columns 缩小时，越界 span 被安全处理
- [ ] viewport 极小尺寸时不崩溃
- [ ] rect 不出现负尺寸
- [ ] 规则网格显示正确
- [ ] span 后显示正确
- [ ] zone separators 显示正确
- [ ] 已打开的当前 MultiviewWindow 能动态应用布局变化，无需重启 OBS

## 1.5 Hit-test 与右键菜单

- [ ] 已有信号 cell hit-test 正确
- [ ] 空 cell hit-test 正确
- [ ] gutter / border hit-test 正确
- [ ] gutter / border 覆盖 cell 上下左右之间的间距 / 边缘
- [ ] 右键已有信号 cell 显示：Fullscreen / Always on Top / Change Source / Clear Cell / Edit Grid / Save Cell Assignments / Global Settings / Close
- [ ] 右键空 cell 显示：Fullscreen / Always on Top / Add Source / Edit Grid / Save Cell Assignments / Global Settings / Close
- [ ] 右键 gutter / border 不显示 cell 级操作
- [ ] MultiviewWindow 右键菜单中不出现：Open / Focus / Clone / Delete instance / Copy UUID

## 1.6 MultiviewWindow

- [ ] 可从管理 / 设置 Dialog 打开 MultiviewWindow
- [ ] 可关闭 MultiviewWindow
- [ ] 关闭窗口不删除 Multiview 实例
- [ ] 可全屏
- [ ] 可退出全屏
- [ ] 可窗口置顶
- [ ] 可取消窗口置顶
- [ ] 切换置顶后仍能继续渲染
- [ ] 置顶切换引起 native window 重建时不渲染到失效 HWND
- [ ] resize 后画面正确更新
- [ ] 高 DPI 下鼠标命中和渲染比例正确
- [ ] 最小化 / 恢复不崩溃
- [ ] 多显示器移动不崩溃
- [ ] PRVW 无 Studio Mode 时 fallback 行为正常
- [ ] PGM 显示正常
- [ ] Scene 显示正常
- [ ] Source 显示正常
- [ ] 空 cell 显示正常
- [ ] Source 未 ready 或尺寸为 0 时显示安全状态，不崩溃

## 1.7 Source Picker 与 OBS 内部信号

- [ ] Source Picker 使用列表 + 搜索
- [ ] Source Picker 可选择 PGM
- [ ] Source Picker 可选择 PRVW
- [ ] Source Picker 可选择 Scene
- [ ] Source Picker 可选择 Source
- [ ] 添加信号后当前窗口立即更新
- [ ] 更换信号后当前窗口立即更新
- [ ] 清空 cell 后当前窗口立即更新
- [ ] 保存网格信号后重启恢复
- [ ] 保存网格信号不是画面更新前置条件
- [ ] Scene 删除后不崩溃
- [ ] Source 删除后不崩溃
- [ ] Source rename 后不崩溃
- [ ] 同一个 Source 被多个 cell 使用时引用释放正确
- [ ] Window Capture / Display Capture 关闭后黄色边框释放正确
- [ ] 多窗口关闭后 source showing 引用释放正确

## 1.8 CI / 构建 / 格式

- [ ] Debug 构建成功
- [ ] RelWithDebInfo 构建成功
- [ ] Release 构建成功
- [ ] clang-format 检查通过
- [ ] GitHub Actions 最新主线构建有成功记录
- [ ] tag 构建有成功记录
- [ ] Windows 构建通过
- [ ] Linux 构建通过
- [ ] macOS 构建状态明确
