# 《潮痕》Adaptive World Runtime 总设计

> 这是唯一的总设计文档，统一记录产品定位、世界模型、内在驱力、NPC/World Intelligence、Runtime 契约、内容格式、存档回放和路线。实现状态、任务顺序和验收结果只写入 [release-continuation-plan.md](release-continuation-plan.md)。

## 1. 定位与目标

《潮痕》首先是一个能够持续运行、记住历史、形成因果并在边界内演化的 Adaptive World Runtime。临潮群岛和三 NPC 小村庄是第一个世界实例，用来验证平台和模型；它们不是 Runtime 的内置知识。未来同一个 Runtime 可以加载不同小说背景，或者加载具有足够实体、资源和规则定义的现实世界简化模型。

第一目标不是做大地图，也不是让 NPC 说更多话，而是用一个很小的故事背景验证：

- 世界在没有玩家时仍然推进；
- 社会由资源、需求、关系、承诺和规则产生持续驱动力；
- NPC 的意图能改变世界，世界结果又会改变下一轮意图；
- 每一次变化都有事实、来源、事件和可回放结果；
- LLM 能理解情境并提出有用的意图、策略和演化提案，但不能越过 Runtime 直接写事实。

产品的长期特征是持续性、自主性、记忆性、涌现性和受治理的演化。玩家可以观察、参与和干预，但玩家不是世界运行的前提。

《潮痕》的第一体验是一个可持续运行的小村庄：资源变少会制造需求缺口，需求缺口推动 NPC 形成目标，目标促成行动和协商，行动改变资源与关系，结果留下事件和记忆，并在下一天改变计划。这个闭环稳定后，才扩展潮门港、阵营、主线和 3D 表现。

玩家在完整实例中的循环是探索、发现、交涉、承诺、兑现或失信、回看后果。自由输入和行动选项都必须落到作者定义的询问、谈判、撒谎、交换、保护、调查或结束意图；承诺进入承诺簿，时间推进后由世界返回价格、线索、路线、盟友、敌人和新的生活状态。模型负责表达差异，规则系统负责决定承诺和后果是否成立。

## 2. 事实、意图与演化边界

世界有三种不同层次，不能混成一个“智能对象”：

| 层次 | 内容 | 权威者 |
|---|---|---|
| 事实层 | 时间、位置、资源、需求状态、关系、承诺、实体生命周期、已提交事件 | WorldState + WorldService |
| 意图层 | 当前想达成的结果、社会策略、谈判立场、行动候选和表达方式 | NPC Runtime 与 World Intelligence 提案，Runtime 验证 |
| 演化层 | 新需求、新指标、新内容实体、新行为配方、新策略和新规则组合 | Proposal Validator + Sandbox + Evolution Manager |

“事实由平台维护、意图由 LLM 维护”是正确方向，但意图也不能直接改事实。完整链路是：

```text
WorldState facts
  -> Perception / Memory projection
  -> LLM appraisal: explain, weigh, rank, propose IntentProposal
  -> candidate, permission, version, resource and rule validation
  -> Command
  -> atomic WorldState commit
  -> WorldEvent / EventLog
  -> next perception and memory
```

底层事实、衰减、资源变化和规则由 C++ Runtime 维护；LLM 不负责维持事实的第二份副本。IntentProposal 不是世界事实，只是带依据、候选 ID、预期结果和不确定性的决策提案。确定性的日常动作可以跳过 LLM，复杂社会情况才调用它。

已有并获准的需求模型在运行时自动推进，不需要人工逐分钟批准。LLM 在已注册候选中选择交互意图，也不需要人工逐句批准，但必须通过版本、权限和业务规则校验。

LLM 提出新的需求类型、驱动力曲线、集体指标、行为配方、内容实体或规则组合时，才进入演化治理：`Proposal -> Validate -> Sandbox/Replay -> Approval Policy -> Versioned Activate -> Measure -> Rollback/Retain`。P0 使用开发者/世界设计者明确批准；以后可以让低风险声明式提案按策略自动批准，高风险提案仍需人工批准。事务、权限、事件格式、存档、调度、组件存储和其他 Runtime Core 永远不能由 LLM 在线修改，只能由开发者改代码、测试、迁移和发布。

## 3. 世界实例与内容边界

Runtime 不认识 Mira、村庄、潮门港或具体剧情。世界内容包用 JSON 声明实体原型、空间拓扑、资源、日程、需求、行为配方、事件条件和对白；Game Instance 组合开局位置、初始关系、剧情入口、视觉资产和玩家体验；World Runtime State 保存当前事实；Intelligence Profile 定义感知投影、记忆策略、Provider、工具白名单和演化策略。

JSON 是内容和提案的机器边界，C++ 类型是编译期契约，WorldState 是某个实例的当前事实。它们不是两份可写的游戏状态。加载时 JSON 只初始化和校验；运行中只改 WorldState；EntityRegistry/Component 是已提交事实的投影；存档保存实例状态。项目尚未发布，字段变化直接同步修改代码、数据、测试和文档，不保留旧字段别名或兼容解析路径。

临潮群岛的后续内容空间包括潮门港、旧环城、风脊高地和盐沼荒原。灯塔公署追求完整恢复旧网络，自由潮盟只恢复必要通信，静默会主张关闭危险核心。三方不是简单善恶选项，而是秩序、自治和不确定性之间的不同代价。第一阶段只实现小村庄的简单故事；主线和阵营属于后续实例内容，不进入 Runtime 核心。

四个区域的主题分别是交易/走私/难民与消息，档案/身份与旧设施，灯塔/能源与气象，以及采集/护送与失控设备。每个区域提供资源、线索和不同的人际网络；玩家的选择改变区域状态，但一次对话不会直接让模型跳过中间事实。

主线内容仍按三幕设计保留：第一幕“断灯”建立事实共享、承诺和隐藏信息的关系差异；第二幕“三种真相”通过调查和区域目标揭示大静默；第三幕“回潮”在完整回潮、分潮协议和永久静默之间选择。结局由世界状态、阵营关系、承诺和关键 NPC 结果选择作者内容，不能由模型临时创作事实。

结局后进入独立的“回潮后”状态：结局前存档保留，未完成承诺进入余波期限，NPC 继续生活，区域资源、交通、价格和关系受最终方案影响。开始新航程时可继承图鉴、路线和结局记录，但背包、资源、任务、承诺、关系、死亡状态和世界结局重置；残响模式若存在，必须由明确的作者内容处理模糊记忆。

## 4. 世界与社会的内在驱力模型

世界驱力不是一组给 Mira、Lorn、Eve 写死的 `if`，也不是让 LLM 每帧自由发挥。它是可解释、可校准、可被世界智能提案扩展的状态模型。驱力计算和意图审议必须分层：

```text
环境与资源事实
  -> 内驱（需求缺口、内稳态、价值与长期目标）
  -> 外驱（事件、他人请求、截止时间、机会与威胁）
  -> 结构约束（制度、角色权限、价格、库存、道路、风险与规范）
  -> LLM/规则对情境进行解释、权衡并提出 IntentProposal
  -> 资源竞争、交换、合作、冲突或组织协调
  -> 事务事实和事件
  -> 需求、关系、资源和集体指标再次变化
```

内驱是主体内部的连续状态；外驱是世界对主体施加的已记录刺激；结构约束不是驱动力，而是定义哪些意图可以落地的边界。LLM 可以把“粮食减少、邻居求助和承诺截止”理解为一组社会情境，但不能把不存在的粮食、权限或道路写进上下文。

### 个体需求与内稳态

马斯洛只作为作者和调试者的 taxonomy，不是固定五层闸门。可用 taxonomy 包括 physiological、safety、belonging、esteem、autonomy、cognition 和 growth；一个角色可以同时追求安全、关系和真相，低层需求临界时提高行动代价或触发安全约束。

内容包的 `NeedDefinition` 至少包含：`id`、`taxonomy`、`initial_deficit`、`decay_per_hour`、`critical_threshold`、`weight`、`priority`、`satisfiers`。运行时的 `NeedState` 保存 `deficit`、`last_updated` 和 `last_relief_revision`。缺口范围是 `[0,1]`，0 表示满足，1 表示极端缺失。定义还应记录来源（作者初始模型、学习记忆或已批准演化）、版本和适用范围；运行时状态记录最近一次事实来源，便于解释驱动为什么发生变化。已经批准的定义由 DriveSystem 随游戏时间更新：

```text
deficit(t + Δt) = clamp(deficit(t) + decay_per_hour × Δt - committed_relief, 0, 1)
drive = priority + weight × urgency(deficit, critical_threshold)
```

满足只能来自已提交的行为或事件，例如进食、休息、修复、获得安全、完成协作、澄清关系或获得可靠信息。对白、模型自述和未验证的记忆候选不能直接降低缺口。需求跨越临界阈值产生 `need.critical`，时间推进产生 `need.deprived`，已提交满足产生 `need.relieved`；事件记录需求 ID、前后值、行动、规则和 Revision。

当前 P0 采用确定性驱动力内核，确保世界不调用模型也能运行、测试和回放。内驱是主体内部连续状态，外驱是世界对主体施加的已记录刺激，结构约束是意图能否执行的条件。LLM 不替代内稳态计算，而是读取授权的需求、事件、关系和后果，进行语义解释、权衡和候选意图生成；它也可以提出“是否需要新的需求定义”，但提案未通过沙箱和批准前不改变需求模型。

习惯化、累积和预测误差属于后续的驱动力扩展，不能让同一刺激永远产生同样反应。例如反复失信可能增加某个主体的安全需求敏感度，成功协作可能降低同类行动的风险估计。扩展参数由内容定义；LLM 只能解释和提案，Runtime 仍负责数值状态、事件和版本。该模型是用于可信模拟的可解释近似，不声称还原真实人类心理。

### 意图、目标与行动

`GoalSelector` 用需求驱动力、预期满足、资源成本、风险、关系影响、长期目标连续性和时间约束对作者定义的候选行动排序。它提供可解释的候选空间，不要求每个决策都调用 LLM。LLM 在这个空间内进行情境解释、社会权衡、候选排序或提出已注册的 IntentProposal。状态机管理工作、移动、交谈、打断、休眠和不可用模式；行为树执行短计划并返回 Success/Failure/Running/Interrupted；WorldService 最终验证实际结果。

候选行动的效用可表达为：

```text
utility(a) = Σ(drive_i × expected_relief_i)
             + goal_continuity + relationship_effect + future_value
             - resource_cost - risk_cost - constraint_penalty
```

这里的公式是 Runtime 的稳定语义，参数和候选来自内容包。LLM 可以在复杂社会情境中提出 `IntentProposal`，但只能从当前 Context 的 `action_id`、`beat_id`、谈判立场和策略候选中选择；它不能凭空创建资源、任务、关系变化或事件。C++ 校验结构约束后把意图转成 Command，事务成功后的结果才改变世界，并生成下一轮外驱和内驱输入。

### 社会和集体驱力

个体需求通过共享资源和关系汇聚为社会驱力：粮食安全、运输能力、记录完整性、信任、成员流失、公共设施可用性和组织目标。集体指标必须声明参与实体、资源池、目标值、时间窗口、聚合方式、冲突权重和失败后果，不能简单取 NPC 平均值。

P0 只实现个体需求和公共资源，验证“资源变化—行动—他人受影响”的因果链；P1 增加家庭/组织 CollectiveDriver、资源预留和冲突仲裁；P2 才允许 World Intelligence 提出新的社会指标或策略，并通过历史回放沙箱比较效果。自主性、胜任感和关系性作为横向调制因素，不压缩成单一需求层级。

## 5. NPC Runtime 与 World Intelligence

NPC 由身份、物理状态、需求、长期目标、关系、感知、记忆、当前模式、意图和执行计划组成。各层职责固定：

| 层 | 作用 | 决定者 |
|---|---|---|
| Perception | 生成可见事实、来源、时间、置信度和范围 | 规则系统 |
| Needs/Drive | 随游戏时间更新缺口，计算驱动力和临界约束 | DriveSystem |
| Goal/Utility | 选择长期结果和候选行动 | 确定性 Runtime，必要时接受 LLM 提案 |
| State Machine | 管理工作、移动、交谈、打断和休眠 | 规则信号 |
| Behavior Tree | 执行短计划并报告结果 | 注册节点和内容配方 |
| Memory | 由事件生成事实、关系和摘要 | EventLog + MemoryService |
| World Intelligence | 解释复杂情境、权衡社会后果、提出意图、策略和演化提案 | 受约束 LLM |
| WorldService | 校验并提交事实 | 事务和领域规则 |

NPC 不每帧调用 LLM。日常移动、进食、休息、工作、资源结算和远端生活由规则推进；玩家交互、重大冲突、反思、社会策略和演化提案才进入串行 TurnQueue。NPC 具有 active、dormant、destroyed 生命周期；休眠不删除身份和历史。

角色独特性来自角色底稿、价值观、秘密、底线、玩家历史、关系四轴（trust、respect、fear、owed）、当前需求、已确认事实和世界事件的组合。角色底稿由作者维护，不由模型静默改写。NPC 可以从未相识进入初次接触、试探、关系建立或冲突，再进入压力事件、兑现/失信和新目标；这是内容和规则的推进状态，不是模型自由发明的剧情。

每个重要 NPC 至少有一个公开目标、一个隐藏目标、一条底线、一项可以帮助玩家的能力或资源、一段会随时间推进的个人危机，以及盟友、交易伙伴、敌对者、离开群岛或成为阵营节点等不同关系结果。玩家可以信任某人但不尊重他，也可以尊重某人却害怕他；单一“好感度”不能替代关系轴。

## 6. 对话、记忆和 LLM 演化

对话输入先映射到询问、谈判、安抚、威胁、撒谎、承诺、交换、调查或结束等作者定义意图。结构化响应包含 `action_id`、`intent`、`motivation`、`dialogue`、`beat_id`、候选记忆和上下文版本。`motivation` 只用于解释和诊断，不参与规则判定；`action_id` 必须属于当前 DecisionContext。

对话动作例如 `create_promise` 不是直接完成任务，而是提交一条待验证命令。只有前置条件通过，承诺才进入世界并获得截止时间；接受、重谈、拒绝、兑现和违约都会留下事件、记忆、关系和后续计划变化。

记忆分为事实记忆、关系记忆和表达记忆。事实和关系的可规则部分来自已提交事件；模型只能提出摘要候选。Perception 记录 `source`、`observed_at`、`confidence` 和 `scope`，传闻不能自动成为事实。模型迟到时可留在审计日志中，但没有提交权限。Turn 入队保存 `world_revision`、`npc_state_version`、Provider/Model、Prompt 版本和 `turn_id`；返回后若版本变化，动作重新校验或丢弃。

World Intelligence 的闭环是：

```text
Observe -> Understand / Evaluate -> Intent or Evolution Proposal
-> Validate -> Sandbox / Replay -> Approve -> Versioned Activate
-> Measure -> Retain or Roll Back
```

世界演化对象包括 Content、Knowledge、Strategy 和 Declarative Capability。提案统一使用 JSON，带提案 ID、来源模型和上下文、依据事实、影响范围、类型化 operations、预期指标、资源预算、过期时间、兼容版本、沙箱结果和回滚点。模型可以提出新聚落、路线、组织、策略、需求或行为组合；不能修改 Runtime Core、注入 C++、任意加载脚本或绕过权限。

后续可用同一机制做场景实验：例如“在周边小区开一个小店”“提高利率”“发布一个政策”。LLM 把自然语言转成 `ScenarioProposal`，Validator 检查实体、参数和已注册机制，Sandbox 从快照克隆并运行确定性模拟，Runtime 输出收入、价格、库存、就业、迁移、关系和事件序列；LLM 只把这些已计算结果整理为假设、因果链、敏感参数和不确定性说明。缺少经济或社会机制时必须报告“无法评估”，不能用语言补造数值。这个能力属于后续平台验证，不进入小村庄 P0 的交付范围。

Provider 使用一个共享模型槽位和串行队列；本地 llama.cpp 优先，服务失败时规则台词继续运行。云 Provider 可用于调试或高质量表达，但不改变事实接口。API Key 不写入内容包、存档、日志、Prompt、崩溃报告或版本库。

Provider 配置由 JSON 选择，不绑定具体模型：

```json
{
  "providers": {
    "local_llamacpp": {
      "type": "openai_compatible",
      "base_url": "http://127.0.0.1:8192/v1",
      "model": "local-model",
      "timeout_ms": 5000,
      "supports_json_schema": true
    }
  },
  "default_provider": "local_llamacpp"
}
```

结构化对话可以包含 `say`、`emotion`、`beat_id`、`intent`、`action` 和 `memory_candidates`；`create_promise` 仍然只是命令提案，不能直接完成任务。

## 7. Runtime 核心契约

`WorldService` 是应用入口，`WorldState` 是事实，`EntityRegistry` 是投影，Domain Systems 执行规则，`WorldScheduler` 是唯一时间入口，`EventLog` 记录提交事实，`EventBus` 只通知已提交事件，View/Renderer 只读。

### 实体、组件和拓扑

实体只有稳定身份、通用 kind、元数据和生命周期；不提供 `setRoom()`、`setDoor()` 等游戏特例。空间实体使用 `SpatialComponent`，连接使用 `LinkComponent`，资源、需求、关系、位置、渲染和行为通过可组合组件表示。`WorldState.entities` 是身份和生命周期权威来源，生命周期为 `active -> dormant -> destroyed`；销毁身份保留在存档和历史中但不再进入活动投影。世界创建实体必须引用已登记的 `entity_archetype`，经事务追加 `entity.created` 后提交；LLM 只能提出创建提案。

### 事务和事件

```text
Command / IntentProposal / EvolutionProposal
  -> schema、权限、可见性、版本和业务校验
  -> 暂存多实体/多组件修改
  -> 递增 Revision，追加有序 WorldEvent
  -> 原子提交 WorldState 与 EventLog
  -> 重建组件/视图投影
  -> 提交后发布 EventBus
```

提交前失败不改变状态、Revision 或事件；提交后订阅者失败只能进入诊断计数，不能回滚事实。跨实体操作使用同一事务和版本检查；命令使用幂等键。

### 时间、移动和模拟等级

物理时间、游戏时间和事件时间分开。近场物理固定步长，NPC 事件唤醒或低频更新，远端跳到下一个有意义的日程、会面、资源结算或承诺截止。所有跳时由 `WorldScheduler::advance_to` 按时间边界处理；需求结算、目标唤醒和资源冲突不能各自维护时钟。

近场和远端共享 `MovementIntent`/`MovementResult`，包含起点、终点、路线、预计到达时间、资源消耗和失败原因；模拟等级只改变执行精度，不改变业务规则。Navigation 与 Physics 分离；物理观察必须经过 WorldService 才能成为业务事实。

## 8. JSON 内容、行为和提案契约

Content Pack、Game Instance、Intelligence Profile、Behavior DSL、Evolution/Scenario Proposal、Snapshot、EventLog 和 LLM 结构化输出统一使用 JSON，并带 `kind`、`schema_version`、版本身份和稳定 ID。当前 NPC 需求使用 `data/schemas/npc-content.schema.json`，LLM 意图使用 `data/schemas/intent-proposal.schema.json`；世界拓扑和实体使用 `data/schemas/village-content.schema.json`。加载器检查重复 ID、缺失引用、连接端点、需求 satisfier 和 schema 版本；内容规则只能调用 Runtime 已注册的能力，不能加载任意脚本。

Behavior DSL 只组合注册节点和动作，例如：

```json
{
  "kind": "behavior_recipe",
  "schema_version": 1,
  "id": "cooperative_transport",
  "root": {
    "type": "sequence",
    "children": [
      {"type": "condition", "name": "need_critical", "args": {"taxonomy": "safety"}},
      {"type": "action", "name": "coordinate_transport", "args": {"partner": "lorn"}}
    ]
  }
}
```

演化操作是类型化的，例如 `create_entity_archetype`、`overlay_strategy`、`register_behavior_recipe`、`add_relationship` 和 `schedule_event`；不是任意 JSON Patch。新增 C++ Component、System、权限、调度、事件格式、存档迁移或可执行代码必须由开发者发布。

## 9. 存档、回放和调试

Snapshot 包含 `save_schema_version`、`simulation_version`、`content_version/hash`、`world_id`、`world_revision`、`event_sequence`、游戏时间、实体生命周期、需求状态、幂等键和需要的 RNG 状态。`event_sequence` 是快照截断点，后续 EventLog 从其后追加。当前未发布，不保留旧存档兼容路径；schema 改变时同步更新存档读写、测试和文档。

P0 承诺检查点恢复：读取已提交 Snapshot 和记录的结构化响应，不调用 LLM。未来确定性模拟回放还要重跑命令、Provider 响应、上下文版本、RNG 和规则版本；不能把检查点恢复宣传成完整确定性重算。Bullet 只承诺固定环境下尽量复现，规则层和远端 LifeEvent 才是跨平台确定性目标。

文本调试界面和事件检查器至少显示：当前时间、Revision、实体生命周期、NPC 需求缺口/驱动力、当前意图和目标、行为树节点、最近 Command/Proposal、拒绝原因、事件序列、Provider 响应分类、保存截断点和回放差异。所有诊断读取已提交状态，暂存提案必须明确标记。

## 10. 产品路线、第一验证场和验收

### P0：三 NPC 小村庄验证平台和模型

小村庄只提供简单故事背景：一个公共资源、三个重要 NPC、日程、需求、关系、承诺、记忆和一个可观察的资源冲突。验收的重点不是剧情数量，而是以下因果链：

```text
资源变化 -> 需求缺口 -> 驱动力/目标 -> NPC 意图 -> 规则行动
-> 他人或公共资源变化 -> 关系/记忆/策略变化 -> 次日计划变化
-> EventLog 和 Snapshot/Replay 可解释
```

P0 必须支持：任意最小内容包启动；NPC 无玩家继续运行；需求随时间衰减并产生 `need.*` 事件；已提交行动产生满足；Goal/FSM/BT 分层；非法动作不改变事实；实体生命周期可审计；模型关闭时世界仍可玩；Snapshot 恢复和回放不调用 LLM；文本界面能显示驱动力和拒绝原因。

首个验收场景要求玩家与三个重要 NPC 各交谈多轮后仍保持角色底稿、知识边界和已确认事实；接受、拒绝和违背同一承诺会产生不同任务、关系和后续表达；玩家推进两天时至少两个 NPC 在无玩家参与下改变位置、资源或目标阶段；未知实体、过期承诺、版本冲突、非法动作和重复 turn 不改变事实。模型服务关闭时，规则台词、世界推进、任务和存档仍可用。

### P1：潮门港内容和集体驱力

增加港区、阵营、失踪任务、交易、资源预留、家庭/组织指标、近远场 LOD、完整 UI、物理机关和内容迁移。P1 才把村庄的资源冲突扩展为更大的社会网络。

### P2：World Intelligence 演化和沙箱

实现 Proposal Schema、提案库、异常检测、回放沙箱、策略/内容 overlay、指标评估、审批策略、版本化激活、回滚和审计。先支持声明式能力和已有组件组合，不热加载任意代码。场景实验在这一阶段验证，不作为 P0 游戏功能门槛。

### P3：群岛和可替换世界实例

增加多区域、阵营、经济、长期余波和世界自发聚落，并使用同一 Runtime 加载新的小说世界。现实世界实验只有在定义了相应实体、数据边界和机制后才有意义，不能把 LLM 泛化能力当作缺失模型的替代品。

## 11. 研究依据与边界

- Tay & Diener, *Needs and subjective well-being around the world*, 2011, DOI: 10.1037/a0023779。
- Wahba & Bridwell, *Maslow reconsidered: A review of research on the need hierarchy theory*, 1973。
- *The hierarchy of needs empirical examination of Maslow’s theory and lessons for development*, 2023, DOI: 10.1016/j.worlddev.2023.106185。
- *Linking homeostasis to reinforcement learning: internal state control of motivated behavior*, 2025, DOI: 10.1016/j.cobeha.2025.101611。
- Ryan & Deci, *Self-Determination Theory and the Facilitation of Intrinsic Motivation, Social Development, and Well-Being*, 2000, DOI: 10.1207/S15327965PLI1104_01。
- Park et al., *Generative Agents: Interactive Simulacra of Human Behavior*, 2023, DOI: 10.1145/3586183.3606763。
- Wang et al., *A survey on large language model based autonomous agents*, 2024, DOI: 10.1007/s11704-024-40231-1。
- *Understanding the planning of LLM agents: A survey*, 2024, DOI: 10.48550/arXiv.2402.02716。
- AgentSociety v1, *Large-Scale Simulation of LLM-Driven Generative Agents*, arXiv:2502.08691；AgentSociety 2, arXiv:2607.11895。借鉴其环境模块化、实验回放、工作区隔离和研究工作流；不把分布式执行框架直接带入 P0。
- AgentTorch / Large Population Models 的公开架构。借鉴其组合式机制、LLM 与机制模型分离和跨领域泛化方向；不把可微分大规模训练作为小村庄 Runtime 的前置条件。
- SimCity 系列的城市模拟实践。借鉴其资源流、交通、土地/设施约束和政策反馈必须由规则持续结算的思想；具体实现以本项目的事实、事务和事件契约为准。
- GenSim、CUP 等名称对应多个不同项目或论文，待确认具体版本后再纳入正式依赖或引用；当前只采用可核验的公开机制，不以名称作为架构依据。

这些研究用于需求分类、内稳态、社会动机、记忆和 Agent 设计的依据，不替代本项目的可验证 Runtime 契约。`robot_simu` 和 `BieTheirBed` 只作为固定时间、配置校验、工具副作用隔离、持久化和恢复语义的工程参考，不进入游戏运行时依赖。
