# 《潮痕》持续推进计划

> 本文件只记录实施状态、工作包、验证和下一步；架构与模型以 [design.md](design.md) 为唯一来源。
> 最后更新：2026-11-03

## 当前目标

完成一个由三 NPC 小村庄承载的最小平台验证：需求/内驱随游戏时间变化，外部事件和结构约束进入决策上下文，规则或 LLM 产生意图，C++ 校验并提交事实，结果进入 EventLog、Snapshot、Replay 和文本调试界面。3D、港区和场景实验暂不作为当前门槛。

## 当前检查点

| 工作包 | 状态 | 证据/位置 |
|---|---|---|
| C++20/CMake 核心构建 | 已完成 | `src/core`, `src/world`, `CMakeLists.txt` |
| WorldState、事务、Revision、EventLog、EventBus | 已完成 | `src/world/world_service.*`, `src/core/event_bus.*` |
| 通用 Entity/Component、拓扑和生命周期 | 已完成 | `WorldState.entities`, `EntityRegistry`, `createEntity`, `setEntityLifecycle` |
| NPC 状态机、行为树、全局/局部规划 | 已完成 | `src/npc/state_machine.*`, `src/npc/behavior_tree.*`, `src/navigation/*` |
| 文本调试界面和 headless 回放 | 已完成 | `src/text_ui_main.cpp`, `src/headless_main.cpp` |
| 内容包加载 | 已完成 | `WorldService::load/fromContent`, `data/`, `tests/fixtures/minimal-pack` |
| 存档 schema 与事件截断点 | 已完成 | `world_state_json.*`, `save_schema_version=5`, `event_sequence` |
| 内驱/需求模型 | 已实现，待扩展验收 | `src/npc/drive_system.*`, `NeedContent`, `NeedState`, `need.*` events |
| 意图上下文边界 | 已实现，待结构化回归 | `DecisionContext.internalDrives`, `externalStimuli`, `structuralConstraints`, `IntentProposal` |
| 通用资源与实例活动 | 基础层已实现，语义待扩展 | `ResourceContent`, `ResourceState(amount,reserved)`, `ActivityContent`, data-driven schedule effects |
| 元动作语义目录 | 已冻结，资源执行器已接入 | `src/world/meta_actions.*`, 设计文档“原子元动作与复合行为” |
| World Intelligence 演化提案 | 未开始 | 设计已冻结，待 P2 工作包 |
| 3D 表现和 Blender 场景 | 已有 P0 原型 | 不参与当前平台模型验收 |

## 已完成验证

- 任意最小内容包可以启动，不依赖 Mira、Lorn、Eve 或村庄专用 API。
- 实体创建、休眠、激活和销毁经过事务，销毁身份保留在事实层并可从 Snapshot 恢复。
- Promise、资源、关系、NPC 日程和事件在两日离线推进中可观察。
- LLM 不可用时规则台词和世界推进继续工作。
- Snapshot/Replay 不访问 LLM；事件只在事务提交后发布。
- 需求缺口会随游戏时间增加，已提交动作会产生 `need.relieved`，跨阈值会产生 `need.critical`，需求状态可保存和恢复。

## P0 工作顺序

### P0-J：需求与驱动力收口（当前）

1. 保持 `NeedDefinition/NeedState -> DriveSystem -> NeedEvent -> GoalSelector` 的确定性链路。
2. 为每个需求事件补充来源事件、规则版本和时间区间字段。
3. 在文本界面显示每个 NPC 的 deficit、drive、临界状态和最近满足来源。
4. 增加需求衰减、临界事件、满足、快照恢复和最小内容包的自动化断言。
5. 清理剩余内容特例，使 P0 行为通过内容包动作和注册规则表达，避免把 NPC ID 写进 Runtime 算法。
6. 将当前村庄资源字段收口为通用资源组件/资源存储；该基础层已完成，下一步补生产、交易、合同和通信对象。

### P0-K：意图和行动契约

1. 将 Provider 结构化响应固定为 `IntentProposal`：候选动作、意图、理由、预期结果、置信度、上下文 Revision。
2. 校验模型返回的上下文版本、候选白名单、资源、权限、关系和幂等键。
3. 让已提交行动的结果统一形成 WorldEvent，并驱动需求、关系、记忆和下一轮外部刺激。
4. 对模型关闭、模型迟到、过期提案、未知动作和重复提案增加回归测试。

### P0-L：小村庄因果演示

1. 用文本界面展示“资源变化 → 内/外驱 → 意图 → 行动 → 他人/公共资源变化 → 次日计划变化”。
2. 记录每个节点的事实来源和规则原因，支持事件检查和回放差异。
3. 保持一个共享模型槽位；不让 LLM 阻塞游戏时间，也不让 LLM 直接修改 WorldState。
4. P0 通过后再进入港区内容，不提前实现商店、利率或政策沙箱。

### P0-M：世界对象与元动作建模（先于扩展行为）

1. 冻结 Entity/Agent、Resource、ProductionProcess、ConsumptionNeed、Exchange/Contract、Communication、Institution/Rule 和 Event 的属性契约。
2. 让内容加载器校验单位、所有权、容量、输入/输出、时间窗口、权限和引用，不接受只有名称没有语义属性的动作。
3. 完成 P0 元动作注册表：资源消耗/产出/转移/预留、需求满足、关系调整、移动、承诺、记忆、连接状态和实体生命周期。
   - 已完成执行器：`consume_resource`、`produce_resource`、`reserve_resource`、`release_resource`。
4. 把小村庄真实的配给、修船、档案整理和承诺行为组合为 BehaviorRecipe；不再为每个 NPC 添加 C++ 特例。
5. 为元动作、复合行为、事务失败和回滚补自动化测试；测试通过前不增加商店、利率或政策规则。

## 后续工作包

### P1：潮门港与集体驱力

加入港区、交易、资源预留、家庭/组织指标、阵营关系、近远场模拟等级、完整 UI、物理机关和内容迁移。验收仍以可回放的事实链为准。

### P2：World Intelligence 演化和沙箱

实现 Evolution/Scenario Proposal Schema、提案库、来源事实、回放沙箱、指标评估、审批策略、版本化 overlay、激活、观察和回滚。只有已注册的声明式能力可以在线激活；新增 C++ 能力仍需开发者发布。

### P3：可替换世界实例

用同一 Runtime 加载新的小说世界，扩展多区域、阵营、经济和长期余波。现实世界实验只有在定义对应机制、数据边界和校准方法后才进入计划。

## 自动化验证命令

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --scenario village --days 2 --dump-events
./build/tide_marks_text --script docs/examples/village-session.txt
```

每次工作包完成后记录：内容版本、规则/模拟版本、Provider/模型（如有）、随机种子、命令、WorldEvent、Snapshot 截断点、Replay 摘要和失败原因。不得把 API Key 写入日志或回放工件。

## 风险与当前处理

- LLM 延迟或断开：规则决策和回退台词继续推进，模型不拥有提交权限。
- 内驱过于写死：参数、需求、动作满足关系和阈值来自 JSON；新驱动力必须走提案和沙箱，不在 C++ 添加 NPC 特例。
- LLM 越权：候选动作、结构化 IntentProposal、版本检查和 ActionValidator。
- 世界事实分叉：单一 WorldState 写入入口、事务、EventLog 和提交后 EventBus。
- 内容扩大后性能下降：近场/远端模拟 LOD 和事件唤醒，不让所有实体逐帧模拟。

## 恢复工作入口

会话恢复时依次阅读本文件的“当前检查点”和“P0 工作顺序”，再阅读 [design.md](design.md) 的“事实、意图与演化边界”“世界与社会的内在驱力模型”“NPC Runtime 与 World Intelligence”“Runtime 核心契约”。然后查看 `git status --short`，运行自动化验证命令，先修复当前工作包失败项，再继续下一项；不要为了通过测试删除功能或放宽断言。
