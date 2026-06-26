# AGENTS.md — OBS Advanced Multiview 开发指南(给 AI agent / 协作者)

本文件是在本仓库工作的**强制流程**。OBS 插件,跑在用户的直播 OBS 进程里——**炸 OBS = 直播事故**。
稳定性优先于一切功能。下面每一节都按"先怎么想 → 怎么写 → 写完跑什么 → 提交前 → 发版"展开。

---

## 0. 最高约束(不可违背)

1. **绝不炸 OBS** —— 无论是从 OBS *取*数据(渲染源、读场景树),还是把数据*交还* OBS(回调、信号、inc/dec)。
2. **非阻塞** —— 渲染线程(OBS graphics 线程)绝不可被阻塞;会 fire 第三方插件回调的操作绝不在持锁时做。
3. **非入侵 / 隔离** —— 一个实例的故障不连累其它实例 / OBS 本体;私有源、信号检测、渲染状态都隔离。
4. **广播级稳定** —— 宁可降级(显示警告/黑场/fallback),也绝不崩、绝不卡。

任何设计/实现/评审,先用这四条过一遍,再谈功能。

---

## 1. 先设计(大改动前必做)

- **大改动先写设计文档**放 `docs/`(中文),钉死:动机、约束、方案、迁移、取舍、验证方法。范例:
  `docs/signal-lost-redesign-design.md`、`docs/issue-5-signal-lost-fallback-reenable-design.md`。
- 设计里**显式标注崩溃面 / 锁序 / 线程**,并与既有硬化笔记(`docs/*-hardening-notes.md`)对齐。
- 有 OBS 行为不确定时,**直接读 libobs 源码**(见 §2),不要凭记忆。
- 涉及多方案/取舍的关键分叉,先和用户确认再写几百行。

---

## 2. 与 OBS 交互的硬规则(踩坑沉淀,务必遵守)

- **锁序**:唯一合法嵌套是 `OBS graphics lock → source_mutex_`(渲染回调里 OBS 先持 graphics lock,我们再取
  `source_mutex_`,recursive)。**绝不**在持 `source_mutex_` 时 `obs_enter_graphics()`。teardown/rebuild 用
  四阶段:锁内收集 → 释放锁 → GPU/重操作 → 再锁内安装。
- **`inc/dec_showing` / `inc/dec_active`**:会走 `obs_source_activate/deactivate` 遍历场景树(取 libobs 锁),
  且其 hide/show 信号会进第三方插件回调。**绝不在渲染线程、绝不在持 `source_mutex_` 时调**。需要时用
  deferred-flag + `QTimer::singleShot(0, core, ...)` 投递到主线程锁外执行(参考 `reconcile_fallback_showing`)。
- **源 remove 同步窗口里不要 dec_showing**:`source_remove` 信号处理里命中只置空 weak_ref/状态,**不 dec**
  (源销毁会随 show_refs 一起释放;此刻 dec 会同步触发第三方 hide 回调而崩)。主动解绑/切回时才在**锁外** dec。
- **`OBSSourceAutoRelease` 赋值是 adopt(不 addref)**;赋值后再 `obs_source_release` = **过度释放 → UAF**。
  规范:`OBSSourceAutoRelease x = obs_get_source_by_name(...)` 从不手动 release;`OBSGetStrongRef`/`get_ref`
  赋给 autorelease 也是 adopt。详见 `docs/...` 与提交 `ce6e933`。
- **弱引用身份**:`obs_source_get_weak_source` 返回源的单一控制块,可用 `.Get()` 裸指针比较身份。
- **渲染前守卫**:渲染任何外部解析来的 source 前,`obs_source_removed()` 双重守卫(解析时 + video_render 前)。
- **本地有 OBS 源码树**可查:`C:\Users\oldking139\Documents\Repos\Github\obs-studio`(`libobs/obs-source.c`、
  `obs-scene.c`、`obs.hpp` 等);构建依赖在 `.deps/obs-studio-31.1.1`(以**构建依赖那份 obs.hpp** 的语义为准)。

---

## 3. 写完跑什么(每次完成一处改动,自动执行)

顺序固定,**不等用户要求**:

```powershell
# 1) clang-format 检查(已装 19.1.1)——改动的 .cpp/.hpp 逐个
clang-format -i <files...>                      # 直接对齐
clang-format --dry-run --Werror <files...>      # 确认 CLEAN

# 2) 构建两个配置(构建目录 build_x64,已配置)
cmake --build build_x64 --config Debug
cmake --build build_x64 --config RelWithDebInfo  # 若 LNK1201 先删 build_x64/RelWithDebInfo/*.pdb 再重链(OBS 开着会锁 PDB)

# 3) 部署 Rel 版本(PowerShell 脚本;部署 DLL+data 到所有 OBS Portable 路径,并做 locale parity 校验)
.\docs\setup\deploy-plugin.ps1 RelWithDebInfo
```

- **改完代码 → 三步跑完 → 报告结果**。Debug 通过=语法/逻辑无误;Rel 是发给用户测的包。
- **locale 改动**必须保持 `data/locale/en-US.ini` 与 `zh-CN.ini` 的 key 集合一致(deploy 脚本会拦不一致)。
- 只改了 locale 时可只跑 deploy(脚本会拷最新 data;DLL 不变)。
- `LNK1201`(写 PDB 失败)= OBS 正开着锁住了 build PDB,或 mspdbsrv 残留:删 PDB 重链;仍不行需用户关 OBS。

---

## 4. 提交前跑什么 + 提交策略

**提交前格式自检**(对齐 CI):

```powershell
clang-format --dry-run --Werror <changed .cpp/.hpp>   # C/C++ 19.1.1
gersemi --check <changed CMake files>                 # CMake;若改了 CMakeLists.txt / cmake/*.cmake
# gersemi -i <files> 直接对齐(配置见 .gersemirc:line_length 120 / indent 2)
```

**提交策略(重要):**
- **不自动提交。** 流程是:改完 → 构建部署 → **用户测试通过后** → 用户说"提交"才提交。
- **commit 不加任何 co-author / `Co-Authored-By` / AI 署名。** 只写正文。
- **不在 master 上直接改/提交**:先开 feature 分支(`feat/...` 或 `fix/...`)。
- commit message:Conventional Commits(`feat:` / `fix:`),正文说清动机 + 改了什么 + 风险;带 issue 号(如 `(#5)`)。
- 多行 message 用文件 + `git commit -F <file>`(避免 Bash/PowerShell here-string 转义把 `@` 等混进标题)。
- 一个独立的既有 bug 修复**单独拆 commit**,别混进 feature。

---

## 5. 发版前代码硬化(必做,别跳)

新功能(尤其碰并发/源生命周期/渲染)发版前,做一轮**系统硬化审计**,沉淀成 `docs/<feature>-hardening-notes.md`。
参考 `docs/phase-3-hardening-notes.md`、`docs/signal-lost-v2-hardening-notes.md` 的格式。

**审计方法**:对最高风险面**并行派多个审计 agent**(read-only,各自独立视角),典型三路:
1. **并发 / 引用计数 / 锁序**:inc/dec_showing 配对(漏 dec=泄漏 / 重复 dec=过度释放)、TOCTOU、锁内是否误调
   重操作、QTimer 投递在持锁时是否只入队不同步执行、QObject 线程亲和性(析构取消未决事件的前提)。
2. **数据模型 / 序列化 / 迁移**:round-trip 稳定性、旧配置无损迁移、钳位、obs_data 引用计数、边界(空值/默认值)。
3. **状态机 / 时序**:计数器递增-重置闭环、退避/冷却边界、整数溢出/unsigned 回绕、各早返回路径的副作用。

**判级与处置**:
- **HIGH**(UAF / 过度释放 / 锁序倒置 / 会炸或阻塞 OBS / 违反退避契约)→ **发版前必修**。
- **MED**(矛盾态 / 数据丢失 / 自愈型泄漏窗口)→ 能便宜修就修(防御性兜底),否则记录。
- **LOW / 观察项**(理论路径 / 诊断可读性 / 受控例外)→ 写进硬化笔记备查,不强求改。

**硬化笔记内容**:总评(有无阻断缺陷)+ 修复清单(文件/问题/级别)+ 观察项(已分析、为何不改)+ 验证步骤
(尤其需要装第三方插件压测的并发场景,如 streamdeck-plugin-obs 反复 remove/restore + 主源反复掉线)。

---

## 6. Tag / 发版

- **不主动打 tag / 不主动合并 master**——由用户决定。
- 版本号在 `buildspec.json`(`name` / `version`);改版本时同步它。
- 真要发版:用户确认后,bump `buildspec.json` 版本 → tag(语义化 `vX.Y.Z`,与 buildspec 版本一致)→ 可选 changelog。
- 不替用户 push / 不开 PR / 不跑 ultrareview,除非用户明确要求。

---

## 7. 仓库速览

- `src/` —— 核心源码。命名约定:`amv-instance-core-*.cpp`(每实例核心:`-draw` 渲染、`-sources` 源解析/refresh、
  `-health` 外部源监督器、`-status`/`-label`/`-vu`/`-lost-image` 各叠加层);`signal-provider-*.cpp`(ffmpeg/ndi/
  spout/vlc provider);`*-settings-dialog.cpp` 各设置对话框;`multiview-instance-serialize-*.cpp` 序列化。
- `data/locale/` —— `en-US.ini`(基准)+ `zh-CN.ini`,**key 集合必须一致**。
- `docs/` —— 设计文档 + 各阶段硬化笔记(中文);`docs/setup/` 构建/部署脚本与说明。
- 构建目录 `build_x64`(已配置;重配用 `docs/setup/configure-cmake.ps1`)。
- 注释风格:代码注释英文,设计文档中文;与用户对话用中文。
