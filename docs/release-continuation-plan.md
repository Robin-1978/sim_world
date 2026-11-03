# 《潮痕》持续推进计划

> 本文件只记录实施状态、工作包、验证和下一步；架构与模型以 [design.md](design.md) 为唯一来源。
> 最后更新：2026-11-03

## 当前目标

完成一个由三 NPC 小村庄承载的最小平台验证：需求/内驱随游戏时间变化，外部事件和结构约束进入决策上下文，规则或 LLM 产生意图，C++ 校验并提交事实，结果进入 EventLog、Snapshot、Replay 和文本调试界面。3D、港区和场景实验暂不作为当前门槛。

当前 P0 验收已完成。后续工作进入 P1/P2：扩展通用行为 DSL、交易与制度模型，以及带沙箱的 World Intelligence 演化；这些能力不回写 P0 的 Runtime Core 契约。

## 当前检查点

| 工作包 | 状态 | 证据/位置 |
|---|---|---|
| C++20/CMake 核心构建 | 已完成 | `src/core`, `src/world`, `CMakeLists.txt` |
| WorldState、事务、Revision、EventLog、EventBus | 已完成 | `src/world/world_service.*`, `src/core/event_bus.*` |
| 通用 Entity/Component、拓扑和生命周期 | 已完成 | `WorldState.entities`, `EntityRegistry`, `createEntity`, `setEntityLifecycle` |
| NPC 状态机、行为树、全局/局部规划 | 已完成 | `src/npc/state_machine.*`, `src/npc/behavior_tree.*`, `src/navigation/*` |
| 文本调试界面和 headless 回放 | 已完成 | `src/text_ui_main.cpp`, `src/headless_main.cpp` |
| 内容包加载 | 已完成 | `WorldService::load/fromContent`, `data/`, `tests/fixtures/minimal-pack` |
| 存档 schema 与事件截断点 | 已完成 | `world_state_json.*`, `save_schema_version=10`, `simulation_version=adaptive-runtime-p0.10`, `event_sequence`, `instance_id`, `NpcActivity` 聚合、`definition_id` 引用、实例/内容版本校验；运行态不复制角色定义 |
| 内驱/需求模型 | 四域、显式满足配方和有界预期适应已实现；敏感度学习与演化提案待扩展 | `src/npc/drive_system.*`, `NeedContent`, `NeedState`, `need.*` events |
| 外驱/结构上下文 | 已实现结构化 Stimulus/Constraint、到期、习惯化和快照；结构演化治理已接入，沙箱留给 P2 | `DecisionContext.stimuli`, `constraints`, `WorldService::projectStimuli`, `src/evolution/evolution.*` |
| LLM 解释与权衡 | 对话提案已接入引用 Stimulus、tradeoffs、expected relief；高显著性自主审议基础链路已接入，异步 TurnQueue 待实现 | `IntentProposal`, `IProvider`, `WorldService::converse/deliberate` |
| 长过程执行 | 数据驱动时长、单一活动槽、资源预留、完成/挂起/恢复事件和 Snapshot 恢复均已实现；挂起释放预留，刺激清除且资源可用时按原计划恢复并顺延完成时间；Goal、Process、FSM、BT 是同一活动的四个投影 | `ProcessExecution`, `NpcActivity`, `WorldService::tickProcesses`, `resumeSuspendedProcesses`, `src/npc/state_machine.*` |
| 通用资源与实例活动 | 基础层已实现，生产/交易语义扩展属于 P1 | `ResourceContent`, `ResourceState(amount,reserved)`, `ActivityContent`, data-driven schedule effects；承诺关系增量与重谈时长已由 `promise_policy` 驱动 |
| 元动作语义目录 | 已冻结；故事动作 ID 由内容包绑定到通用 operator，行为配方在运行时编译为 BT | `src/world/meta_actions.*`, `src/npc/behavior_recipe.*`, `data/behaviors/village.json` |
| World Intelligence 演化提案 | 结构治理已完成，沙箱/激活待 P2 | `src/evolution/evolution.*`, `data/schemas/evolution-proposal.schema.json`; 校验白名单、禁止 Core mutation、独立审批已覆盖 |
| 3D 表现和 Blender 场景 | 已有 P0 原型 | 不参与当前平台模型验收 |

## 已完成验证

- 任意最小内容包可以启动，不依赖 Mira、Lorn、Eve 或村庄专用 API。
- 实体创建、休眠、激活和销毁经过事务，销毁身份保留在事实层并可从 Snapshot 恢复。
- Promise、资源、关系、NPC 日程和事件在两日离线推进中可观察。
- LLM 不可用时规则台词和世界推进继续工作。
- Snapshot/Replay 不访问 LLM；事件只在事务提交后发布。
- 需求缺口会随游戏时间增加，已提交动作会产生 `need.relieved`，跨阈值会产生 `need.critical`，需求状态可保存和恢复。
- 运行态 NPC 只保存 `definition_id`；快照引用已删除的内容或不同 `content_version` 时会明确拒绝加载。
- 第二个最小内容实例（`tests/fixtures/minimal-pack`，NPC `sable`）可以独立加载、保存和查询实体投影，证明 Runtime 不依赖村庄角色。
- 承诺的关系轴增量和重谈延长时长来自 `promise_policy`，修改内容即可改变结果。
- 行为树由 `recipes` 自动构造；Mira、Lorn、Eve 与故事动作没有专用 C++ BT。
- 对话与自驱由 `decision_channels` 声明；通道属性、话语权限、事件/记忆投影和 Provider 策略不再依赖 trigger 字符串分支。
- 嵌套配方失败只回滚自己的保存点，不会把所在世界时间步一起恢复；快照回滚不复制不可变事件历史。
- 27 个核心测试当前在本机以约 3 秒完成，单测耗时输出用于防止长期模拟回归。

## P0 工作顺序

### P0-J：内驱、外驱与自动适应（已完成）

1. 保持 `NeedDefinition/NeedState -> DriveSystem -> NeedEvent -> GoalSelector` 的确定性链路。
2. ~~把需求域收口为四域~~ 已完成：内容使用 `domain=physiological|psychological|social|cognitive`，每项具体需求绑定一个或多个可执行满足配方。
3. ~~实现显式 `relieve_need` 元动作~~ 已完成：由配方和来源事件驱动，LLM 不能直接设置缺口或权重。
4. ~~把外驱从文本拼接改为 Stimulus~~ 已完成：包含来源、效价、紧迫度、新颖性、可见范围、置信度、到期时间和显著性。
5. ~~实现有界预期适应~~ 已完成：学习率、步长、冷却期、版本、`drive.adapted` 和 `stimulus.habituated` 均进入 Snapshot/Replay；下一步才增加敏感度学习。
6. ~~在文本界面显示驱力上下文~~ 已完成：显示四域需求、deficit、drive、临界状态、外驱和约束。
7. ~~增加需求和适应回归~~ 已完成：覆盖衰减、临界、满足、适应、快照恢复和最小内容包。
8. ~~清理剩余内容特例，使 P0 行为通过内容包动作和注册规则表达，避免把 NPC ID 写进 Runtime 算法。~~ 已完成；Mock Provider 的关键词也由对白内容声明，Runtime 不按村庄动作 ID 分支。
9. ~~将当前村庄资源字段收口为通用资源组件/资源存储。~~ 已完成；生产、交易、合同和通信对象的完整语义扩展属于 P1。

### P0-N：结构自动演化（治理基线已完成）

1. ~~将 `StructuralConstraint` 从字符串改为可引用的事实投影，覆盖访问、权限、所有权、容量、配方、合同和信息范围。~~ 已完成当前 P0 的资源、拓扑、能力白名单投影；更丰富的制度字段留给 P1。
2. 已声明的常规结构变化走现有事务；新的规则/能力/配方/类型走 `EvolutionProposal`。
3. ~~P0 的声明式结构提案必须由独立的开发/设计审批策略批准；LLM 可以提出，不得批准自己的提案。~~ 已完成 `EvolutionManager::approve`。
4. Schema/不变量校验和审批记录已完成；密封回放沙箱、指标与版本化 overlay 激活属于 P2，当前只允许通过治理接口产生 Approved 状态。

### P0-K：意图和行动契约（已完成 P0 基线）

1. 将 Provider 结构化响应固定为 `IntentProposal`：候选动作、意图、理由、预期结果、引用 Stimulus、权衡、预期满足、置信度、上下文 Revision。
2. 校验模型返回的上下文版本、候选白名单、资源、权限、关系和幂等键。
3. ~~让一次有效决策启动持久化为 `ProcessExecution`，过程中不重复调用 LLM~~ 已完成：候选行为声明时长、可中断策略和显著性阈值；完成/中断才提交对应效果。`NpcActivity` 现在是唯一活动槽和生命周期状态源，Goal/Process/FSM/BT 不再各自维护生命周期。
4. 已提交行动统一形成 WorldEvent，并驱动资源、需求、记忆和下一轮外部刺激；更丰富的关系/合同元动作属于 P1。
5. 把真实 Provider 的自主审议接入异步 TurnQueue；当前同步路径只用于 Mock、测试和受控推进，不阻塞主循环是后续门槛。
6. 对模型关闭、模型迟到、过期提案、未知动作、重复提案、过程恢复和中断释放预留增加回归测试。

P0-K 的前三项以及模型关闭、过期提案、重复提案、过程恢复和中断释放预留已有回归覆盖。异步 Provider TurnQueue 属于 P1；当前同步路径只服务于受控推进。

### P0-L：小村庄因果演示（已完成）

1. 用文本界面展示“资源变化 → 内/外驱 → 意图 → 行动 → 他人/公共资源变化 → 次日计划变化”。
2. 记录每个节点的事实来源和规则原因，支持事件检查和回放差异。
3. 保持一个共享模型槽位；不让 LLM 阻塞游戏时间，也不让 LLM 直接修改 WorldState。
4. P0 通过后再进入港区内容，不提前实现商店、利率或政策沙箱。

### P0-M：世界对象与元动作建模（已完成 P0 基线）

已完成：运行态 NPC 只保存 `definition_id`，快照不复制内容定义；承诺关系增量与重谈期限由 `promise_policy` 数据驱动；实体 ID 前缀和游戏时间单位由共享运行时常量生成；Game Instance 清单与 Content Pack 清单分开；快照保存并校验 `instance_id`；第二个最小实例验证替换性。

1. 冻结 Entity/Agent、Resource、ProductionProcess、ConsumptionNeed、Exchange/Contract、Communication、Institution/Rule 和 Event 的属性契约。
2. 让内容加载器校验单位、所有权、容量、输入/输出、时间窗口、权限和引用，不接受只有名称没有语义属性的动作。
3. 完成 P0 元动作注册表：资源消耗/产出/转移/预留、需求满足、关系调整、移动、承诺、记忆、连接状态和实体生命周期。
   - 已完成执行器：`consume_resource`、`produce_resource`、`reserve_resource`、`release_resource`。
4. ~~把小村庄真实的配给、修船、档案整理和承诺行为组合为 BehaviorRecipe；不再为每个 NPC 添加 C++ 特例。~~ 已完成：`data/behaviors/village.json` 定义 meta actions、conditions 和 recipes，Runtime 编译 Sequence/Selector/Condition/Action/Timeout/Interruptible 节点；ActionCandidate 只引用配方，ProcessExecution 在完成时执行同一配方。
5. 为元动作、复合行为、事务失败和回滚补自动化测试；测试通过前不增加商店、利率或政策规则。

## 后续工作包

### P1：潮门港与可组合行为扩展

在现有 JSON Behavior DSL 上加入 Wait、补偿步骤和更丰富的资源/交易语义，再加入港区、家庭/组织指标、阵营关系、近远场模拟等级、完整 UI、物理机关和内容迁移。验收仍以可回放的事实链为准。

### P2：World Intelligence 演化和沙箱

实现 Evolution/Scenario Proposal Schema、提案库、来源事实、回放沙箱、指标评估、审批策略、版本化 overlay、激活、观察和回滚。只有已注册的声明式能力可以在线激活；新增 C++ 能力仍需开发者发布。

### P3：可替换世界实例

用同一 Runtime 加载新的小说世界，扩展多区域、阵营、经济和长期余波。现实世界实验只有在定义对应机制、数据边界和校准方法后才进入计划。

## 自动化验证命令

```bash
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/tide_marks_headless --scenario village --days 2 --provider mock --dump-events
./build/tide_marks_text --script docs/examples/village-session.txt --provider mock
./build/tide_marks_headless --scenario village --days 0 --profile data/intelligence-profile.json --provider profile
```

每次工作包完成后记录：内容版本、规则/模拟版本、Provider/模型（如有）、随机种子、命令、WorldEvent、Snapshot 截断点、Replay 摘要和失败原因。不得把 API Key 写入日志或回放工件。

## 风险与当前处理

- LLM 延迟或断开：规则决策和回退台词继续推进，模型不拥有提交权限。
- 内驱过于写死：参数、需求、动作满足关系和阈值来自 JSON；新驱动力必须走提案和沙箱，不在 C++ 添加 NPC 特例。
- LLM 越权：候选动作、结构化 IntentProposal、版本检查和 ActionValidator。
- 世界事实分叉：单一 WorldState 写入入口、事务、EventLog 和提交后 EventBus。
- 内容扩大后性能下降：近场/远端模拟 LOD 和事件唤醒，不让所有实体逐帧模拟。

### 本地 Qwen 验证记录

使用 llama.cpp 的 `Huihui-Qwen3.5-4B-abliterated.Q4_K_M`、OpenAI-compatible `/v1/chat/completions` 和 JSON Schema 约束验证 P0 意图输出。结构化短决策默认关闭 thinking；实测关闭 thinking 时返回合法 JSON，并由 Runtime 接受 `offer_ration`。显式设置 `reasoning_budget_tokens=256` 时，llama.cpp 仍正常结束响应，服务端生成量为 426 tokens，说明该字段确实限制思考段而不是被模板静默忽略。小模型不应默认开启长思考：它会显著增加延迟，并可能在 `max_tokens` 用尽前还没有产生最终 JSON。Prompt 同时明确本次请求允许的刺激 ID 和需求 ID，空刺激时强制 `cited_stimulus_ids=[]`，避免模型把需求 ID 当成外部刺激引用。

当前基准的推荐档位是：普通对话关闭 thinking；有社会权衡的决策使用 `reasoning_budget_tokens=128`；低置信度才升级到 256；512 以上留给演化分析。该分档基于同一模型的三类村庄提示实测，而不是单看生成长度。

Provider、模型和两档预算现在由 `data/intelligence-profile.json` 配置，并通过 `--profile` 加载。真实自驱验证中，玩家对白使用关闭 thinking 的 `player_dialogue` 档；当 Lorn 的 crossing safety 越过临界阈值后，`high_salience_context` 自动进入 `autonomous_reappraisal` 档，模型选择 `repair_ferry` 并提交了资源变化和过程完成事件。

## 恢复工作入口

会话恢复时依次阅读本文件的“当前检查点”和“P0 工作顺序”，再阅读 [design.md](design.md) 的“事实、意图与演化边界”“世界与社会的内在驱力模型”“NPC Runtime 与 World Intelligence”“Runtime 核心契约”。然后查看 `git status --short`，运行自动化验证命令，先修复当前工作包失败项，再继续下一项；不要为了通过测试删除功能或放宽断言。
