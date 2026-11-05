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

Runtime 不认识 Mira、村庄、潮门港或具体剧情。世界内容包用 JSON 声明实体原型、空间拓扑、资源、日程、需求、行为配方、事件条件、对白和可选视觉资产；Game Instance 选择内容版本并组合开局位置、初始关系和玩家体验；World Runtime State 保存当前事实；Intelligence Profile 定义感知投影、记忆策略、Provider、工具白名单和演化策略。Content Pack 是可复用的原型、规则和行为定义，Game Instance 是一次具体运行的身份与开局覆盖；运行中的状态永远不回写这两类静态定义。

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

### 内驱：需求域、驱动状态与自动适应

P0 使用四个可同时激活的需求域。它们用于建模和内容校验，不是严格互斥的心理实体，也不形成一个固定先后顺序：

| 需求域 | 典型状态变量 | 村庄中的具体需求示例 | 事实满足路径示例 |
|---|---|---|---|
| 生理 / 稳态 | 能量、水分、疲劳、疼痛、健康、体温、睡眠 | 食物安全、休息、船况对自身安全的威胁 | 进食、休息、治疗、修复设施 |
| 心理 / 自我调节 | 安全、可控性、自主、胜任、尊严、身份连续性 | Mira 的自主性、Eve 的档案安全与身份焦虑 | 获得保护、选择权、完成任务、澄清身份 |
| 社会 / 关系 | 归属、信任、互惠、承认、公平、地位、共同责任 | 被伙伴接纳、获得协作、兑现承诺、维护声誉 | 交谈、共同工作、交换、承诺、加入组织 |
| 认知 / 认识 | 确定性、信息充分性、因果一致性、新颖性、掌握 | Eve 对家谱真相的求知、对记录可靠性的怀疑 | 调查、询问、比较证据、实验、学习 |

Maslow 的五层图可以用于角色底稿和玩家可读的叙事提示，但不足以作为第一期 Runtime 的需求模型：其层级证据并不支持固定闸门，不能表示同一主体的并行需求，也不提供外部刺激、满足路径、学习、制度约束和可执行协议。P0 从四个需求域中各定义至少一项具体需求，以验证通用链路；这些是小村庄的内容分类，不是 Runtime 的枚举。需求域是非空的内容标签，其他世界可以增加 `ecological`、`institutional` 或新的域，而无需修改 C++ 驱动力内核；受治理的演化提案仍必须为新域提供满足配方、来源和可验证结果。

一项具体需求由 `NeedDefinition` 定义，运行时实例由 `NeedState` 保存。Definition 声明 `id`、`domain`、可选 `dimension`、初始缺口、基础变化率、临界阈值、基础权重与优先级、一个或多个满足配方以及来源/版本。State 保存当前缺口、最近更新时间、最近满足来源、有限度敏感度、预期满足和适应版本。这里的“五个驱动属性”是同一驱动状态的五个操作维度，不是五个人类需求：

1. **缺口**：当前状态与目标状态的差异；
2. **紧迫度**：缺口、临界阈值和时间窗口共同形成的行动压力；
3. **敏感度/权重**：该需求对当前主体的相对影响，可在声明边界内改变；
4. **预期满足**：候选行动根据历史结果预期能降低多少缺口；
5. **证据置信度**：感知、记忆和预测支持上述判断的可信程度。

已经批准的定义由 DriveSystem 随游戏时间更新：

```text
deficit(t + Δt) = clamp(deficit(t) + decay_per_hour × Δt - committed_relief, 0, 1)
`DriveSystem::evaluate` 将这些维度投影为结构化 `DriveEvaluation`：`urgency` 把缺口和临界区间归一化，`effective_weight = priority + sensitivity × urgency`，`expected_relief` 取个体有来源的经验值（没有时使用内容包初值），`confidence` 取当前证据置信度，单个需求对候选行动的贡献为 `effective_weight × expected_relief × confidence`。`drive` 数值只是同一投影的压力摘要，不再承担第二套排序语义。
expected_relief(action, need) = learned_expectation × evidence_confidence
```

每个具体需求必须绑定一个或多个 `NeedSatisfier`。满足配方说明在什么条件下、通过哪条已注册行为或过程、消费哪些事实、产生多少有证据的满足，以及失败和替代路径。配方最终编译成一个或多个元动作，例如 `consume_resource + relieve_need`、`reserve_resource + produce_resource + relieve_need`、`communicate + adjust_relationship + relieve_need`。一个配方可以满足多个需求，多个配方也可以满足同一需求；没有可执行满足配方的需求不能进入活动内容。满足只能引用已提交的行为、过程或事件。对白、模型自述、未验证记忆和 LLM 自行填写的数字不能直接降低缺口。

需求跨越临界阈值产生 `need.critical`，时间推进产生 `need.deprived`，已提交满足产生 `need.relieved` 或 `drive.satisfied`；事件记录需求 ID、前后值、来源行动/事件、配方、规则、内容和 Revision。

自动适应发生在单个主体的状态上，且必须有界、可审计、可回滚。例如行动前预测会缓解 0.2，实际结算为 0.05，DriveSystem 在内容声明的学习率、单次步长和冷却期内校准该需求的预期满足，并产生 `drive.adapted`；外驱习惯化则在校准该类刺激的敏感度。适应参数属于世界规则；LLM 可以解释误差和提出策略提案，但不能直接写权重、阈值或衰减率。个体适应不等于社会规则或 Runtime Core 演化。

当前 P0 采用确定性驱动力内核，确保世界不调用模型也能运行、测试和回放。内驱是主体内部连续状态，外驱是世界对主体施加的已记录刺激，结构约束是意图能否执行的条件。LLM 不替代内稳态计算，但会实际参与高价值决策：读取授权的需求、事件、关系和后果，解释处境，比较满足路径的代价与社会影响，引用它依赖的 Stimulus，生成 `tradeoffs` 和 `expected_relief`，再提出候选意图。C++ 只接受经过版本、权限、资源、可见性和业务规则校验的行动；模型不能直接写缺口、敏感度、资源、关系或事件。LLM 也可以提出新的需求定义、刺激类型、结构规则、行为配方和策略，但提案未通过沙箱和批准前不改变需求模型。

该模型是用于可信模拟的可解释近似，不声称还原真实人类心理。理论依据用于确定状态语义和边界，具体阈值、曲线与满足强度必须由内容或经批准的演化提案定义并通过回放校准。

### 外驱：结构化刺激与自动重评

外驱是平台从已提交事件、可观察环境和结构变化中投射给特定主体的刺激，不是另一份需求，也不是装饰性的长字符串。`StimulusState` 至少记录 `stimulus_id`、来源事实/事件、感知者、相关实体、类型、出现时间、到期时间、可见范围、来源置信度、强度、紧迫度、新颖性、正负效价和基础显著性。内部至少要区分生理变化、环境事件、社会请求/承诺、机会、威胁、制度变化和异常发现；具体类型仍由内容声明。

```text
salience = clamp(exposure × relevance × intensity × urgency × novelty × source_confidence, 0, 1)
effective_salience = salience × adapted_sensitivity(source_type, actor)
```

事件提交后，C++ 的 `StimulusProjector` 依据可见性、距离、关系和规则生成刺激事实；不能感知的主体不会获得该 Stimulus。LLM 读取已授权的 Stimulus 和证据，解释它对自己意味着什么、提出 `IntentProposal` 或记忆候选，但不能改写原始事件。相同来源的重复刺激可以触发内容声明的习惯化，使敏感度在上下限内下降；意外结果、目标失效或关系变化可能重新提高敏感度。适应产生 `stimulus.habituated` 或 `stimulus.reappraised`，并在 Snapshot 中恢复。

外驱的“自动演化”有两层：第一层是在已批准规则内，事件自动生成刺激、到期、聚合、习惯化和重新评价；第二层是新的刺激类型、评价规则或聚合方式，只能通过 `EvolutionProposal` 进入沙箱和版本化激活。后者不能由 LLM 直接登记为活动规则。

安全需求必须有真实的威胁来源。P0 曾只用 `crossing_safety` 验证“安全状态衰减—修复—满足”；P1 现已增加 `conflict.attack`、`conflict.sabotage`、`security.threaten`、`security.defend`、`security.evacuate` 和 `security.repair`。攻击或破坏是对目标的行动尝试，威胁和恐吓可以只改变威胁与暴露而不造成物理损伤，防守、撤离和修复是独立响应。它们先提交可审计事件，再由 `StimulusProjector` 投射为威胁刺激；实际安全缺口由事件、暴露时间、完整性/可用性变化和主体可信感知共同计算，LLM 只能权衡风险和响应，不能直接把安全感改成某个数值。

### 结构：可执行约束与受治理演化

结构不是内驱或外驱的别名，也不是一块不可变的背景。它是决定谁能感知、持有、移动、交换、协作和改变世界的规则事实，包括空间连接与访问、角色和权限、组织与制度、所有权与容量、生产配方、合同规则、信息可见范围及判定优先级。决策上下文中的 `StructuralConstraint` 必须是结构化条目，包含约束 ID、类型、作用实体/范围、来源规则、硬/软级别、数值边界、有效期和不可用原因。

结构通过两条路径改变。路径一是已声明机制产生的常规变化，例如合同状态转换、门被授权的开启、实体生命周期变化、资源所有权转移和机构规则按时生效；它们在事务中自动执行。路径二是结构本身的演化，例如新增角色规则、行为配方、需求曲线、机构权限或实体原型；提案结构至少包含来源事实、变更 diff、适用范围、预期指标、反事实/回放场景、风险级别、审批策略、目标版本和回滚点。

```text
StructureObservation
  -> EvolutionProposal
  -> Schema / capability / invariant validation
  -> sealed replay or sandbox simulation
  -> independent approval policy
  -> versioned overlay activation
  -> outcome monitoring
  -> retain / revise / rollback
```

批准权属于平台治理策略，而不是提出提案的 LLM。P0 中，新的需求定义、规则、拓扑、能力、行为配方和结构 overlay 由开发者或世界设计者批准；LLM 可以创建提案、补充证据和比较候选，但不得批准自己的提案。低风险且可在沙箱中完整验证的声明式变化以后可以自动批准，核心事务、权限、事件格式、调度、存档和组件存储等 Runtime Core 只能由开发者通过代码、测试、迁移和发布审核演化。任何自动激活都必须记录提案、审批者/策略、内容版本、模拟版本、指标、激活 Revision 和回滚结果。

### 意图、目标与行动

`GoalSelector` 用需求驱动力、预期满足、资源成本、风险、关系影响、长期目标连续性和时间约束对作者定义的候选行动排序。它提供可解释的候选空间，不要求每个决策都调用 LLM。LLM 在这个空间内进行情境解释、社会权衡、候选排序或提出已注册的 IntentProposal。状态机管理工作、移动、交谈、打断、休眠和不可用模式；行为树执行短计划并返回 Success/Failure/Running/Interrupted；WorldService 最终验证实际结果。

候选行动的效用可表达为：

```text
utility(a) = Σ(drive_i × expected_relief_i)
             + goal_continuity + relationship_effect + future_value
             - resource_cost - risk_cost - constraint_penalty
```

这里的需求收益由 `GoalSelector` 对同一 `action_id` 满足的所有需求贡献求和，并把 `utility_score` 与每项 `utility_contributions` 放入决策上下文和意图事件。行动可以由内容声明 `utility_profile`，包括 `resource_cost`、`risk`、`relationship_effect`、`future_value` 和 `constraint_penalty`；Runtime 校验范围后按声明组合这些分项，不在 C++ 里按角色或故事写死权重。LLM 可以在复杂社会情境中解释和权衡这些项，但它不能伪造已经不存在的事实。候选计划和参数契约来自内容包，模型只能填写声明的参数，例如一个通用 `goto` 计划的 `destination`。Runtime 校验参数类型、目标实体、权限和当前拓扑后，才把参数绑定到 BT 的元动作；模型不能凭空创建资源、任务、关系变化或事件。C++ 校验结构约束后把意图转成 Command，事务成功后的结果才改变世界，并生成下一轮外驱和内驱输入。

### 社会和集体驱力

个体需求通过共享资源和关系汇聚为社会驱力：粮食安全、运输能力、记录完整性、信任、成员流失、公共设施可用性和组织目标。集体指标必须声明参与实体、资源池、目标值、时间窗口、聚合方式、冲突权重和失败后果，不能简单取 NPC 平均值。

P0 只实现个体需求和公共资源，验证“资源变化—行动—他人受影响”的因果链；P1 已增加可配置的组织成员、集体指标、跨主体关系、原子资源交易、声明式资源预留/抢占、受约束的冲突/安全事实和近远场更新；P2 允许 World Intelligence 提出新的社会指标或策略，并通过历史回放沙箱比较效果。复杂合同仲裁与更大规模协商仍是内容扩展项。自主性、胜任感和关系性作为横向调制因素，不压缩成单一需求层级。

### 世界模型的基本对象

在增加任何新行为前，先确认行为操作的对象、状态变量和计量单位已经存在。小村庄 P0 采用 ODD（Overview、Design concepts、Details）式检查；每个对象同时有三种表示：

1. **Definition**：内容包中的稳定定义，例如资源单位、过程配方、角色能力和规则版本；只读，运行中不能被状态污染。
2. **Runtime state**：由 C++ 维护的当前事实，例如持有数量、预留、位置、合同状态和过程进度；所有写入都经过事务。
3. **Cognitive projection**：主体看到、相信或推断的事实，例如传闻、解释、偏好和置信度；可以不完整或错误，不能反向成为世界事实。

| 对象 | Definition 必须声明 | Runtime state 必须维护 | 典型事实 |
|---|---|---|---|
| Entity / Agent | 稳定 ID、kind、原型、角色、能力标签、可执行权限、生命周期策略 | 生命周期、位置、所有者、当前模式、活动/休眠状态、参与过程 | 创建、移动、获得、失去、休眠、销毁 |
| Resource definition / holding | 资源 ID、单位、可分割性、质量轴、替代组、可再生/衰减规则、容量规则 | 持有者、保管位置、数量、质量、已预留量、可用量、来源和到期时间；`available = amount - reserved` | 生产、消耗、转移、预留、释放、过期、损耗 |
| Capability / facility | 能力 ID、适用角色/设施、输入输出类型、许可和版本 | 当前可用性、占用者、冷却、损坏和维护状态 | 获得能力、占用设施、维修、失效 |
| Production process | 输入（资源/数量/单位/质量）、输出/副产物/废料、设施、角色技能、持续时间、能耗、容量、失败条件、结果质量和随机源 | execution ID、参与者、开始/结束时间、预留、进度、实际输入输出、状态和失败原因 | 开始、完成、失败、产出、损耗、能力提升 |
| Consumption rule / need | 消费者类型、可满足需求、资源替代、效用曲线、时间窗口、预算和副作用 | 需求缺口、最近满足来源、实际消费量、效用和短缺 | 消费、需求缓解、预算变化、浪费、短缺 |
| Exchange / contract | 参与方角色、报价/支付资源、数量和单位、交付地点/时间、预留策略、信任门槛、结算和违约规则 | 合同状态、版本、各方预留、已交付量、应付款、截止时间和争议 | 提议、接受、拒绝、交付、结算、违约、重谈 |
| Communication / speech act | 渠道、可见范围、话语行为类型、允许引用的事实、成本、承诺效果和记忆策略 | 消息、参与者、发送/接收时间、观察者、来源、置信度、已兑现承诺 | 说出、听见、询问、请求、承诺、拒绝、误解、传播 |
| Institution / rule | 适用范围、角色权限、价格/税率/配额、优先级、有效期、执行者和处罚 | 当前版本、启用状态、例外、执行记录和违规次数 | 发布、生效、许可、拒绝、处罚、废止 |
| Event / causal record | 事件类型、schema、因果来源、允许参与者和数据契约 | 时间、顺序、规则/内容版本、前置事实、结果、影响范围和回放输入 | 可审计的世界历史 |

核心对象的写入权和生命周期必须只有一个来源。这里的“投影”可以复制状态用于查询，但不能成为第二个事实存储：

| 对象 | 唯一事实/状态所有者 | 生命周期 | 写入入口 | 持久化责任 |
|---|---|---|---|---|
| `WorldState` / ComponentStore | `WorldService` 事务边界 | 世界实例全程 | 已校验 `Command` 或规则提交 | Snapshot + EventLog |
| `WorldEvent` | 提交器的 `EventLog` | 永久追加；`event:<sequence>` 稳定标识 | 成功事务自动产生 | EventLog；EventBus 只通知 |
| `Entity` 生命周期 | `WorldState.entities` | `active -> dormant -> destroyed` | `createEntity` / `setEntityLifecycle` | Snapshot + 生命周期事件 |
| `Goal` / `NpcActivity` | NPC 活动转换函数 | 目标可持续；同一时刻一个主活动；`proposed` 只能表示空槽 | 意图接受、过程完成/失败/中断 | NPC activity 聚合 |
| `ProcessExecution` | `NpcActivity` | 启动、运行、挂起、恢复、完成/失败 | 行为配方启动与时间推进 | activity 聚合 + 过程事件 |
| FSM / BT | `NpcActivity` 的运行投影 | 活动期间 | 状态转换函数 / 配方 tick | 仅保存恢复所需聚合，不独立改写阶段 |
| `IntentProposal` | Intelligence/Provider 输出 | 单次决策 | `TurnQueue` 或同步决策 | 通过 `npc.intent*` 事件审计 |
| `StimulusState` | 感知投影 | 出现、聚合、到期/撤销 | 已提交事件经 `StimulusProjector` | NPC 状态 + 投影事件 |
| Memory / Knowledge / Experience | NPC 认知投影 | 可修正、可遗忘、保留来源 | 已提交事件驱动的 operator | NPC 状态；不能反写事实 |
| `EvolutionProposal` / Overlay | `EvolutionManager` 与当前内容视图 | 提案、验证、批准、激活、回滚 | 独立治理策略 | Proposal/EventLog + Snapshot overlay 栈 |

`Goal` 可以在未来扩展为多个候选目标集合，但当前实现只把一个被接受的主目标绑定到唯一 `NpcActivity`；候选目标不是并行事实，也不能绕过活动转换函数直接写 `ProcessExecution`、FSM 或 BT 状态。当前不保存临时候选活动：`proposed` 必须是无 Goal、无 Process 的空槽，任何非空 Activity 必须同时具有身份一致的 Goal 与 Process，并与 `active/suspended/completed/failed/cancelled` 投影严格一致。Snapshot 加载拒绝非法组合，而不是静默修正。

生产、创造、交易、消费和聊天是同一套过程语义的不同入口，而不是五套互相独立的逻辑：

- **生产**是一个有输入、输出、设施、能力、持续时间和失败结果的 `ProductionProcess`。输入可按过程的 `reservation_mode` 预留，完成时按过程规则结算；不能由 LLM 直接写“产出 10 个物品”。
- **创造**分为运行时实体实例化和内容/能力演化。运行时只能从已登记的 archetype 创建实体；新实体类型、行为配方或规则组合属于 `EvolutionProposal`，要经过验证、沙箱、批准、版本化激活和回滚。
- **交易**先创建 `ExchangeContract`，接受不等于交付，交付不等于结算；合同保存双方、两侧资源、截止时间、版本和状态，只有 `Accepted -> Settled` 的结算事务才移动双方资源。任何一侧余额不足都会撤销本次结算暂存，合同保留 `Accepted`，并产生引用失败码的 `exchange.settlement_rejected` 审计事实；截止时间进入 `Expired` 时产生指向合同 ID 的独立事件。每次决定和结算都会重新校验参与方生命周期，休眠、销毁或非参与方不能取得旧合同的操作权。价格、税费、抵押、预留和补偿条款属于内容/制度配置，不能由对话文本凭空改变。
- **消费**把资源转成需求满足、效用和副作用，同时记录实际消耗、替代品、浪费和短缺；需求数值由 Runtime 衰减和结算。
- **聊天**是带可见范围和话语行为的 `Communication`。自然语言由 LLM 生成或解释，询问、请求、承诺、撒谎和威胁是结构化意图；只有经过事实、权限和承诺规则验证的结果才能进入 WorldState。

对象之间的最小因果链是：

```text
Perception / Need / Opportunity
  -> IntentProposal
  -> BehaviorRecipe / ProcessExecution
  -> reserve inputs and permissions
  -> atomic meta-actions
  -> WorldState facts + WorldEvents
  -> memory, relationship and next drives
```

因此，P0 可以先用通用资源池验证村庄因果链，但必须保留单位、所有权、质量、容量、预留和时间窗口这些字段的语义入口；只有数量而没有这些属性时，系统只能称为“资源计数器”，不能称为生产、经济或社会模拟。

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
| WorldService | 协调系统并提交事实 | 事务边界和领域系统 |

NPC 不每帧调用 LLM。日常移动、进食、休息、工作、资源结算和远端生活由规则推进；玩家交互、重大冲突、反思、社会策略和演化提案才进入串行 TurnQueue。NPC 具有 active、dormant、destroyed 生命周期；休眠不删除身份和历史。后台自主请求使用内容声明的 `max_decision_age_minutes` 截止点；同一 NPC、同一决策通道的尚未开始请求可由 `coalesce_pending` 合并，过期或被合并的结果只产生审计事件，不能启动旧活动。玩家交互仍按通道单独处理，不被后台请求吞掉。异步 decision channel 还可以声明 `allow_intervening_world_changes`：会话默认关闭；自驱可开启以允许同一调度边界中其他主体的无关提交先发生，但返回时仍必须通过主体版本、时间、权限、参数和当前事实校验。这个声明只放宽全局 Revision 的乐观并发检查，不授予模型绕过 Runtime 的权限。

经验与知识不是同一个对象。EventLog 是事实来源；NPC 的 episodic memory 保留“发生了什么”，KnowledgeFact 表示主体对实体/关系/规则的带来源判断，Experience 表示从一次或多次行动结果中抽象出的可复用倾向或技能。直接感知、亲身行动结果、观察他人事件、可信 testimony 和探索结果都可以产生候选经验；Runtime 只接受带来源、时间、范围和置信度的声明，LLM 可以总结或提出归纳但不能把文本直接变成客观事实。通用经验可被多个 NPC 或策略模板复用，个人经验绑定 NPC、关系和视角。决策上下文同时提供已确认知识与经验，但候选行动仍必须经过当前事实校验。

`WorldService` 是唯一协调入口，但不应成为所有领域规则的实现容器。合同协调由 `ContractSystem` 负责承诺和交换的状态机、参与方权限、截止时间与结算规划；NPC 活动推进由 `NpcSimulationSystem` 负责日程边界、长过程、预留抢占、挂起/恢复和策略日界修订；这两个系统都通过 WorldService 的统一事务和事件回调提交事实，不拥有独立状态副本。资源、社会、元动作、NPC 活动和合同系统都遵守同一条 `validate -> transaction -> WorldState/EventLog` 路径。

一次有效的 `IntentProposal` 只负责选择并启动一个已登记的行为/过程，不负责逐 tick 指挥。运行时把它固化为 NPC 的 `ProcessExecution`：记录 `process_id`、动作与配方 ID、开始/完成时间、进度、资源预留、上下文 Revision、可中断策略和状态。之后每个游戏时间步只推进过程、重算前置条件和检查中断信号；完成、失败或中断才提交对应事实。只要过程仍有效，不重复询问 LLM；过程结束后或出现高显著性外驱、硬约束变化、资源失效时，当前 WorldService 的重评估入口才重新安排意图，未来可抽出独立 GoalSystem。一个 NPC 只有一个 `NpcActivity` 槽，它是活动生命周期的唯一所有者；槽中的 `ActivityStatus` 是唯一阶段，Goal 和 ProcessExecution 的状态字段只是该阶段的持久化投影。Goal、ProcessExecution、FSM 和 BT 是同一个活动的四个视角，各自只回答一个问题：Goal 回答“为什么做、是否仍值得做”，ProcessExecution 回答“正在执行哪个过程、进度和预留是什么”，FSM 回答“NPC 当前处于什么运行模式”，BT/BehaviorRecipe 回答“当前过程如何逐步执行”。Goal 的终止状态只能由 `NpcActivity` 的转换函数根据过程结果、中断信号或失效条件统一写入；FSM 模式由同一转换函数投影得到；BT 只返回 `Running/Success/Failure/Interrupted`，不能直接改写 Goal 或过程状态。

角色独特性来自角色底稿、价值观、秘密、底线、玩家历史、关系四轴（trust、respect、fear、owed）、当前需求、已确认事实和世界事件的组合。角色底稿由作者维护，不由模型静默改写。NPC 可以从未相识进入初次接触、试探、关系建立或冲突，再进入压力事件、兑现/失信和新目标；这是内容和规则的推进状态，不是模型自由发明的剧情。

每个重要 NPC 至少有一个公开目标、一个隐藏目标、一条底线、一项可以帮助玩家的能力或资源、一段会随时间推进的个人危机，以及盟友、交易伙伴、敌对者、离开群岛或成为阵营节点等不同关系结果。玩家可以信任某人但不尊重他，也可以尊重某人却害怕他；单一“好感度”不能替代关系轴。

## 6. 对话、记忆和 LLM 演化

对话输入先进入 LLM 的 `utterance_understanding` 阶段，由模型根据内容包为每类话语行为提供的 `description` 和 `world_action_allowed` 识别通信意图；Runtime 不扫描玩家文本，也不保存关键词表，更不能从候选动作反推玩家意图。随后 `player_dialogue` 阶段读取结构化理解、当前活动、记忆和授权事实，生成回答以及可选的 `action_id`。主体投影把公开角色信息与 `private_character` 分开；隐藏目标、私有边界和内部策略可以用于权衡，但除非内容显式授权披露，否则不能被复制到对白、记忆或玩家可见事件。Provider 失败时，对话 Turn 直接失败且不写事件、不启动过程；Runtime 不会用关键词或固定台词伪造一次语义理解。Runtime 永远根据模型返回的话语行为 ID 和内容包权限重新校验是否允许行动，不接受模型自行授予权限。问候、询问、结束和普通陈述可以配置为仅对话；请求、命令、谈判和承诺可以配置为允许产生行动提案。结构化理解包含 `speech_act`、`communication_goal`、`action_requested`、引用实体和歧义；结构化决策包含 `action_id`、`parameters`、`intent`、`motivation`、`dialogue`、`cited_stimulus_ids`、`tradeoffs`、`expected_relief`、候选记忆和上下文版本。`parameters` 只能填充候选计划声明的 JSON Schema，并随 `ProcessExecution` 保存，恢复时不要求模型重建。`cited_stimulus_ids` 指出模型实际依据的外驱，`tradeoffs` 解释权衡，`expected_relief` 表达预测；这些字段用于审计、学习和回放校验，不能直接写入事实。`motivation` 只用于解释和诊断；`action_id` 必须属于当前 DecisionContext。仅对话响应使用空 `action_id`，必须同时包含非空玩家可见 `dialogue`，否则 Runtime 失败关闭且不写事件；它不会进入行为树或修改世界事实。

对话和自驱不是 Runtime 中的两个写死分支，而是世界内容包中的 `decision_channels`。每个 channel 配置是否是会话、是否进行话语行为分类、默认行动权限、事件类型、记忆类型、事件目标和对应的 Intelligence Profile 策略；`conversation_channel` 与 `autonomous_channel` 只是当前实例选择的两个入口。Runtime 按 channel 属性执行同一条提案校验和事务路径，因此新的观察者、定时器、群体审议或外部传感器入口只需增加 JSON channel，不需要增加 `if (trigger == ...)` 的业务耦合。

对话动作例如 `create_promise` 不是直接完成任务，而是提交一条待验证命令。只有前置条件通过，承诺才进入世界并获得截止时间；接受、重谈、拒绝、兑现和违约都会留下事件、记忆、关系和后续计划变化。

记忆分为事实记忆、关系记忆和表达记忆。事实和关系的可规则部分来自已提交事件；模型只能提出摘要候选。Perception 记录 `source`、`observed_at`、`confidence` 和 `scope`，传闻不能自动成为事实。模型迟到时可留在审计日志中，但没有提交权限。Turn 入队保存 `world_revision`、`npc_state_version`、Provider/Model、Prompt 版本和 `turn_id`；返回后若版本变化，动作重新校验或丢弃。

World Intelligence 的闭环是：

```text
Observe -> Understand / Evaluate -> Intent or Evolution Proposal
-> Validate -> Sandbox / Replay -> Approve -> Versioned Activate
-> Measure -> Retain or Roll Back
```

世界演化对象包括 Content、Knowledge、Strategy 和 Declarative Capability。提案统一使用 `schema_version=1` 的 JSON，带提案 ID、来源模型和上下文、依据事实的事件 Sequence、影响范围、类型化 operations、预期指标、资源预算、过期时间、兼容版本、沙箱结果和回滚点。当前 Runtime 强制校验 `source`、治理类别和 `evidence_event_sequences`：只接受当前 EventLog 中真实存在且不重复的事件引用，不能用一个模型名称或虚构事实替代审计依据。模型可以提出新聚落、路线、组织、策略、需求或行为组合；不能修改 Runtime Core、注入 C++、任意加载脚本或绕过权限。

Level 1 是主体行为演化：LLM 在现有 Runtime 能力内泛化 NPC 的经验、社会证据和内外驱，提出新的策略 Overlay 或已注册元动作的行为配方。它改变以后如何形成目标和选择计划，不等于替 NPC 执行当前行动。Level 2 是世界底座/内容演化：LLM 根据多个主体和世界事件提出新的实体原型、行为能力组合、社会策略或规则参数。两级都只改变可声明、可验证的 Simulation Framework/Content Overlay；都经过 `LLM Proposal -> Validate -> Sandbox -> Independent Approval -> Versioned Activate`。`register_behavior_recipe` 是创建新 BT 的唯一在线入口；它只能组合注册节点、谓词和 operator，且新能力默认只进入提案声明的传播范围，不自动授予所有 NPC。请求入口是 `EvolutionManager::propose(world, level, trigger)`，产品 CLI 使用 `--evolve level1|level2`；提案生成阶段调用当前 Intelligence Profile 的 `evolution_proposal` 策略，采用单一强制 function call，校验失败最多把结构化错误反馈给模型修正两次，沙箱阶段只重放捕获的上下文，不重新调用 LLM。Entity、事务、事件、权限、回放和回滚这些 Runtime Core 不属于 LLM 可演化对象；需要新的 C++ 能力时只能形成 capability request，待开发者发布注册 operator。

演化不是每个 Tick 的后台调用。Content Pack 的 `evolution_policy` 只声明是否启用自动提案、两个等级的最小日间隔、是否合并同等级挂起请求和提案上下文的最大年龄；它不能授予自动批准或激活权限。到达配置边界时，Runtime 在已提交时间事务之后捕获只读上下文，把 Level 1/2 请求放入独立的串行 Evolution Queue；Provider 工作线程只读取捕获的 JSON，不持有 WorldState，不阻塞下一次 Tick。结果回到主线程后重新检查内容版本、当前事实中的证据和治理 Schema；无关事实可以在模型思考期间提交，提案会以当前 Revision 重新校验和 Sandbox，最多给真实 Provider 两次结构化修正；有效结果自动运行无 LLM 的 Sandbox 并记录 `evolution.proposal_ready` 审计事件，仍需独立审批才能生成 Overlay。失败、过期或模型不可用只记录审计事件，不用规则或 Mock 冒充演化。普通计划由玩家输入、刺激、需求压力和过程边界触发；Level 1 在有界的个人经验窗口、显著预测误差、连续失败或日边界触发并受冷却/预算限制，自动日边界按 NPC 分别排队；Level 2 只在跨主体异常、资源/关系指标持续偏离或世界周期边界触发。Runtime 先确定性聚合证据，LLM 在一次结构化提案中完成总结、反思和最小创新，之后仍必须经过治理链。Level 1 上下文带有 `target_npc`，只提供该主体的记忆、知识、经验、刺激和关系私域，其他主体最多以地点和运行模式摘要出现；提案的策略/日程操作必须命中该目标，行为配方也必须把目标列入 `available_to`。Level 2 才接收受权限裁剪的跨主体摘要。窗口上限是内容无关的上下文安全上限，排序、可见性和传播范围由 Runtime 的事实权限和配置决定。NPC 的 DecisionContext 使用按主体可见性、地点、关系、承诺和当前候选计划裁剪的 `worldSnapshot`，并分别提供 `selfState`、`planCatalog`、记忆、知识、经验、刺激和约束；不把全量 WorldState 或内部版本号当作模型语义材料。

同一机制可以承载场景实验：例如“在周边小区开一个小店”“提高利率”“发布一个政策”。LLM 把自然语言转成 `ScenarioProposal`，Validator 检查实体、参数和已注册机制，Sandbox 从同一快照克隆并运行 baseline/scenario，Runtime 输出资源、库存、组织指标、需求、位置、关系、承诺、异常和两侧事件序列；LLM 只把这些已计算结果整理为假设、因果链、敏感参数和不确定性说明。缺少经济或社会机制时必须报告“无法评估”，不能用语言补造数值。当前 P2 已实现资源、组织指标和 NPC 需求三类有界 intervention，以及面向不必填用的 CLI；小店、利率和政策只有在对应事实模型落地后才能增加专用 intervention。

`DecisionContext` 的窗口由内容包的 `decision_context` 配置约束：刺激按显著性保留前 N 条，记忆/知识/经验按时间保留最近 N 条，同地点实体和相邻连接按稳定顺序截断，关系按主体可见范围截断。截断只影响模型观察窗口，不删除事实、事件或 NPC 的长期认知；上下文预算应随 Intelligence Profile 校准，而不能通过扩大快照把所有世界状态塞给模型。

Provider 使用一个共享模型槽位和串行队列；本地 llama.cpp 是产品入口的默认语义 Provider，服务失败时玩家对话和 NPC 自驱提案都明确失败且不提交事实。Runtime 可以继续推进时间、衰减和已批准的过程，但不会用确定性候选冒充 LLM 的主体决策。若 LLM 返回的计划结构通过 JSON Schema 却不满足 Runtime 的参数契约，在线同步请求和异步请求都最多获得一次结构化 validator feedback；资源不足、权限、拓扑、Revision 或过期错误不重复调用模型。Mock Provider 只由测试、沙箱重放和离线可重复回归显式注入；它不是产品默认行为。云 Provider 可用于调试或高质量表达，但不改变事实接口。API Key 不写入内容包、存档、日志、Prompt、崩溃报告或版本库。

P0 的结构化 `IntentProposal` 是短输出任务；玩家对白拆成独立的 `utterance_understanding` 和 `player_dialogue` 两次结构化调用。前者需要理解自然语言和歧义，默认使用约 128 token 的 thinking；后者只根据已解析的通信意图和当前事实生成简短回答，默认关闭 thinking。自驱审议没有玩家给出的明确目标，需要依据内驱、外驱和结构约束进行短权衡，因此默认开启约 128 token 的 thinking。Provider 通过顶层 `reasoning_budget_tokens` 限制思考段预算；该预算不等于最终 JSON 的总输出预算。无论是否开启 thinking，Runtime 只解析最终 JSON，`reasoning_content` 永远不进入对白、记忆或世界事实。

小模型的 Provider 策略按决策价值分档，而不是固定追求最大思考预算：普通对白和低风险动作关闭 thinking；候选之间存在社会冲突、秘密或承诺权衡时使用约 128 token 的 reasoning budget；低置信度或校验失败才升级到 256。512 以上只用于演化提案、沙箱解释和离线分析，不进入每个 NPC Turn。预算升级必须记录原因、耗时、最终候选和校验结果，便于以后用真实回放重新校准。

Provider 与决策预算由可版本化的 Intelligence Profile 选择，不绑定具体模型。正式配置是 `data/intelligence-profile.json`；交互式 client、text UI 和 headless 默认使用 `profile`，测试与可重复离线批处理通过 `--provider mock` 显式选择。Schema 位于 `data/schemas/intelligence-profile.schema.json`。

Profile 的 `decision_policy` 是按 channel 使用的命名策略表。Provider 接收 Runtime 解析后的 `provider_policy`，只负责执行对应预算和输出格式，不再根据 `player_dialogue` 或 `autonomous_reappraisal` 这样的业务触发字符串猜测决策类型。

每次决策还带有独立的 `runtime_metadata` 审计信封，包含 `world_id`、`instance_id`、`content_version`、`simulation_version`、`prompt_version`、`world_revision` 和 `npc_state_version`，以及实际 channel/trigger。它用于日志关联、异步过期校验和回放定位，不属于 NPC 的可见世界事实；`worldSnapshot` 不复制这些工程身份字段，Provider 的系统提示也明确禁止据此推断资源、地点、动机或行动。

```json
{
  "kind": "intelligence_profile",
  "schema_version": 1,
  "provider": {
    "type": "llama_cpp",
    "base_url": "http://127.0.0.1:8192/v1",
    "model": "local-model",
    "timeout_ms": 120000,
    "temperature": 0.2
  },
  "decision_policy": {
    "player_dialogue": {
      "thinking": false,
      "max_tokens": 512
    },
    "utterance_understanding": {
      "thinking": true,
      "reasoning_budget_tokens": 128,
      "max_tokens": 512
    },
    "autonomous_reappraisal": {
      "thinking": true,
      "reasoning_budget_tokens": 128,
      "max_tokens": 1024
    }
  }
}
```

Provider 的 thinking、reasoning budget、max tokens 和 timeout 只从版本化 Intelligence Profile 读取；环境变量不再覆盖这些决策参数。API Key 仍只从配置声明的环境变量读取，不写入内容包、存档或日志。

结构化对话可以包含 `say`、`emotion`、`beat_id`、`intent`、`action` 和 `memory_candidates`；`create_promise` 仍然只是命令提案，不能直接完成任务。

## 7. Runtime 核心契约

`WorldService` 是应用入口，`WorldState` 是事实，`EntityRegistry` 是投影，Domain Systems 执行规则。`WorldCommandSystem` 只协调实体、拓扑、资源转移、交易、关系、组织成员和移动命令；它不拥有第二份状态，也不绕过 `WorldService` 的事务和事件边界。`EntityProjectionSystem` 只根据已提交 `WorldState + ContentPack` 重建和同步 `EntityRegistry`，不拥有可独立修改的实体事实；`DecisionContextProjector` 是只读的主体视图边界：它根据 `WorldState + ContentPack` 和主体身份生成位置、所有权、关系、需求、记忆、知识、经验、刺激、约束、候选计划和 BT 摘要；它不能提交命令，也不拥有事实。`ResourceSystem` 负责与 NPC/故事无关的资源数量、公共池/实体库存解析、转移、预留和释放校验；`SocialSystem` 负责关系轴、组织指标和成员关系的通用事实变换；`CognitionSystem` 负责 NPC 作用域的知识和经验写入、来源事件校验、置信度合并和审计事件，行为执行器不能直接改写这些容器；`StimulusSystem` 负责按内容声明把已提交事件投射为主体可见的外驱、重复刺激习惯化、容量淘汰和到期，不拥有事件事实；`NpcSimulationSystem` 负责内容声明的 Activity/ProcessExecution、预留抢占、挂起恢复和日界策略投影；`BehaviorExecutionSystem` 负责将候选计划解析为 BehaviorRecipe/BT、评估注册条件、绑定参数并调用有限元动作，同时把移动、资源、关系、合同和安全效果交给 WorldService 的高层命令；`CommandDispatcher` 只负责解析 Runtime 公共命令协议并路由到这些高层命令，不拥有事实或内容规则；`WorldIntelligenceSystem` 负责把按主体裁剪的 DecisionContext 交给 Provider、校验返回的意图、启动 ProcessExecution 并处理异步结果，`IntelligenceCoordinator` 只管理 Provider 队列和请求生命周期；`WorldEvolutionSystem` 负责 Level 1/2 提案所需的世界摘要、证据窗口、Provider 请求以及已批准 Overlay 的激活/回退协调，`EvolutionManager` 负责纯验证、沙箱和审批策略；`SecuritySystem` 负责内容声明的冲突、威胁、防守、撤离和修复参数，解析主体与目标并把安全事实交给统一事务入口；`SimulationCoordinator` 负责从当前游戏时间选择下一个调度边界、打开一次事实事务、协调活动/合同/日程系统、在提交后泵送 Provider 结果和 durable checkpoint；它不拥有资源或 NPC 事实，`SimulationScheduler` 仍是纯边界计算器。这些系统都不打开独立事实边界、不发布事件，由 `WorldService` 协调事务和审计。`MetaActionExecutor` 只接收内容绑定的 operator 定义和参数，执行已注册的有限 Primitive；需要创建 Promise、移动、交易或安全事实时只调用 `WorldService` 注入的高层命令回调。这样 JSON 可以组合 operator，LLM 可以提出新的 BT 配方，但不能由配置或模型直接执行任意代码。时间边界由纯函数式 `SimulationScheduler` 计算；被合并的安静分钟仍逐分钟计入 `world_revision` 与模拟时间，因此分段推进和一次大跳在无外部干预时保持相同事实。`TransactionManager` 独占保存点、Revision 和提交/回滚，`EventJournal` 独占事件身份、因果和提交后发布，`IntelligenceCoordinator` 独占异步 Provider 队列和挂起请求身份；这些模块都不拥有资源、NPC 或社会规则。`EventLog` 记录提交事实，`EventBus` 只通知已提交事件，View/Renderer 只读。Snapshot 与 JSONL Replay 通过可注入的 `IWorldPersistence` 写入；`JsonFilePersistence` 先写临时文件、刷新并完成后再替换目标路径，`SqlitePersistence`（构建发现 sqlite3 时可选）则把同名快照或 Replay 工件替换放在单个 SQLite 事务中。两者都只保存已提交表示，不产生第二份可写 WorldState；当前 SQLite 适配器也不宣称已经把 WorldState、EventLog、Revision 和幂等记录统一到 durable commit boundary。当前原型的 `data/` 只保存静态输入，运行存档写到外部 run 目录；持久化取舍和升级条件记录在 [ADR 0001](adr/0001-persistence-storage.md)。未来的生产化数据库仍应在提交后更新投影并发布 EventBus。

Runtime Core 只保证实体、组件存储、事务、Revision、事件、调度、持久化、回放和权限；Needs、Drive、Perception、Goal、Behavior、Resource 和 Relationship 是可组合的 World Simulation Framework 能力。P0 可以把这组能力编译在同一进程中，但不能把村庄角色或某一套心理模型写进 Core。这样同一 Runtime 才能加载不同内容实例，且 LLM 只能通过已注册能力提出变化。

### 实体、组件和拓扑

实体只有稳定身份、通用 kind、元数据和生命周期；不提供 `setRoom()`、`setDoor()` 等游戏特例。空间实体使用 `SpatialComponent`，连接使用 `LinkComponent`，资源、需求、关系、位置、渲染和行为通过可组合组件表示。`WorldState.entities` 是身份和生命周期权威来源，生命周期为 `active -> dormant -> destroyed`；销毁身份保留在存档和历史中但不再进入活动投影。世界创建实体必须引用已登记的 `entity_archetype`，经事务追加 `entity.created` 后提交；LLM 只能提出创建提案。

NPC 的 `schedule` 是 Content Pack 中的基线可用性/位置规则，不是当前目标，也不是 LLM 的隐式控制权。例如 `"06:00-18:00": "ferry_dock"` 表示游戏时间每天 06:00（含）到 18:00（不含）默认位于 `ferry_dock`；跨午夜的 `"18:00-06:00"` 覆盖 18:00 至次日 06:00 前。当前原型在小时边界按该规则确定性更新运行态 `location` 和 `schedule_label`，因此模型不会每个 Tick 重写日程，也不会因为一次对话自动修改源文件。LLM 可以在 Level 1 反思后提出 `overlay_schedule`，但它必须声明 NPC、完整时间槽和已存在地点，经过验证、沙箱、独立批准后才作为版本化 Overlay 激活；临时离开仍是运行态行动，不等于改变基线日程。

日程的持久改变属于结构/策略演化：`overlay_schedule` 是当前已实现的最小 `ScheduleProposal`，经过 Schema/权限/回放或沙箱验证后，按治理策略批准并作为版本化 Overlay 激活。当前操作替换指定 NPC 的完整日程，位置必须来自内容拓扑，时间边界必须在一天范围内；未来可增加有效期、冲突承诺和指标约束。任何批准都产生版本、来源和生效时间事件，不能静默改写原始 Content Pack。

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

事务只覆盖一次状态转换，不跨越整个长过程。长过程采用“启动事务 → 资源预留与 `ProcessExecution` → 时间推进/中断检查 → 完成或失败事务”的边界。完成时消费本过程拥有的预留并写入效果；被高显著性外驱打断时进入 `ActivityStatus::Suspended`，同一事务释放预留并记录 `npc.process_suspended`，保留目标、进度和计划需求；刺激失效且资源仍可满足时，当前活动生命周期逻辑按原计划重新预留并恢复，`completesAt` 顺延暂停时长，记录 `npc.process_resumed`。资源被更高优先级过程合法抢占时使用同一挂起/恢复生命周期，但暂停原因记录为 `resource_preempted`，事件同时引用抢占者和新过程；仲裁先只读计算完整资源方案，只有全部缺口都能由符合策略的过程释放时才一次提交。若资源无法满足或目标已失效，则请求在写入前拒绝，原过程继续运行，不能留下“暂停了一部分参与者”的中间状态。这样既保持单次提交原子性，也允许捕鱼、维修、搬运、等待交易和移动持续多个 tick。

这里的回滚有四个明确边界：校验失败发生在写入前，直接拒绝且不改变事实；复合 `BehaviorRecipe` 在校验通过后若某个后续元动作失败，嵌套事务只撤销本次短效果提交里已经暂存的前置效果，不撤销 BT、Goal、FSM 或世界时间；长过程失败不倒退世界时间，而是释放预留、取消过程或执行声明式补偿，活动转为 `Failed` 后由 Goal/Drive 在下一次唤醒时重新规划；Evolution Overlay 回滚只恢复未来使用的内容/策略视图，已经发生的 WorldState 和 EventLog 永远保留。它们不能混称为把整个世界倒回上一个 tick。

### 原子元动作与复合行为

Runtime 只向内容和模型暴露一组可审计的原子元动作。元动作不是任意字段写入器，而是带有 `id`、参数 Schema、前置条件、目标范围、权限、资源成本、直接效果、产生事件的规则和失败码的注册执行器。注册表还必须标记 `implemented`；未实现的设计目录项不能被内容加载器接受。后续能力必须通过注册表和 C++ 执行器增加，不能由 JSON 指向任意函数或代码。

小村庄 P0 的元动作目录分为“当前已实现的运行时能力”和“尚未实现、只能作为 P1 设计入口的能力”。设计目录不能被误读为已注册执行器：

| 状态 | 组 | 元动作/接口 | 说明 |
|---|---|---|---|
| P0 operator | 资源 | `consume_resource` / `produce_resource` | 检查可用量、单位、质量和预留后扣减或增加资源 |
| P0 operator | 资源 | `reserve_resource` / `release_resource` | 为当前过程预留或释放资源，避免并发超用 |
| P0 operator | 需求 | `relieve_need` | 依据已提交行动和来源事件降低需求缺口 |
| P0 operator | 关系 | `adjust_relationship` | 按 trust、respect、fear、owed 单轴调整并记录原因 |
| P0 operator | 拓扑 | `set_link_state` | 开关、锁定或改变连接通行状态 |
| P1 operator | 资源 | `transfer_resource` | 已实现为 `resource.transfer`：在实体库存或公共池之间转移数量，校验参与方、生命周期、余额和容量并产生 `resource.transferred` |
| P1 operator | 移动 | `movement.traverse` / `movement.arrive` | 已实现：通过活动拓扑规划路线，校验通行状态后提交 `entity.moved` |
| P1 operator | 冲突 | `conflict.attack` / `conflict.sabotage` | 已实现安全事实入口：只改变完整性、可用性、威胁和暴露，不直接销毁实体或改需求数值 |
| P1 operator | 安全 | `security.threaten` / `security.defend` / `security.evacuate` / `security.repair` | 已实现：更新 `SecurityState` 并由 StimulusRule 投射安全外驱；修复可消费已定义资源 |
| P1 operator | 承诺 | `promise.create` / `promise.transition` | 建立、接受、重谈、兑现或违约；命令与 Behavior Recipe 共用同一事务和事件边界 |
| P1 operator | 合同 | `exchange.create` / `exchange.transition` / `exchange.settle` | 提案、接受/拒绝/取消、截止和两侧原子结算；合同状态与资源所有权都进入 Snapshot/EventLog |
| P1 operator | 知识/经验 | `memory.append` / `knowledge.assert` / `experience.record` | 已实现为 NPC 作用域投影：记忆、知识和经验都必须引用已提交事件，保存时间、范围和置信度；它们不能直接改写客观事实 |
| P1 operator | 生产 | `production.execute` | 先验证全部输入余量、资源注册和输出容量，再一次性交换，输入不足或容量溢出不产生部分事实 |
| P0 command API | 实体 | `create_entity` / `set_entity_lifecycle` | 从注册原型创建实体，或休眠/销毁；当前不是 Behavior DSL operator |

`emit_event` 不作为可由模型自由调用的独立“改写动作”；每个成功元动作和事务由 Runtime 自动产生 WorldEvent，避免事实与事件分开提交。`set_resource`、`adjust_resource` 和 `set_npc_resource` 这类可写入任意绝对值或第二份库存的接口不向内容和 LLM 暴露，内容使用受单位、所有权、预留、前置条件和容量约束的语义化元动作。

村庄复合行为可以由当前已注册元动作自然生成：配给是“检查粮食 → 消耗粮食 → 满足需求 → 记录”；修船是“检查粮食和船况 → 消耗粮食 → 增加船况 → 调整关系”；档案整理使用资源/需求配方；资源转移、生产、移动、安全和承诺都可以作为配方节点进入内容包，知识、经验和记忆通过带来源约束的 operator 写入主体投影。

资源账本是可复用的 World Simulation Framework 能力，不是村庄专用规则。Runtime 只实现通用的数量、容量、所有权、转移、生产、消耗、预留和释放算子；资源 ID、单位、容量、衰减和替代关系由 Content Pack 声明。资源能力是可选的，未声明资源的世界不会因为 Runtime 存在资源算子而产生资源事实。

长过程是否预留输入由行为配方的 `reservation_mode` 声明，当前值为 `none` 或 `input_resources`。Runtime 不根据持续时间、资源名称或 NPC 身份推断预留策略。`input_resources` 只建立本过程的计划占用，完成时消费并释放，暂停/取消时释放；它不改变资源事实数量。资源仲裁同样完全由内容声明：`priority` 是非负分配优先级，`preemption_policy=none` 表示只接受当前可用资源，`lower_priority` 表示请求可暂停持有所需预留、优先级更低且声明为可中断的其他过程。相同或更高优先级不会被抢占，被选中的过程按优先级和稳定 ID 确定顺序，整组暂停在同一事务中完成。抢占者完成后，受害者只有在刺激条件清除且资源重新可用时才恢复，`completesAt` 顺延实际暂停时长；`npc.process_suspended` 和 `npc.process_resumed` 保存原因、抢占者、过程 ID、资源方案与优先级证据。该机制属于通用资源基础设施，村庄、港区和未来经济实例只配置数值和策略，不增加 C++ 特例。

定时 `Activity` 负责在作者定义的时刻唤醒内容动作；它可以直接声明已注册 operator，也可以用 `operation=meta_action` 引用行为包中的元动作和参数。Activity 的全部效果在嵌套事务中执行，任何一个 operator 失败都会撤销本次活动的暂存效果，并留下 `activity.failed` 审计事件。JSON Behavior DSL 已把已注册效果组合为条件、顺序、选择、超时和中断节点；`production.execute` 已实现输入检查、输出容量检查和全量提交，长过程中断后的声明式补偿仍按具体内容声明。`MetaActionDefinition` 描述单个已注册事实变更，行为配方、过程与元动作由 ID 引用并分别校验。Game Instance 还可以声明 NPC/玩家库存初始值；这些是运行态事实的种子，不会写回 Content Pack。LLM 可以创造性提出新的行为组合、参数和目标，但必须使用已注册元动作；缺少表达力时生成 CapabilityProposal。整条计划先在暂存事务中汇总成本、预留和效果，不能逐个元动作直接写 WorldState。

```text
IntentProposal + BehaviorRecipe
  -> precondition and whole-plan validation
  -> staged atomic effects and reservations
  -> commit all effects or none
  -> ordered WorldEvents with event/correlation/causation identity
  -> next facts, stimuli and memories
```

每个 `WorldEvent` 由 Runtime 生成稳定 `event_id=event:<sequence>`，并冻结该事实提交时的 `content_version` 与 `simulation_version`。`event_sequence` 与 `world_revision` 是不同概念：前者标识事件在只追加历史中的位置，后者是根事务提交后的世界状态代次。一次已提交的时间推进即使没有业务事件也增加 `world_revision`；同一根事务中的全部事件共享同一个 `world_revision`，无变化的拒绝路径和回滚保存点不推进它。异步提案同时绑定 Revision、NPC State Version 和游戏时间截止点，因此世界时钟越过提案上下文后，即使没有业务事件也会使旧提案失效。同一根事务产生的事件共享 `correlation_id=correlation:<first_event_sequence>`；事务中的第一个事件 `causation_sequence=0`，后续事件指向前一个同事务事件，因此能区分“同一决策链中的直接因果”和仅按时间相邻的外围事件。嵌套保存点继承外层 correlation；只有成功提交才推进外层直接前因，失败保存点会同时截断其暂存事件、Revision 和因果位置。Snapshot 恢复会校验事件序号、身份、版本来源、world revision、correlation 前缀和 causation 必须指向同链的前序事件，损坏的因果历史会被拒绝。该标识描述可审计的提交链，不把时间相关性伪装成因果解释。

这种分层把“判断条件、执行计划、改写事实”拆开：条件属于规则和上下文，执行属于受控计划，事实改写只发生在事务提交点。LLM 获得组合和创造空间，Runtime 保留幂等、原子性、权限、回放和回滚。

### 时间、移动和模拟等级

物理时间、游戏时间和事件时间分开。近场物理固定步长，NPC 事件唤醒或低频更新，远端跳到下一个有意义的日程、会面、资源结算或承诺截止。P0 由 `WorldService::advanceMinutes` 顺序推进分钟并在边界执行结算；未来的 `WorldScheduler::advance_to` 必须保持同一契约，需求结算、目标唤醒和资源冲突不能各自维护时钟。

近场和远端共享 `MovementIntent`/`MovementResult`，包含起点、终点、路线、预计到达时间、资源消耗和失败原因；模拟等级只改变执行精度，不改变业务规则。内容包的 `simulation_policy.near_tick_minutes` 和 `far_tick_minutes` 决定 NPC 唤醒节奏，NPC 的需求衰减使用该 NPC 上次驱动结算时间到当前时间的实际跨度，因此远端跳过多个小时不会丢失需求变化。`SimulationMetrics` 记录近/远场唤醒、跳过检查和离线推进分钟数；快照序列化大小与事件数量由性能基准测试测量。Navigation 与 Physics 分离；物理观察必须经过 WorldService 才能成为业务事实。

## 8. JSON 内容、行为和提案契约

Content Pack、Game Instance、Intelligence Profile、Behavior DSL、Exchange Contract、Evolution/Scenario Proposal、Snapshot、EventLog 和 LLM 结构化输出统一使用 JSON，并带 `kind`、`schema_version`、版本身份和稳定 ID。当前 NPC 需求使用 `data/schemas/npc-content.schema.json`，LLM 意图使用 `data/schemas/intent-proposal.schema.json`，行为配方、节点、优先级和抢占策略使用 `data/schemas/behavior-pack.schema.json`，合同状态使用 `data/schemas/exchange-contract.schema.json`；WorldEvent 使用事件 schema 4，除时间、参与者、World Revision 和数据载荷外还包含 `event_id`、`correlation_id`、`causation_sequence`、`content_version` 与 `simulation_version`，契约位于 `data/schemas/world-event.schema.json`；世界拓扑、实体和可选 `scene_asset/camera` 表现配置使用 `data/schemas/village-content.schema.json`。`scene_asset` 相对于 Content Pack 根目录解析，客户端当前支持 `slanted_top_down` 与 `top_down` 两种相机模式。加载器检查重复 ID、缺失引用、连接端点、需求 satisfier 和 schema 版本；内容规则只能调用 Runtime 已注册的能力，不能加载任意脚本。客户端从当前内容包投影空间实体、连接、NPC 标记、资源 HUD 和可选场景资产，不按 `world_id` 或角色 ID 选择渲染代码。

Behavior DSL 只组合注册节点、条件和元动作，例如：

```json
{
  "kind": "behavior_pack",
  "schema_version": 2,
  "meta_actions": [
    {"id": "consume_crate", "category": "resource", "operator": "resource.consume"},
    {"id": "relieve_loading_need", "category": "need", "operator": "need.relieve"}
  ],
  "conditions": [
    {"id": "crate_available", "operator": "resource.available"}
  ],
  "recipes": [
  {
  "id": "unload_crate",
  "allowed_actors": ["role:dockworker"],
  "reservation_mode": "input_resources",
  "priority": 20,
  "preemption_policy": "lower_priority",
  "root": {
    "type": "sequence",
    "children": [
      {"type": "condition", "name": "crate_available", "args": {"resource": "crate", "minimum": 1}},
      {"type": "meta_action", "name": "consume_crate", "args": {"resource": "crate", "amount": 1}},
      {"type": "meta_action", "name": "relieve_loading_need", "args": {"action": "unload_crate"}}
    ]
  }
  }
  ]
}
```

 P0 不把这套 DSL 误称为通用编程语言，但它已经支持由 JSON 构造一棵受约束的行为树。内容包声明 `meta_actions`（故事动作 ID 到 Runtime 通用 operator 的绑定）、`conditions`（条件 ID 到只读谓词的绑定）和 `recipes`；加载器校验引用，`BehaviorRecipeRuntime` 在运行时编译 `Sequence`、`Selector`、`Interruptible`、`Timeout`、`Condition` 和 `Action` 节点。游戏代码不会为 Mira、Lorn、Eve 或某个故事动作创建专用 BT。`GoalSelector`/LLM 只选择候选 `recipe_id`，接受后由 Runtime 创建 `ProcessExecution` 并记录该 recipe；长过程等待由 ProcessExecution 管理，过程完成时才在事务中运行已编译配方的原子效果。Recipe 可以声明参数 JSON Schema，节点通过 `{"bind":"parameter_name"}` 引用运行时输入；因此 `goto`、交易对象、协作对象和调查目标都是能力参数，而不是写死在 operator 中的故事 ID。普通决策只能选择已批准计划并填写参数；LLM 要创建全新的 BT，必须通过 Level 1/2 的 `register_behavior_recipe` 提案，经过引用验证、沙箱和独立批准后才会进入候选目录。新增行为只需新增 JSON 配方、参数契约和已登记的通用 operator；只有新增底层事实变换原语才需要扩展 Runtime。

演化操作是类型化的，例如 `create_entity_archetype`、`overlay_strategy`、`overlay_schedule`、`register_behavior_recipe`、`add_relationship` 和 `schedule_event`；不是任意 JSON Patch。新增 C++ Component、System、权限、调度、事件格式、存档迁移或可执行代码必须由开发者发布。

内容定义必须描述对象的语义属性，而不只是给动作起名字。资源定义与运行时持有量分开；生产过程定义与一次执行分开。资源定义至少声明单位、是否可分割、质量轴、替代组、容量和衰减规则；持有量至少记录所有者、位置、数量、已预留量、可用量、来源和到期时间。

```json
{
  "kind": "resource_definition",
  "schema_version": 1,
  "id": "ration_grain",
  "unit": "ration",
  "divisible": false,
  "quality_axes": ["freshness", "contamination"],
  "substitutes": ["preserved_grain"],
  "capacity": {"default": 100, "minimum": 0},
  "decay": {"amount_per_day": 0, "reason": "ration_grain.decayed"}
}
```

```json
{
  "kind": "production_process_definition",
  "schema_version": 1,
  "id": "repair_ferry",
  "inputs": [{"resource": "grain", "quantity": 5, "unit": "ration"}],
  "outputs": [{"resource": "boat_integrity", "quantity": 8, "unit": "points"}],
  "actor_role": "ferryman",
  "facility": "ferry_dock",
  "duration_minutes": 60,
  "failure": {"event": "repair.failed", "reason": "insufficient_input"}
}
```

一次过程执行还要记录定义版本、参与者、设施、预留、开始/截止时间、进度、实际输入输出和状态：

```json
{
  "kind": "process_execution",
  "schema_version": 1,
  "id": "process:ferry-repair:42",
  "definition_id": "repair_ferry",
  "definition_version": 1,
  "participants": ["lorn"],
  "facility": "ferry_dock",
  "reservations": ["reservation:grain:17"],
  "started_at": "day:3T10:00",
  "due_at": "day:3T11:00",
  "state": "running"
}
```

交易合同至少记录双方、交换资源、数量/质量、报价或支付物、预留、交付地点和时间、有效期、结算及违约路径。承诺状态变化对关系轴的影响由内容包 `promise_policy` 声明，包括违约双方、拒绝方和接受方的 `trust/respect/fear/owed` 增量以及重谈延长时长；Runtime 只负责把增量原子写入事务，不在代码里写死数值。聊天记录至少包含 `speech_act`、发送者、接收者/观察者、渠道、事实引用、来源和置信度、会话轮次、回复期限及预期后果。对话里的“我答应给你粮食”只能提交 `promise.offer` 或合同提案，不能直接增加粮食或关系。P1 注册对应 operator 后，运行时才把确认后的定义编译成 `transfer_resource`、`create_promise`、`append_memory` 等元动作事务；当前配方可使用已注册资源、需求、关系、拓扑、移动、安全和记忆/知识 operator，未注册能力仍会被内容加载器拒绝。Event 由提交器自动产生。LLM 可以提出新的过程组合，但不能只凭一段文字生成不存在的产量、所有权或承诺结果。

## 9. 存档、回放和调试

Snapshot 包含 `save_schema_version`、`simulation_version`、`content_version/hash`、`world_id`、`instance_id`、`world_revision`、`event_sequence`、游戏时间、实体生命周期、需求状态、社会关系、组织成员与指标、承诺、交换合同、进行中的 `ProcessExecution`、NPC 的 `lastSimulationTick/lastDriveTick`、幂等键和需要的 RNG 状态。运行态 NPC 只保存 `definition_id`，不复制内容包里的角色定义；角色名、角色、公开/隐藏目标等静态数据在加载时从当前内容包解析。`instance_id` 与 `content_version` 共同锁定这次运行所使用的 Game Instance 和 Content Pack，任何一个不匹配都直接拒绝恢复。`event_sequence` 是快照截断点，后续 EventLog 从其后追加。加载时校验 `definition_id` 可解析且 `content_version`、`instance_id` 与当前内容一致，并验证事件序号、因果链和事件版本来源，任一不满足直接拒绝并报出差异，不回退到旧字段或静默重置。当前未发布，不保留旧存档兼容路径；schema 改变时同步更新存档读写、测试和文档。当前快照 schema 为 18，模拟版本为 `adaptive-runtime-p1.19`；实体库存与公共资源池分开保存，资源转移只能通过注册 operator 改变两者。激活中的 overlay 以有序记录保存 proposal ID、基线内容版本、激活版本和类型化操作；恢复时从基线 Content Pack 重放这些已批准操作，回滚也由同一版本栈重建。已经提交的世界事实和 EventLog 不随 overlay 回退。需要显式 durable checkpoint 时，`WorldService::saveDurableCommit` 把已提交 Snapshot 与 Replay 交给当前持久化适配器；SQLite 在一个事务中保存状态载荷、事件载荷、Revision、幂等键和 Overlay 投影。`DurableCheckpointPolicy` 默认关闭，可选择在已提交调度边界按分钟周期或日边界写入；它不会把 I/O 放进每个 Tick 或打开的事务，逐根事务自动写盘仍需更高的生产化崩溃恢复契约。

P0 同时支持两种产物：Snapshot 负责普通检查点恢复；可选 Replay Package 保存基线状态、从基线到当前状态的聚合 JSON Patch、事件范围、World Revision 和状态摘要。`replay()` 会实际应用该 Patch，追加事件并通过最终 Snapshot 校验，所以它能验证“这份审计包是否重建出同一事实”；它同时接受独立 JSONL Replay Package 和 `saveDurableCommit()` 生成的显式 checkpoint artifact。这仍不是 Event Sourcing：事件载荷尚未被定义为逐领域 reducer，不能声称仅靠事件从初始状态重建；Replay Package 是提交效果重放和因果审计。Replay 只在显式生成时做一次聚合 diff，不进入每个 tick 的运行路径。未来确定性模拟回放还要重跑命令、Provider 响应、上下文版本、RNG 和规则版本；不能把检查点恢复宣传成完整确定性重算。Bullet 只承诺固定环境下尽量复现，规则层和远端 LifeEvent 才是跨平台确定性目标。

文本调试界面和事件检查器至少显示：当前时间、Revision、实体生命周期、NPC 需求缺口/驱动力、当前意图和目标、行为树节点、最近 Command/Proposal、拒绝原因、事件序列、Provider 响应分类、保存截断点和回放差异。所有诊断读取已提交状态，暂存提案必须明确标记。

## 10. 产品路线、第一验证场和验收

### P0：三 NPC 小村庄验证平台和模型

小村庄只提供简单故事背景：一个公共资源、三个重要 NPC、日程、需求、关系、承诺、记忆和一个可观察的资源冲突。验收的重点不是剧情数量，而是以下因果链：

```text
资源变化 -> 需求缺口 -> 驱动力/目标 -> NPC 意图 -> 规则行动
-> 他人或公共资源变化 -> 关系/记忆/策略变化 -> 次日计划变化
-> EventLog 和 Snapshot/Replay 可解释
```

P0 必须支持：任意最小内容包启动；NPC 无玩家继续运行并由默认 LLM 产生自主提案；需求随时间衰减并产生 `need.*` 事件；已提交行动产生满足；Goal/FSM/BT 分层；非法动作不改变事实；实体生命周期可审计；LLM 不可用时已批准规则和存档仍可恢复，但不会伪造新的 NPC 意图；Snapshot 恢复和回放不调用 LLM；文本界面能显示驱动力和拒绝原因。

首个验收场景要求玩家与三个重要 NPC 各交谈多轮后仍保持角色底稿、知识边界和已确认事实；接受、拒绝和违背同一承诺会产生不同任务、关系和后续表达；玩家推进两天时至少两个 NPC 在无玩家参与下改变位置、资源或目标阶段；未知实体、过期承诺、版本冲突、非法动作和重复 turn 不改变事实。模型服务关闭时，世界时间推进、任务和存档仍可用；玩家对话返回可重试错误且不猜测语义、不提交行动。

### P1：潮门港内容和集体驱力

已完成一个可替换的潮门港纵向切片：独立 Content Pack 声明市场、两名 NPC、组织成员和集体指标；Game Instance 声明两方库存；定时 Activity 通过注册的 `trade.execute`、`group.metric_adjust` 和 `relationship.adjust` 形成“交换→组织士气→跨 NPC 信任”的事件链，`ExchangeContract` 还覆盖提案、接受、两侧原子结算和过期，并能从 Snapshot 恢复。近远场节奏和 `advance_to` 基准也已覆盖。家庭、失踪任务、完整 UI、物理机关和内容迁移仍是后续港区扩展，不进入 Runtime Core。

### P2：World Intelligence 演化和沙箱

已完成 Proposal Schema、提案库、异常检测、回放沙箱、策略/内容 overlay、指标评估、审批策略、版本化激活、回滚和审计。异常规则和 Scenario intervention 都是内容可声明、Runtime 可验证的通用边界；先支持声明式能力和已有组件组合，不热加载任意代码。场景实验不进入 P0 游戏门槛，缺少事实模型时明确拒绝。

### P3：群岛和可替换世界实例

已完成最小可替换性纵向切片：`Glass Orchard` 使用跨区域拓扑、两个组织/阵营、制度指标、资源交换、自定义 `stewardship` 与 `ecological` 需求域，以及逾期承诺造成的关系余波。它证明新的小说语义不需要进入 Runtime Core；Headless 的实例标题、对话参与者和回放内容根也来自当前内容，而不是固定村庄。

群岛的多区域剧情、世界自发聚落和更大规模经济仍属于内容扩展和能力演化，不应靠增加 C++ 特例完成。现实世界实验只有在定义了相应实体、数据边界、校准方法和机制后才有意义，不能把 LLM 泛化能力当作缺失模型的替代品。

## 11. 研究依据与边界

- Tay & Diener, *Needs and subjective well-being around the world*, 2011, DOI: 10.1037/a0023779。
- Wahba & Bridwell, *Maslow reconsidered: A review of research on the need hierarchy theory*, 1973。
- *The hierarchy of needs empirical examination of Maslow’s theory and lessons for development*, 2023, DOI: 10.1016/j.worlddev.2023.106185。
- *Linking homeostasis to reinforcement learning: internal state control of motivated behavior*, 2025, DOI: 10.1016/j.cobeha.2025.101611。
- Ryan & Deci, *Self-Determination Theory and the Facilitation of Intrinsic Motivation, Social Development, and Well-Being*, 2000, DOI: 10.1207/S15327965PLI1104_01。
- Baumeister & Leary, *The need to belong: Desire for interpersonal attachments as a fundamental human motivation*, 1995, DOI: 10.1037/0033-2909.117.3.497。
- Cacioppo & Petty, *The need for cognition*, 1982；Kruglanski & Webster, *Motivated closing of the mind: “Seizing” and “freezing”*, 1996。用于认知需求、信息闭合和证据不足时的动机语义；不把它们直接当作普遍通用的人类常数。
- Park et al., *Generative Agents: Interactive Simulacra of Human Behavior*, 2023, DOI: 10.1145/3586183.3606763。
- Wang et al., *A survey on large language model based autonomous agents*, 2024, DOI: 10.1007/s11704-024-40231-1。
- *Understanding the planning of LLM agents: A survey*, 2024, DOI: 10.48550/arXiv.2402.02716。
- Grimm et al., *The ODD Protocol for Describing Agent-Based and Other Simulation Models*, JASSS 23(2):7, 2020, DOI: 10.18564/jasss.4259。用于检查模型目的、实体、状态变量、过程、调度、交互、随机性和可复现描述。
- Ostrom, *A General Framework for Analyzing Sustainability of Social-Ecological Systems*, Science 325(5939), 2009, DOI: 10.1126/science.1172133。用于把 resource system、resource unit、actor、governance、interaction 和 outcome 分开，避免把库存、制度与主体认知揉成一个资源数字。
- Lancaster, *A New Approach to Consumer Theory*, Journal of Political Economy 74(2), 1966, DOI: 10.1086/259131。用于把消费建模为资源属性向需求满足/效用的转换，而不是“买一个物品就加满意值”。
- Smith, *The Contract Net Protocol: High-Level Communication and Control in a Distributed Problem Solver*, IEEE Transactions on Computers C-29(12), 1980, DOI: 10.1109/TC.1980.1675516。用于区分任务公告、报价/提议、授予/接受和执行，支持交易与协作合同的显式状态。
- Akerlof, *The Market for Lemons: Quality Uncertainty and the Market Mechanism*, Quarterly Journal of Economics 84(3), 1970, DOI: 10.2307/1879431。用于要求交易对象记录质量、信息不对称、来源和担保，不能只用数量表示商品。
- Searle, *Speech Acts: An Essay in the Philosophy of Language*, 1969；结合 Clark & Brennan 的 grounding 研究。用于把聊天拆成话语行为、事实引用、共同确认和后续承诺，而不是把自然语言文本当作世界事实。
- AgentSociety v1, *Large-Scale Simulation of LLM-Driven Generative Agents*, arXiv:2502.08691；AgentSociety 2, arXiv:2607.11895。借鉴其环境模块化、实验回放、工作区隔离和研究工作流；不把分布式执行框架直接带入 P0。
- AgentTorch / Large Population Models 的公开架构。借鉴其组合式机制、LLM 与机制模型分离和跨领域泛化方向；不把可微分大规模训练作为小村庄 Runtime 的前置条件。
- SimCity 系列的城市模拟实践。借鉴其资源流、交通、土地/设施约束和政策反馈必须由规则持续结算的思想；具体实现以本项目的事实、事务和事件契约为准。
- GenSim、CUP 等名称对应多个不同项目或论文，待确认具体版本后再纳入正式依赖或引用；当前只采用可核验的公开机制，不以名称作为架构依据。

这些研究用于需求分类、内稳态、社会动机、记忆和 Agent 设计的依据，不替代本项目的可验证 Runtime 契约。`robot_simu` 和 `BieTheirBed` 只作为固定时间、配置校验、工具副作用隔离、持久化和恢复语义的工程参考，不进入游戏运行时依赖。
