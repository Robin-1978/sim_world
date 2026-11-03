# 《潮痕》持续推进计划

> 状态：P0 内容包与通用拓扑迁移已完成，进入演化提案基础设施
> 最后更新：2026-11-03  
> 当前目标：完成三 NPC 小村庄的可测试核心，并在进入港区之前完成事务、事件、存档和组件投影的验收。

## 上下文恢复入口

对话被压缩、会话重启或开发者接手时，按以下顺序恢复，不重复已经通过的工作：

1. 阅读本文件的“当前检查点”和“未完成工作”。
2. 阅读 [design.md](design.md) 的“核心哲学”“Runtime 总体架构”“数据驱动内容和演化”和“实施路线”四节。
3. 查看 `git status --short` 和最近一次提交。
4. 运行 `cmake --build --preset default` 与 `ctest --test-dir build --output-on-failure`。
5. 先修复失败的验收项，再继续当前检查点；不要为了通过测试删除功能或放宽断言。
6. 完成一个工作包后，更新本文件的状态、验证命令和下一步，再提交 Git。

## 已锁定的产品和技术决策

- 产品是单人开放世界叙事 RPG，长期架构是“规则驱动、可自主运行、可通过验证提案演化的数字世界”；《潮痕》是第一个世界应用。
- 第一交付先完成文本可测试核心，3D 斜俯视客户端随后接入；渲染不决定世界规则。
- 游戏核心使用 C++20/CMake；渲染使用 raylib 6.x；UI 使用 TGUI raylib backend。
- 物理使用 Bullet 3.25，通过 `IPhysicsWorld` 隔离；不让 NPC 和剧情代码包含 Bullet 头文件。
- 世界由 `WorldService` 作为 Model 权威推进；Controller 只提交命令；View 只读投影。
- NPC 由 WorldState、Perception、Memory、Character、Decision、Execution 六层组成。
- `GoalSelector` 选择长期目标，状态机管理模式，行为树执行计划；LLM 只提出受约束的交互意图和表达候选，ActionExecutor 原子提交事件。
- LLM、规则系统、UI 和物理都只能提交命令或提案；世界演化必须可验证、可追溯、可回滚，系统能力演化必须经过沙箱和版本化发布，核心运行时不能被模型在线改写。
- LLM 使用本地 llama.cpp OpenAI-compatible API；单共享模型槽位、触发式调用、串行队列；失败回退规则台词。
- 游戏运行中推进所有区域；未加载区域使用 LifeEvent 压缩推进；进程关闭默认不按现实时间推进。
- P0 场景是一个村庄、三个重要 NPC、一个公共资源冲突、一个承诺任务和一个物理互动。

## 当前检查点

| 项目 | 状态 | 证据 |
|---|---|---|
| 游戏和技术设计 | 已整合 | `docs/design.md` 是唯一总设计 |
| 业界实践与三 NPC 验证方案 | 已整合 | `docs/design.md` 的实施路线和 P0 展示 |
| 持续推进计划 | 当前提交 | 本文件 |
| Git 初始化 | 已完成 | 首个提交 `78f06db`；后续使用普通 `git` 命令，禁止 `gs-commit` |
| P0 C++ 核心骨架 | 已完成 | `src/core`, `src/world`, `src/npc`, `src/provider`, CMake FetchContent |
| 核心事件与调试契约 | 已完成 | `src/core/event_bus.*`, `docs/design.md`, `--dump-events` |
| 三 NPC 村庄运行时 | 已完成 | 日程、资源、目标阶段、记忆、承诺和离线推进 |
| 通用实体/组件索引与生命周期 | 已完成 | `WorldState.entities`、`WorldService::createEntity/setEntityLifecycle`、`EntityRegistry` 投影 |
| NPC 状态机和全局/局部规划 | 已实现，待提交验收 | `src/npc/state_machine.*`, `src/navigation/path_planner.*` |
| 文本调试界面 | 已实现并验证 | `src/text_ui_main.cpp`, `docs/examples/village-session.txt` |
| 事务原子性与提交后事件 | 已实现并验证 | `WorldService::TransactionScope`, CTest 提交后发布和订阅者失败测试 |
| 存档 schema/事件截断点 | 字段已实现，迁移校验待补 | `save_schema_version`, `simulation_version`, `event_sequence` |
| LLM 演化回路 | 产品核心，设计与内容包接口待实现 | Evolution Proposal、沙箱评估、版本化激活、回滚和审计 |
| 三 NPC 自主因果链演示 | 已完成 | 无玩家推进产生 `life.*`、`social.cooperation`、`npc.goal_revised`，文本和 headless EventLog 可观察 |
| World Content Pack 加载 | 已完成 | `WorldService::load()` 从 JSON 内容包构建实例；拓扑使用通用 `entities`/`links`，不保留房间/门专用运行时接口 |
| 自动化验收 | 已完成 | GCC 13、CTest、headless replay/snapshot |
| Blender 场景资产 | 已完成 | `assets/blender/village.blend` 和 `village.glb` |
| 3D 客户端 | 已完成 P0 | raylib 5.5 client、Blender GLB loading、2D HUD、WASD/E interaction；Bullet 3.25 adapter is optional and tested separately |

## P0 完成定义

只有满足以下条件，才可以把三 NPC 小村庄标记为完成：

1. 三个 NPC 具有独立的日程、公开目标、隐藏目标、底线和失败结果。
2. `GoalSelector` 能按压力、资源、承诺和关系选择候选目标。
3. 行为树支持 `Sequence`、`Selector`、`Condition`、`Action`、`Timeout` 和 `Interrupt`。
4. LLM Provider 能接受感知/记忆/角色/候选行动，返回 `action_id`、动机和结构化对白；mock Provider 可重复测试。
5. 非法动作、过期承诺、未知 ID、版本冲突和重复 `turn_id` 不改变世界。
6. 玩家可以接受、重谈、拒绝和违约同一承诺，并观察四种后果。
7. 玩家推进两天时，至少两个 NPC 在没有玩家参与的情况下改变位置、资源或目标阶段。
8. 存档、事件日志和 replay 能在不访问 LLM 的情况下得到一致世界结果。
9. LLM 服务关闭时，规则台词、世界推进、任务和存档仍然可用。
10. 3D 客户端可以加载村庄、移动玩家、观察三个 NPC 和触发至少一个 Bullet 物理互动；headless 核心测试不依赖显示器。
11. 无玩家输入时，至少一条“资源压力 → NPC 行动 → NPC 间影响 → 次日状态变化”的因果链可由规则系统完成，并能在文本界面和 EventLog 中观察。

## 工作顺序

### P0-A：仓库与构建（已完成）

建立 CMake targets、依赖锁定、编译选项、`tide_marks_headless`、测试目录和许可证清单。先让纯核心在没有图形环境时通过构建。

### P0-B：确定性世界核心（已完成）

实现 `WorldState`、`WorldService`、游戏时间、命令、事件、快照和事件日志。测试同一命令序列的结果和版本冲突。

### P0-C：NPC 生活与行为树（已完成）

实现三 NPC 内容、感知投影、四类记忆、关系四轴、日程、GoalSelector、行为树和 LifeEvent。测试离线推进、打断和失败回退。

### P0-D：LLM 决策适配（已完成）

实现 Provider Registry、串行 TurnQueue、OpenAI-compatible 请求、mock Provider、JSON 解析和动作白名单。LLM 只在候选行动中选择，不直接写世界。

### P0-E：村庄玩法闭环（已完成）

实现公共资源冲突、承诺任务、接受/重谈/拒绝/违约四种结果、三个 NPC 的关系变化和可观察世界后果。

### P0-F：3D 客户端与回放（已完成）

接入 raylib 5.5、斜俯视相机、Blender GLB、玩家移动、NPC 标记、2D 对话 HUD、Bullet 3.25 适配器和 replay viewer。TGUI 面板保留在 P1 UI 工作包；P0 的 HUD 使用 raylib 原生 2D API。

### P0-G：核心一致性收尾（已完成）

完成通用实体组件索引、NPC 粗粒度状态机、全局 Dijkstra/局部 A* 规划和文本客户端的测试验收。随后把世界写入收口到可回滚事务，确保事件只在提交后发布；区分持久化事件日志和进程内 EventBus，并为存档记录 schema、规则/内容身份和事件序列截断点。此工作包完成前不扩展港区内容。

验收命令：

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/tide_marks_text --script docs/examples/village-session.txt
```

### P0-H：自主因果链（已完成）

在现有三 NPC 日程之上完成不依赖玩家输入的最小闭环：资源/需求检测产生 NPC 目标，目标通过规则动作影响另一个 NPC 或公共资源，第二天由关系或资源结果改变计划。链路使用同一 `WorldService` 事务和 `WorldEvent`，模型关闭时仍可完成。

### P0-I：内容包加载（已完成）

把实例定义从应用代码拆成版本化 JSON 内容包：NPC 定义、空间实体、连接关系、日程、资源和行为候选由 Schema 加载；Runtime 只保留通用算法、组件和验证器。当前入口是 `WorldService::load()` / `fromContent()`，加载后的状态由事务维护。

## 自动化验收

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --scenario village --days 2 --replay /tmp/tide_marks-village.jsonl
cmake --preset engine
cmake --build --preset engine
```

验收报告必须记录：编译器和构建版本、内容版本、随机种子、Provider、模型名（如有）、每次命令、每个 WorldEvent、回放摘要、测试数量和失败原因。模型响应必须脱敏后随回放工件保存，不能把 API key 写入工件。

本次 P0 证据：默认构建和 CTest 使用 GCC 13 通过；raylib 5.5 客户端在 Xvfb 下成功初始化；Bullet 3.25 adapter build 和 `BulletPhysicsWorld` 碰撞测试通过；本机 `127.0.0.1:8192/v1/models` 发现 `Huihui-Qwen3.5-4B-abliterated.Q4_K_M`，一次结构化对话成功提交，关闭/超时路径回退测试也通过。

## 风险和处理

- **LLM 延迟或断开**：前台串行队列、超时、取消、规则台词；不阻塞世界时间。
- **模型选择越权动作**：候选行动枚举、Schema、ActionValidator、版本检查和幂等提交。
- **NPC 生活过于机械**：作者提供条件日程和关系冲突；LLM 只增强表达，不用随机任务填充内容。
- **NPC 数量扩大后性能下降**：关键/支持/环境三档模拟 LOD；远端只处理下一个 LifeEvent。
- **Bullet 与游戏状态分叉**：物理只产生有限事件，游戏事实仍由 WorldService 提交。
- **内容和代码不同步**：启动时 Schema 校验、全局 ID 校验和内容版本号。

## 下一步

当前检查点已完成 P0-A 至 P0-I 的主要实现，构建、CTest、文本会话、通用拓扑加载、实体生命周期和 headless 回放均通过。下一项是把演化提案的 Schema、沙箱评估和版本化激活接入 Runtime；在此之前不扩展港区内容。



## 全局开发路线（村庄之后）

完成 P0-F 后，按垂直切片继续推进，而不是先铺空地图：

1. **P1 港区**：把村庄资源冲突接到潮门港贸易、船运和第一条主线；补 raylib/TGUI UI、Bullet 碰撞和玩家移动。
2. **P2 群岛骨架**：增加两处可离线推进的区域、道路旅行、阵营关系和关键 NPC LOD；保持事件日志可回放。
3. **P3 主线与结局**：实现灯塔网络调查、承诺汇聚、阵营分歧、失败与多结局；LLM 只表达作者确认的事实。
4. **P4 内容工具链**：内容校验器、对话 beat 编辑、存档迁移、性能基准和开源构建矩阵。

每个阶段都必须先有可玩闭环、离线回放和自动化验收，再扩展地图数量。
