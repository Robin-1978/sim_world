# 《潮痕》持续推进计划

> 本文件只记录实施状态、工作包、验证和下一步；架构与模型以 [design.md](design.md) 为唯一来源。
> 最后更新：2026-11-05
> 目标摘要见 [`goal.md`](goal.md)。恢复工作时先读目标摘要，再按本文件的检查点和工作包继续。

## 当前目标

维护三 NPC 小村庄、可替换港区和第三个独立小说实例，并在同一契约上补齐 World Intelligence 的可审计闭环：需求/内驱随游戏时间变化，外部事件和结构约束进入决策上下文，规则或 LLM 产生意图，C++ 校验并提交事实，结果进入 EventLog、Snapshot、Replay 和文本调试界面；异常规则做确定性检测，Scenario Proposal 从同一 Snapshot 对照运行且不污染源世界。P3 的可替换性纵向切片已经完成；后续工作是把同一模型扩展到更多内容与真实领域，不把尚未建模的商店、利率或政策伪装成可计算结果。

当前 P0、P1、P2 与 P3 的“最小纵向检查点”均已完成。这里的完成表示对应契约已有可运行、可回放、可测试的闭环，不表示完整产品愿景已经做完。P1 增加了异步决策队列、游戏时间截止、同 NPC 请求合并、已注册 operator、港区社会切片、声明式跨主体资源抢占和近远场调度基准；P2 增加了内容声明异常检测、反事实场景实验、结果 JSON 审计与 CLI 复现入口；P3 增加了跨区域、双阵营、制度协定、自定义需求域和长期承诺余波的第三个内容实例。抵押/价格/复杂仲裁、声明式长过程补偿、商店/利率/政策模型、大规模社会校准与运行时 Schema 热演化仍属于后续扩展。 `ContractSystem` 拆分承诺/交换协调后，WorldService 继续只保留统一事实协调和事务边界。

## 当前检查点

| 工作包 | 状态 | 证据/位置 |
|---|---|---|
| C++20/CMake 核心构建 | 已完成 | `src/core`, `src/world`, `CMakeLists.txt` |
| WorldState、事务、Revision、EventLog、EventBus | 已完成 | `src/world/world_service.*`, `src/core/event_bus.*` |
| 通用 Entity/Component、拓扑和生命周期 | 已完成 | `WorldState.entities`, `EntityRegistry`, `createEntity`, `setEntityLifecycle` |
| NPC 状态机、行为树、全局/局部规划 | 已完成 | `src/npc/state_machine.*`, `src/npc/behavior_tree.*`, `src/navigation/*` |
| 文本调试界面和 headless 回放 | 已完成 | `src/text_ui_main.cpp`, `src/headless_main.cpp` |
| 内容包加载 | 已完成 | `WorldService::load/fromContent`, `data/`, `tests/fixtures/minimal-pack` |
| 存档 schema 与事件因果链 | 已完成 | `world_state_json.*`, `save_schema_version=18`, `simulation_version=adaptive-runtime-p1.19`, 事件 schema 4、稳定 `event_id`、事务 `correlation_id`、直接 `causation_sequence`、逐事件内容/模拟版本、Snapshot 因果与版本完整性校验、`event_sequence`, `instance_id`, `inventories`, `relationships`, `social_groups`, `exchange_contracts`, `NpcActivity` 聚合、`lastSimulationTick/lastDriveTick`、`definition_id` 引用、实例/内容版本校验与可重放 overlay 栈；运行态不复制角色定义 |
| 内驱/需求模型 | 需求域是内容定义的标签，村庄默认四域、Glass Orchard 使用 stewardship/ecological；`DriveEvaluation` 统一 urgency、effective weight、预期满足、证据置信度和效用贡献，同一行动满足多个需求时逐项求和；显式满足配方和有界预期适应已实现，安全事实已支持冲突/威胁/防守/撤离/修复并通过 Stimulus 投射 | `src/npc/drive_system.*`, `src/npc/goal_selector.*`, `NeedContent`, `NeedState`, `need.*` events, `tests/fixtures/glass-orchard-pack` |
| 外驱/结构上下文 | 已实现结构化 Stimulus/Constraint、到期、习惯化和快照；结构演化治理、沙箱和版本化激活均已接入 | `DecisionContext.stimuli`, `constraints`, `WorldService::projectStimuli`, `src/evolution/evolution.*` |
| LLM 解释与权衡 | 同步对话/自驱提案、channel 策略、Qwen 结构化输出、两阶段话语理解、失败关闭和异步 TurnQueue 已完成；`DecisionContextProjector` 按主体可见性拆成 `worldSnapshot/selfState/planCatalog`，候选计划暴露 BT 摘要与参数 Schema；意图参数校验失败最多反馈修正一次，演化使用强制 function call 且校验错误最多反馈修正两次；`runtime_metadata` 单独承载实例/内容/提示/Revision 审计身份，不污染 NPC 世界事实投影 | `src/provider/decision_context_projector.*`, `IntentProposal.parameters`, `DecisionContext.runtimeMetadata`, `IProvider`, `TurnQueue`, `decision_channels`, `WorldService::converse/deliberate/pumpAsyncTurns` |
| 长过程执行 | 数据驱动时长、单一活动槽、配方声明的输入预留、完成/挂起/恢复事件和 Snapshot 恢复均已实现；挂起释放预留，刺激清除且资源可用时按原计划恢复并顺延完成时间；配置允许的高优先级过程可抢占低优先级预留，整组仲裁原子提交；Goal、Process、FSM、BT 是同一活动的四个投影 | `ProcessExecution.priority/preemptionPolicy`, `NpcActivity`, `reservation_mode`, `WorldService::planResourcePreemption`, `src/npc/state_machine.*` |
| 通用资源与实例活动 | 通用资源数量、可选输入预留、声明式抢占、实体库存、资源转移、生产、原子交易、承诺和定时 Activity 已实现；Activity 可组合已注册 operator 并在嵌套事务中失败补偿 | `ResourceContent`, `ResourceState(amount,reserved)`, `WorldState.inventories`, `GameInstanceContent.inventoryInitial`, `reservation_mode`, `priority`, `preemption_policy`, `tradeResources`, `ActivityContent`；承诺关系增量与重谈时长已由 `promise_policy` 驱动 |
| 社会关系与组织 | 已实现 NPC/玩家/组织四轴关系、声明式社会组成员与指标、交易和快照恢复；港区与 Glass Orchard 分别使用自己的组织、阵营和制度事实，没有 Runtime 特例 | `NpcState.relationships`, `WorldState.socialGroups`, `SocialGroupContent`, `relationship.adjust`, `trade.execute`, `tests/fixtures/port-pack`, `tests/fixtures/glass-orchard-pack` |
| 元动作语义目录 | 已冻结；故事动作 ID 由内容包绑定到通用 operator，行为配方在运行时编译为 BT；Recipe 可声明参数 Schema，`IntentProposal.parameters` 经校验后绑定到通用能力（含动态移动目标）并随过程保存 | `src/world/meta_actions.*`, `src/npc/behavior_recipe.*`, `src/npc/action_validator.*`, `data/behaviors/village.json` |
| NPC 基线日程与策略演化 | 已实现确定性小时边界投影；Level 1 的 `overlay_schedule` 经过验证、沙箱、独立审批和版本化 Overlay 后才生效，临时偏离仍通过行动表达 | `NpcContent.schedule`, `NpcState.location/scheduleLabel`, `WorldService::processHourBoundary`, `EvolutionManager::applyOperations` |
| World Intelligence 演化提案 | 提案持久化、来源事件证据、验证、沙箱、版本化 overlay 激活/恢复/栈式回滚、内容声明异常检测和反事实场景实验已完成；配方节点引用和整批 overlay 原子性均由 Runtime 校验 | `src/evolution/evolution.*`, `src/evolution/anomaly.*`, `src/evolution/scenario.*`, `src/world/world_service.*`, `data/schemas/evolution-proposal.schema.json`, `data/schemas/scenario-proposal.schema.json`; 校验白名单、EventLog 来源、治理类别、禁止 Core mutation、独立审批、快照重放和结果 JSON 审计已覆盖 |
| 3D 表现和 Blender 场景 | 已完成内容化投影 | `src/client_main.cpp` 从当前内容包读取空间实体、连接、NPC、资源、场景资产与相机模式，无村庄 `world_id` 或角色分支；Xvfb 烟雾测试覆盖村庄和 Glass Orchard |

## Runtime 契约收敛队列

这组工作的优先级高于继续增加 NPC、剧情或需求类型。判断依据是事实来源、过期决策、恢复和可验证性；不能把已有广覆盖设计当成生产级 Runtime。

| 优先项 | 当前状态 | 完成条件 |
|---|---|---|
| Revision 与世界时钟 | 已完成本次修正 | `world_revision` 每个已提交根状态转换只增加一次；无事件的分钟推进会令旧异步提案失效；同事务事件共享 Revision；回滚不推进 Revision |
| Activity 生命周期不变量 | 已完成本次修正 | `proposed` 只能是空槽；非空 Goal/Process 身份和阶段投影必须一致；Snapshot 遇到非法组合直接拒绝 |
| 提交效果 Replay | 已完成最小闭环 | 显式生成 Replay Package 时保存基线、聚合 JSON Patch、事件范围、Revision 和状态摘要；`replay()` 实际应用 Patch、追加事件并验证最终 Snapshot，也能读取显式 durable checkpoint 中嵌入的 Replay；仍明确不是 Event Sourcing，逐领域事件 reducer 属于后续 |
| Persistence Adapter | 显式 durable checkpoint 与 opt-in 周期/日边界策略已完成 | `IWorldPersistence` 同时支持默认 `JsonFilePersistence` 与构建可选的 `SqlitePersistence`；`WorldService::saveDurableCommit` 以单个检查点保存 Snapshot、Replay、Revision、事件、幂等和 Overlay 投影；`DurableCheckpointPolicy` 默认关闭，只在已提交调度边界写盘，每个内存事务自动 durable commit 仍不启用；headless CLI 可用 `--durable-checkpoint PATH` 配置日边界或 `--checkpoint-every-minutes N` 配置周期写盘，JSON 适配器会创建外部 run 目录 |
| WorldService 职责拆分 | 进行中：事务、事件、调度协调器、异步协调器、只读决策投影、Entity 投影、通用资源/社会/认知/外驱系统、行为执行、合同协调、NPC 活动、World Intelligence、演化和安全系统已抽出；Snapshot/Replay 持久化协调已移到独立编译单元 | `TransactionManager`、`EventJournal`、`SimulationScheduler`、`SimulationCoordinator`、`EvolutionCoordinator`、`IntelligenceCoordinator`、`DecisionContextProjector`、`EntityProjectionSystem`、`ResourceSystem`、`SocialSystem`、`CognitionSystem`、`StimulusSystem`、`BehaviorExecutionSystem`、`MetaActionExecutor`、`ContractSystem`、`NpcSimulationSystem`、`CommandDispatcher`、`WorldIntelligenceSystem`、`WorldEvolutionSystem`、`SecuritySystem` 已独立；知识/经验均通过 `CognitionSystem` 校验已提交来源事件并写入审计事件；外驱投射、习惯化、容量淘汰和到期由 `StimulusSystem` 统一处理；EntityRegistry 只从已提交 WorldState 重建或同步，不拥有事实；本阶段把 `advanceMinutes/advanceTo` 的边界选择、时间事务、活动/合同协调、提交后 Provider pump 和 durable checkpoint 协调提取为 `SimulationCoordinator`；`evolution_policy` 配置的 Level 1/2 日边界提案已由 `EvolutionCoordinator` 排入独立串行队列，在主线程重新校验并运行 Sandbox，结果写入审计但不自动批准 Overlay；这些系统只通过 WorldService 的统一事务和事实回调提交，不拥有第二份 WorldState；不做微服务 |
| 事件驱动调度 | 已完成原型闭环 | `advanceMinutes` 选择下一个小时/日程、NPC 唤醒、过程完成、刺激到期或合同截止边界；安静分钟合并推进但保留逐分钟 Revision 语义，远场回归验证边界数显著低于模拟分钟数 |
| ECS 定位 | 已冻结为投影模型 | `WorldState` 是事实权威，`EntityRegistry` 是当前已知类型的查询投影；自定义 Component 进入权威持久化前，不宣称支持通用动态 ECS |
| 行动效用分项 | 已完成最小闭环 | `utility_profile` 可声明资源成本、风险、关系影响、未来价值和结构惩罚；范围校验、确定性合成和审计分项已接入；复杂社会效用模型仍由内容扩展 |

开放评审中的正确判断包括：当前 `writeReplay()` 的 Snapshot 已包含最终事实，事件行没有参与重建；`WorldService` 体量过大；分钟级全量调度不扩展到大型世界；内存事务不等于持久化事务；内容与规则可组合不等于 LLM 可新增任意 C++ Primitive。

## 已完成验证

- 任意最小内容包可以启动，不依赖 Mira、Lorn、Eve 或村庄专用 API。
- 实体创建、休眠、激活和销毁经过事务，销毁身份保留在事实层并可从 Snapshot 恢复。
- Promise、资源、关系、NPC 日程和事件在两日离线推进中可观察。
- 默认产品路径使用 LLM；LLM 不可用时，时间、衰减和已批准过程仍可恢复，但自驱提案和玩家对话都失败关闭，不用确定性候选冒充模型决策、不猜测语义、不写事实。Mock Provider 仅由测试和离线回归显式注入。
- Snapshot/Replay 不访问 LLM；事件只在事务提交后发布。Snapshot 用于普通读档，Replay 只在显式生成时做一次聚合状态重建和审计校验。
- 每个事件都有稳定 `event_id`，并冻结提交时的内容与模拟版本；同事务事件共享 `correlation_id` 并以前序 `causation_sequence` 形成直接因果链。嵌套保存点失败时暂存事件与因果位置一起撤销，Snapshot 会拒绝越序、跨链或缺少版本来源的事件。
- `event_sequence` 与 `world_revision` 已分离：时间推进即使没有业务事件也推进 World Revision；同根事务事件共享同一个 Revision；异步上下文绑定事务即将提交的 Revision，避免边界刚完成就把自身判为过期。
- Activity Snapshot 不再静默归一化非法 Goal/Process 投影；`proposed` 空槽、身份一致性和阶段投影均有严格校验。
- 候选行动的多需求收益按每个需求的 `effective_weight × expected_relief × evidence_confidence` 求和；排序分数和分项贡献进入 LLM 候选上下文与意图事件。
- 需求缺口会随游戏时间增加，已提交动作会产生 `need.relieved`，跨阈值会产生 `need.critical`，需求状态可保存和恢复。
- 运行态 NPC 只保存 `definition_id`；快照引用已删除的内容或不同 `content_version` 时会明确拒绝加载。
- 第二个最小内容实例（`tests/fixtures/minimal-pack`，NPC `sable`）可以独立加载、保存和查询实体投影，证明 Runtime 不依赖村庄角色。
- 第三个独立小说实例（`tests/fixtures/glass-orchard-pack`）使用自定义需求域、两个区域、两个阵营和一个制度协定运行，并在一天后产生承诺违约的关系余波；headless 的标题、对话 NPC 和回放内容根也从当前实例读取。
- 3D 客户端在默认构建和 raylib 构建中均通过编译；Xvfb 实测村庄与 Glass Orchard 都能完成窗口、OpenGL 和内容初始化并连续绘制。村庄 GLB 只在内容声明 `scene_asset` 且显式设置 `TIDE_MARKS_USE_GLB=1` 时加载。
- 承诺的关系轴增量和重谈延长时长来自 `promise_policy`，修改内容即可改变结果。
- 行为树由 `recipes` 自动构造；Mira、Lorn、Eve 与故事动作没有专用 C++ BT。
- NPC 普通决策选择完整的已登记 BT 计划，不直接选择元动作；新 BT 只能由 Level 1/2 的 `register_behavior_recipe` 提案创建，必须经过节点/operator 引用校验、沙箱和独立批准。
- 行为参数由内容声明 JSON Schema；动态 `goto` 目标通过 `{"bind":"parameter_name"}` 绑定，Runtime 校验实体生命周期、权限和拓扑后才执行，并把已接受参数保存到 `ProcessExecution`。
- Level 1/Level 2 的真实 Qwen function-call 回归均已通过；Level 1 返回策略 Overlay，Level 2 返回声明式内容/能力提案。Provider 校验失败最多触发两次带 `code/message/path` 的修正请求，超过次数失败关闭。
- 对话与自驱由 `decision_channels` 声明；通道属性、话语权限、事件/记忆投影和 Provider 策略不再依赖 trigger 字符串分支。
- 嵌套配方失败只回滚自己的保存点，不会把所在世界时间步一起恢复；快照回滚不复制不可变事件历史。
- 冲突与安全 operator 的 attack、sabotage、threaten、defend、evacuate、repair 均覆盖事实变化、主事件与 Snapshot；关闭的 authored topology 会阻止移动，生产输入不足或输出超重不会留下部分资源或事件。
- Overlay 的 Behavior Recipe 只有在条件、元动作和节点 ID 都能解析时才可进入 Sandbox；多操作 Overlay 只有全部操作成功才整体交换内容视图。`CommandResult.event` 固定返回本次命令的主事件，不会被子事件或投射刺激覆盖。
- 核心测试、可执行程序构建和三个独立内容实例的 CLI 回归以当前机器实测耗时为准；单测耗时输出用于防止长期模拟回归。

## P0 工作顺序

### P0-J：内驱、外驱与自动适应（已完成）

1. 保持 `NeedDefinition/NeedState -> DriveSystem -> NeedEvent -> GoalSelector` 的确定性链路。
2. ~~把需求域收口~~ 已完成：村庄使用 `physiological|psychological|social|cognitive` 作为默认内容分类；加载器与 Schema 接受任意非空领域标签，每项具体需求仍必须绑定一个或多个可执行满足配方。
3. ~~实现显式 `relieve_need` 元动作~~ 已完成：由配方和来源事件驱动，LLM 不能直接设置缺口或权重。
4. ~~把外驱从文本拼接改为 Stimulus~~ 已完成：包含来源、效价、紧迫度、新颖性、可见范围、置信度、到期时间和显著性。
5. ~~实现有界预期适应~~ 已完成：学习率、步长、冷却期、版本、`drive.adapted` 和 `stimulus.habituated` 均进入 Snapshot/Replay；下一步才增加敏感度学习。
6. ~~在文本界面显示驱力上下文~~ 已完成：显示所有内容声明的需求域、deficit、drive、临界状态、外驱和约束。
7. ~~增加需求和适应回归~~ 已完成：覆盖衰减、临界、满足、适应、快照恢复和最小内容包。
8. ~~清理剩余内容特例，使 P0 行为通过内容包动作和注册规则表达，避免把 NPC ID 写进 Runtime 算法。~~ 已完成；话语行为只由内容包声明语义说明和权限，在线理解由 LLM 完成，Runtime 与 Mock Provider 都不再匹配玩家关键词。
9. ~~将当前村庄资源字段收口为通用资源组件/资源存储。~~ 已完成；生产、交易、合同和通信对象的完整语义扩展属于 P1。

### P0-N：结构自动演化（治理基线已完成）

1. ~~将 `StructuralConstraint` 从字符串改为可引用的事实投影，覆盖访问、权限、所有权、容量、配方、合同和信息范围。~~ 已完成当前 P0 的资源、拓扑、能力白名单投影；更丰富的制度字段留给 P1。
2. 已声明的常规结构变化走现有事务；新的规则/能力/配方/类型走 `EvolutionProposal`。
3. ~~P0 的声明式结构提案必须由独立的开发/设计审批策略批准；LLM 可以提出，不得批准自己的提案。~~ 已完成 `EvolutionManager::approve`。
4. Schema/不变量校验和审批记录、密封回放沙箱、指标与版本化 overlay 激活均已完成；LLM 仍只能产生提案，独立审批后才可激活。

### P0-K：意图和行动契约（已完成 P0 基线）

1. 将 Provider 结构化响应固定为 `IntentProposal`：候选动作、意图、理由、预期结果、引用 Stimulus、权衡、预期满足、置信度、上下文 Revision。
2. 校验模型返回的上下文版本、候选白名单、资源、权限、关系和幂等键。
3. ~~让一次有效决策启动持久化为 `ProcessExecution`，过程中不重复调用 LLM~~ 已完成：候选行为声明时长、可中断策略和显著性阈值；完成/中断才提交对应效果。`NpcActivity` 现在是唯一活动槽和生命周期状态源，Goal/Process/FSM/BT 不再各自维护生命周期。
4. 已提交行动统一形成 WorldEvent，并驱动资源、需求、记忆和下一轮外部刺激；更丰富的关系/合同元动作属于 P1。
5. ~~把真实 Provider 的自主审议接入异步 TurnQueue~~ 延后到 P1；当前同步路径只用于受控推进，不能被误称为非阻塞调度。
6. ~~对模型关闭、模型迟到、过期提案、未知动作、重复提案、过程恢复和中断释放预留增加回归测试~~ 已覆盖；对话和自驱 Provider 失败时都失败关闭且不提交事实，测试通过显式 Mock Provider 保持可重复。

P0-K 的提案校验、对话/自驱失败关闭、过期提案、重复提案、过程恢复、嵌套事务回滚和中断释放预留已有回归覆盖。异步 Provider TurnQueue 属于 P1；当前同步路径只服务于受控推进。

### P0-L：小村庄因果演示（已完成）

1. 用文本界面展示“资源变化 → 内/外驱 → 意图 → 行动 → 他人/公共资源变化 → 次日计划变化”。
2. 记录每个节点的事实来源和规则原因，支持事件检查和回放差异。
3. 保持一个共享模型槽位；不让 LLM 阻塞游戏时间，也不让 LLM 直接修改 WorldState。
4. P0 通过后再进入港区内容，不提前实现商店、利率或政策沙箱。

### P0-M：世界对象与元动作建模（P0 基线已完成，扩展项进入 P1）

已完成：运行态 NPC 只保存 `definition_id`，快照不复制内容定义；承诺关系增量与重谈期限由 `promise_policy` 数据驱动；实体 ID 前缀和游戏时间单位由共享运行时常量生成；Game Instance 清单与 Content Pack 清单分开；快照保存并校验 `instance_id`；第二个最小实例验证替换性。

1. ~~冻结 Entity/Agent、Resource、ProductionProcess、ConsumptionNeed、Exchange/Contract、Communication、Institution/Rule 和 Event 的属性契约~~ 已完成 P0 的基础字段和 JSON 边界；完整交易、制度和生产字段继续在 P1 逐项落地。
2. ~~让内容加载器校验单位、所有权、容量、输入/输出、时间窗口、权限和引用~~ 已完成当前资源、拓扑、需求、行为配方和引用校验；质量、所有权、交易输入/输出的完整语义属于 P1 operator。
3. ~~完成 P0 元动作注册表：资源消耗/产出/预留、需求满足、关系调整和连接状态~~ 已完成；当前已注册执行器为 `resource.consume`、`resource.produce`、`resource.reserve`、`resource.release`、`need.relieve`、`relationship.adjust`、`link.set_state`。资源转移、移动、承诺和知识动作保留为 P1 operator，不提前写入可加载内容。
4. ~~把小村庄真实的配给、修船、档案整理和承诺行为组合为 BehaviorRecipe；不再为每个 NPC 添加 C++ 特例。~~ 已完成：`data/behaviors/village.json` 定义 meta actions、conditions 和 recipes，Runtime 编译 Sequence/Selector/Condition/Action/Timeout/Interruptible 节点；ActionCandidate 只引用配方，ProcessExecution 在完成时执行同一配方。
5. ~~为元动作、复合行为、事务失败和回滚补自动化测试~~ 已完成。新增商店、利率或政策规则前，必须先完成对应的 P1 事实模型。

## 后续工作包

### P1：异步决策与社会模拟扩展

按以下顺序推进：

1. ~~**异步决策队列**：Provider 在独立工作线程运行；请求保存 channel、World Revision、NPC State Version、内容/提示版本和幂等键；返回后只接受仍然有效的提案，过期提案进入审计日志，不阻塞 World Tick。~~ 已完成：`TurnQueue` 使用单独工作线程执行 Provider；主线程通过 `pumpAsyncTurns()` 在世界边界提交，revision、NPC state version、content/prompt version 任一变化都会拒绝事实提交并追加 `npc.intent_expired`；Provider 异常或校验失败追加 `npc.intent_rejected`。同步对话仍使用同一 `commitTurn` 校验路径。
2. **已注册 operator 扩展**：全部检查点已完成。`resource.transfer` 使用实体库存和公共资源池并原子提交；移动校验活动拓扑；`conflict.attack/sabotage` 与 `security.threaten/defend/evacuate/repair` 改变安全事实并产生刺激；`promise.create/transition`、`exchange.create/transition/settle`、`knowledge.assert`、`experience.record` 和 `production.execute` 均已有命令或配方路径、事件与失败原子性回归。资源抢占由配方 `priority` 和 `preemption_policy` 配置，先规划全部受害者再原子暂停，快照保存仲裁事实；抵押、价格和复杂仲裁仍需后续内容模型。
3. ~~**潮门港纵向切片**：增加交易、家庭/组织指标、阵营关系和近远场更新，仍使用同一 WorldState、ProcessExecution、EventLog 和 Replay 契约。~~ 已完成最小可替换切片：交易、组织指标、跨 NPC 关系、实例库存、近远场更新和 Snapshot 恢复均由 `tests/fixtures/port-pack` 验证。
4. ~~**验证与性能**：增加多个实例并行离线推进、事件数量/内存指标和 `advance_to` 调度边界测试；保持本地三 NPC 回归在可接受时间内。~~ 已完成基线测试：多个实例共享 `advanceTo`，记录近/远场唤醒、跳过检查、模拟分钟和 Snapshot 序列化大小。

验收标准是同一 Runtime 加载港区内容后，交易和社会变化能由事件链解释，暂停/恢复/回放不调用 LLM，远端实体不会逐帧模拟。

### P2：World Intelligence 演化和沙箱（当前检查点已完成）

Evolution Proposal Schema、提案库、EventLog 来源事实、回放沙箱、指标评估、审批策略、版本化 overlay、激活、快照恢复和栈式回滚已完成。提案的 `kind` 必须来自治理注册表，`evidence_event_sequences` 必须引用当前世界中已提交且不重复的事件，无法解析的证据在进入 Sandbox 前即被拒绝。`WorldEvolutionSystem` 现在向 Level 1 上下文提供有界的具体知识/经验条目及其来源、置信度和触发/结果字段，Level 2 只接收受权限裁剪的跨主体摘要；条目计数不再被当作演化证据。内容包可声明 `resource.below/above`、`need.above`、`group_metric.below/above` 和 `promise.overdue` 异常规则；`AnomalyDetector` 输出带规则、观测值、阈值和最近事实事件证据的 JSON。`ScenarioProposal` 只允许对已建模的资源、组织指标和 NPC 需求做有边界的 delta，要求精确的内容版本和世界 Revision，从同一 Snapshot 分别运行 baseline/scenario，并输出指标、数值差、两侧事件和结果 Snapshot。CLI 的 `--anomalies` 与 `--experiment` 可复现实验，源世界不被修改。小店、利率和政策仍不是已实现机制；没有对应事实模型时会被拒绝，不能由 LLM 直接生成数值结果。

### P3：可替换世界实例

纵向切片已完成。`tests/fixtures/glass-orchard-pack` 使用同一 Runtime 加载新的小说世界：两个命名区域经两条通用拓扑连接，两个阵营与一个制度协定使用社会组事实，资源交换使用通用交易 operator，stewardship/ecological 需求域由内容声明，逾期承诺在一天后改变关系和事件历史。该实例不经过村庄、港区或角色专用代码；Snapshot 恢复和 headless replay 都已接入回归。

下一阶段扩展的是内容尺度和校准，不是替换执行模型：更多区域与组织、任务内容、经济与制度规则需要通过同一 Content Pack / Evolution Proposal 契约加入；现实世界实验必须定义对应实体、数据边界、校准数据和失败标准后才能运行。

## 目标总表（按状态）

这张表列出项目目标的完整状态。标记“纵向完成”表示契约已经在可运行实例、回放和测试中闭环；它不表示所有产品内容或所有规模问题都已完成。

| 目标 | 当前状态 | 验证或下一步 |
|---|---|---|
| Runtime Core：事实、事务、Revision、事件、调度、实体生命周期、快照/回放 | 纵向完成 | 继续保持单一 WorldState 写入边界；JSON 与可选 SQLite durable checkpoint 已接入，逐根事务自动 durable commit 仍需更强的崩溃恢复策略 |
| Content Pack / Game Instance / World Runtime State 分离 | 纵向完成 | `data/README.md` 固定静态输入与运行存档边界；继续用第二、第三实例验证可替换性 |
| 内驱：需求衰减、临界、满足、预期适应、多需求排序 | 纵向完成 | `DriveEvaluation` 已进入排序和 LLM 上下文；后续增加成本/风险的通用内容字段 |
| 外驱与结构：Stimulus、Constraint、权限、异常和安全事实 | 纵向完成 | 继续扩展可声明事实类型，不用文本关键词代替模型理解 |
| NPC 活动生命周期：Goal / ProcessExecution / FSM / BT 单一活动槽 | 纵向完成 | 保持 `NpcActivity` 为唯一阶段所有者；候选 Goal 集合是后续扩展 |
| LLM Intelligence：话语理解、权衡、意图、异步队列、截止和重新校验 | 纵向完成 | 真实 Qwen 对话、自驱、Level 1/2 已验证；自动演化日边界由配置驱动并经过异步 Queue、当前事实校验和 Sandbox；不让 Provider 写事实 |
| P0 小村庄因果验证 | 纵向完成 | 资源→需求→意图→行动→关系/记忆→次日选择闭环已覆盖 |
| P1 社会与通用 operator | 纵向完成 | 合同、交易、生产、移动、安全、知识/经验和资源仲裁已有最小切片 |
| P2 World Intelligence 演化治理 | 纵向完成 | Proposal→Validate→Sandbox→Approve→Overlay→Observe 已覆盖；Runtime Core 不在线热改 |
| P3 可替换世界实例 | 纵向完成 | Port 与 Glass Orchard 已证明内容替换；继续增加内容规模而非 C++ 特例 |
| 持久化生产化 | 原型阶段部分完成 | JSON 仍是默认开发/测试格式；可选 SQLite 已能以 `saveDurableCommit` 事务化保存 Snapshot/Replay 及其状态索引。满足 ADR 触发条件后，再把每个根事务的提交自动接入同一 durable commit |
| 商店、价格、利率、税费、政策和复杂仲裁 | 尚未建模 | 先定义对应事实、主体、规则和校准数据，再接入 Scenario/Evolution |
| 大规模社会校准、多模型并发、分布式 Runtime | 尚未开始 | 不进入当前小村验证门槛 |
| Runtime Core 自主修改/任意代码热加载 | 明确禁止 | 只能由开发者通过代码、测试、迁移和发布演化 |

## 后续目标清单

| 优先级 | 剩余目标 | 完成条件 |
|---|---|---|
| P1-1 | 异步 Provider TurnQueue | 已完成：模型请求不阻塞 World Tick；过期结果可审计且不改事实 |
| P1-2 | 资源转移、预留/抢占、移动、冲突/安全、承诺/合同、知识和生产 operator | 已完成基础 operator 与事务/事件/回放契约；合同提案/接受/结算/过期、双侧失败原子性、参与方生命周期复检、失败审计和文本查询已覆盖，抵押、价格和复杂仲裁仍是扩展项 |
| P1-3 | 潮门港社会切片 | 已完成最小切片：交易、组织/阵营关系和近远场模拟形成可回放因果链；家庭、任务和 UI 属于后续内容扩展 |
| P1-4 | 调度与性能基线 | 已完成：`advance_to` 边界、多个实例离线推进、近/远场唤醒、安静分钟合并、跳过检查和快照大小有回归基准 |
| P2-1 | Evolution Sandbox | 已完成：从快照运行候选变化，输出指标、事件差异和来源，不污染原始 Pack |
| P2-2 | Overlay 激活与回滚 | 已完成：内容/策略/日程版本可审计、可观察，沙箱与提案必须锁定当前 Revision，快照恢复后仍可按栈回退 |
| P2-3 | 场景实验 | 已完成当前通用边界：资源、组织指标、NPC 需求的声明式反事实实验；小店、利率、政策等具体机制待其事实模型落地后再接入 |
| P2-4 | 自动 LLM 演化边界 | 已完成：`evolution_policy` 配置 Level 1/2 日间隔；`EvolutionCoordinator` 在已提交边界异步排队，结果按当前事实重新验证、Sandbox 并审计，不能自动批准或激活 Overlay；Mock Provider 不冒充演化 |
| P3 | 新世界实例 | 已完成 Glass Orchard 纵向切片：第三个实例、跨区域、阵营、制度、自定义需求域和长期承诺余波均由内容驱动 |

## 自动化验证命令

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --days 2 --provider mock --dump-events
./build/tide_marks_text --script docs/examples/village-session.txt --provider mock
./build/tide_marks_text --content-root tests/fixtures/port-pack --script tests/fixtures/port-pack/exchange-session.txt --provider mock
./build/tide_marks_headless --days 0 --profile data/intelligence-profile.json --provider profile
./build/tide_marks_headless --anomalies --days 0
./build/tide_marks_headless --content-root tests/fixtures/port-pack --experiment docs/examples/port-morale-scenario.json
./build/tide_marks_headless --content-root tests/fixtures/glass-orchard-pack --days 1 --provider mock --dump-events
./build/tide_marks_text --provider profile --script docs/examples/llm-self-drive-session.txt
```

每次工作包完成后记录：内容版本、规则/模拟版本、Provider/模型（如有）、随机种子、命令、WorldEvent、Snapshot 截断点、Replay 摘要和失败原因。不得把 API Key 写入日志或回放工件。

## 风险与当前处理

- LLM 延迟或断开：世界时间和已批准规则可继续推进；自驱和玩家对话返回错误且不提交事件或行动，模型不拥有提交权限。离线回归必须显式选择 Mock Provider。
- 自驱阈值与异步可见性：`min_drive_pressure` 以 `drives[].drive`/`effective_weight` 为单位，必须由每个内容包校准；默认村庄阈值已调到能在开局首个小时触发真实 LLM。World Tick 不阻塞模型，headless 只有显式 `--wait-llm-ms` 才等待有限时间并泵出结果。
- 内驱过于写死：参数、需求、动作满足关系和阈值来自 JSON；新驱动力必须走提案和沙箱，不在 C++ 添加 NPC 特例。
- LLM 越权：候选动作、结构化 IntentProposal、版本检查和 ActionValidator。
- 世界事实分叉：单一 WorldState 写入入口、事务、EventLog 和提交后 EventBus。
- 回滚边界混淆：校验拒绝、复合配方 savepoint、长过程补偿和 Overlay 版本回退分别记录；任何一种都不删除已经提交的历史事件。
- 内容扩大后性能下降：近场/远端模拟 LOD 和事件唤醒，不让所有实体逐帧模拟。

### 本地 Qwen 验证记录

使用 llama.cpp 的 `Huihui-Qwen3.5-4B-abliterated.Q4_K_M`、OpenAI-compatible `/v1/chat/completions` 和 JSON Schema 约束验证 P0 意图输出。玩家对白现在先调用 `utterance_understanding`（thinking + 128 token 上限）依据内容包中的话语行为说明识别通信意图，再调用关闭 thinking 的 `player_dialogue` 生成回答或行动提案；Runtime 不匹配关键词，只根据返回的行为 ID 和内容策略重新校验权限。真实验证中，"Please tell me what you are doing." 被模型识别为 `information_request`，第二阶段返回空 `action_id`，没有资源变化。显式设置 `reasoning_budget_tokens=256` 时，llama.cpp 仍正常结束响应，服务端生成量为 426 tokens，说明该字段确实限制思考段而不是被模板静默忽略。Prompt 同时明确本次请求允许的刺激 ID 和需求 ID，空刺激时强制 `cited_stimulus_ids=[]`。

当前基准的推荐档位是：话语理解使用 `reasoning_budget_tokens=128`，普通回答关闭 thinking；有社会权衡的自驱决策使用 `reasoning_budget_tokens=128`；低置信度才升级到 256；512 以上留给演化分析。该分档基于同一模型的三类村庄提示实测，而不是单看生成长度。

Provider、模型和三档预算现在由 `data/intelligence-profile.json` 配置，并通过 `--profile` 加载。真实自驱验证中，玩家对白使用 `utterance_understanding` + `player_dialogue` 两阶段；当 Lorn 的 crossing safety 越过临界阈值后，`high_salience_context` 自动进入 `autonomous_reappraisal` 档，模型选择 `repair_ferry` 并提交了资源变化和过程完成事件。最新三 NPC 回归使用 `docs/examples/llm-self-drive-session.txt`：一次 `advance 60` 后，Eve、Lorn、Mira 各收到一次真实 Qwen 请求，均在安全边界进入内容声明的 `ProcessExecution`，未由 Provider 直接写入事实；运行耗时和结果记录在 [`qwen35-4b-llama-cpp.json`](evidence/qwen35-4b-llama-cpp.json)。

## 恢复工作入口

会话恢复时依次阅读本文件的“当前检查点”和“P0 工作顺序”，再阅读 [design.md](design.md) 的“事实、意图与演化边界”“世界与社会的内在驱力模型”“NPC Runtime 与 World Intelligence”“Runtime 核心契约”。然后查看 `git status --short`，运行自动化验证命令，先修复当前工作包失败项，再继续下一项；不要为了通过测试删除功能或放宽断言。
