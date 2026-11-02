# 《潮痕》持续推进计划

> 状态：P0 小村庄垂直切片开发中  
> 最后更新：2026-11-03  
> 当前目标：完成三 NPC 小村庄的可运行核心、LLM 决策适配、行为树执行、世界推进、回放和自动化验证。

## 上下文恢复入口

对话被压缩、会话重启或开发者接手时，按以下顺序恢复，不重复已经通过的工作：

1. 阅读本文件的“当前检查点”和“未完成工作”。
2. 阅读 [development-design.md](local-npc-llm/development-design.md) 第 3、5、15 节。
3. 查看 `git status --short` 和最近一次提交。
4. 运行 `cmake --build --preset default` 与 `ctest --test-dir build --output-on-failure`。
5. 先修复失败的验收项，再继续当前检查点；不要为了通过测试删除功能或放宽断言。
6. 完成一个工作包后，更新本文件的状态、验证命令和下一步，再提交 Git。

## 已锁定的产品和技术决策

- 产品是单人 3D 开放世界 RPG；P0 使用 3D 斜俯视/跟随相机，UI 使用 2D 面板。
- 游戏核心使用 C++20/CMake；渲染使用 raylib 6.x；UI 使用 TGUI raylib backend。
- 物理使用 Bullet 3.25，通过 `IPhysicsWorld` 隔离；不让 NPC 和剧情代码包含 Bullet 头文件。
- 世界由 `WorldService` 作为 Model 权威推进；Controller 只提交命令；View 只读投影。
- NPC 由 WorldState、Perception、Memory、Character、Decision、Execution 六层组成。
- `GoalSelector` 选择目标，LLM 在候选行动中做高层决策，行为树执行行动，ActionExecutor 原子提交事件。
- LLM 使用本地 llama.cpp OpenAI-compatible API；单共享模型槽位、触发式调用、串行队列；失败回退规则台词。
- 游戏运行中推进所有区域；未加载区域使用 LifeEvent 压缩推进；进程关闭默认不按现实时间推进。
- P0 场景是一个村庄、三个重要 NPC、一个公共资源冲突、一个承诺任务和一个物理互动。

## 当前检查点

| 项目 | 状态 | 证据 |
|---|---|---|
| 游戏和技术设计 | 已完成 | `docs/local-npc-llm/design.md`、`design.en.md` |
| C++ 开发设计 | 已完成 | `docs/local-npc-llm/development-design.md` |
| 业界实践与三 NPC 验证方案 | 已完成 | 开发设计第 14、15 节 |
| 持续推进计划 | 当前提交 | 本文件 |
| Git 初始化 | 当前工作包完成前 | 使用普通 `git` 命令；禁止 `gs-commit` |
| P0 C++ 核心骨架 | 已完成 | `src/core`, `src/world`, `src/npc`, `src/provider`, CMake FetchContent |
| 三 NPC 村庄运行时 | 已完成 | 日程、资源、目标阶段、记忆、承诺和离线推进 |
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

当前检查点已完成 P0-A 至 P0-F：三 NPC 核心、离线推进、LLM fallback、承诺、回放、Blender 场景、raylib 客户端和 Bullet 适配器均有构建或测试证据。下一步进入 P1 港区垂直切片，再按长期路线扩展潮痕的区域、主线、战斗、工具和内容生产管线。



## 全局开发路线（村庄之后）

完成 P0-F 后，按垂直切片继续推进，而不是先铺空地图：

1. **P1 港区**：把村庄资源冲突接到潮门港贸易、船运和第一条主线；补 raylib/TGUI UI、Bullet 碰撞和玩家移动。
2. **P2 群岛骨架**：增加两处可离线推进的区域、道路旅行、阵营关系和关键 NPC LOD；保持事件日志可回放。
3. **P3 主线与结局**：实现灯塔网络调查、承诺汇聚、阵营分歧、失败与多结局；LLM 只表达作者确认的事实。
4. **P4 内容工具链**：内容校验器、对话 beat 编辑、存档迁移、性能基准和开源构建矩阵。

每个阶段都必须先有可玩闭环、离线回放和自动化验收，再扩展地图数量。
