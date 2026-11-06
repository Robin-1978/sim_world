# 三 NPC 小村庄 7 日运行摘要

这份摘要从两份完整 Replay 的交易事件和 Provider usage 统计整理而来。两次运行都从第 1 天 06:00 推进到第 8 天 06:00，`pending_async_results=0`，Replay 与源状态一致。它把“世界发生了什么”“反思改了什么”和“创新是否真的进入并被采用”分开记录。

## 运行结论

| 运行 | Runtime 结果 | 反思 | 创新/演化 | 缓存证据 |
|---|---|---:|---|---:|
| 本地 Qwen 4B v11 | revision 10186；boat/records integrity 94/94 | 18 committed；13 个局部字段被跳过；2 次 provider 失败；1 次整条拒绝 | Level 2 `repair_ferry_v2` 通过 Runtime preflight → `runtime_governor` 自动批准 → Overlay active；Level 1 两次私有假设被拒 | decision 48.95%；reflection 54.59%；Level 2 32.29% |
| beam-free v14 | revision 10175；boat/records integrity 91/94 | 20 committed；4 个局部字段被跳过；1 次 provider 失败 | 1 次 Level 2 `no_change`；1 次 Level 2 provider 失败；4 个 Level 1 假设被 Runtime 拒绝；没有 approved/active Overlay | decision 20.21%；reflection 23.29%；Level 2 0%；合计 20.54% |

无凭据的 `beam-free-auth-preflight-7d` 只证明 Runtime 在认证缺失时可以 fail-closed 地推进时间、需求衰减和已批准过程；它没有模型质量、缓存或演化证据，不能与上表混合。

## 七日内实际发生的行为

Qwen v11 记录了 60 个自主意图、57 个完成过程和 3 个失败过程；beam-free v14 记录了 67 个自主意图、58 个完成过程和 8 个失败过程。三名 NPC 都持续经过完整的 `intent → ProcessExecution/BT → process_completed/failed → experience` 链，失败不会伪造成功事实。

Qwen v11 的完成过程按主体为：Eve 25、Lorn 14、Mira 18；失败全部来自 Lorn 的 `repair_ferry`（3 次）。主要动作是：Eve `withhold_record` 22 次、`reveal_clue` 3 次；Lorn `repair_ferry` 2 次完成、`offer_crossing` 12 次完成；Mira `explain_ration` 12 次、`offer_ration` 6 次。最终资源为 `boat_integrity=94`、`records_integrity=94`、`cooperation=6`、`grain=0`。

beam-free v14 的完成过程按主体为：Eve 28、Lorn 15、Mira 15；Lorn 的 `repair_ferry` 失败 6 次，Mira 的 `offer_ration` 失败 2 次。主要动作是：Eve `withhold_record` 24 次、`reveal_clue` 4 次；Lorn `offer_crossing` 12 次、`repair_ferry` 2 次；Mira `offer_ration` 7 次、`explain_ration` 8 次。最终资源为 `boat_integrity=91`、`records_integrity=94`、`cooperation=5`、`grain=0`。

这些差异是模型选择和运行时事实的结果，不是 Tick 由模型控制。每个过程启动后，Runtime 按声明式 BT 和元动作耗时推进；LLM 只在决策/反思唤醒点参与。

## 反思实际提交了什么

Qwen v11 产生 82 条 `experience.recorded`、24 条 `knowledge.asserted` 和 17 条 `memory.reflected`；beam-free v14 产生 66 条 Experience、17 条 Knowledge 和 15 条 Memory。Runtime 只接受带有游标窗口内来源事件的字段，坏字段被局部拒绝，合法认知更新仍可提交。

beam-free v14 还出现了两次真实策略调整：Lorn 从 `routine` 调整为 `repair_ferry_focus`，随后调整为 `offer_crossing_focus`。这说明 Reflection 可以改变已有策略而不必生成 Innovation Proposal。Qwen v11 没有产生 `strategy.reflected`，三名 NPC 最终仍为 `routine`。

## 创新、Overlay 和 Adoption 的真实边界

Qwen v11 的唯一有效演化是 Level 2 提案 `repair_ferry_v2`：

1. `evolution.proposal_ready`（sequence 966）通过 Runtime preflight；
2. `evolution.approved`（967）由 `runtime_governor` 自动批准；
3. `evolution.overlay_activated`（968）把内容版本切到 `village-p0.5+overlay:repair_ferry_v2`；
4. Overlay 的唯一 operation 是 `create_entity_archetype`，声明了 `repair_tool` 和 `tool_quality` 元数据。

因此它证明了 Level 2 的“提案 → Runtime 验证 → 自动批准 → Overlay 激活”闭环，也证明了激活不是 Adoption：7 日事件中没有 `innovation.adoption_started/completed/failed`，没有 NPC 真正采用 `repair_ferry_v2`。当前仍缺一个有效的共享 Behavior Recipe 提案，才能证明“激活后被候选目录选中并运行一段 horizon，再由 Runtime 指标和后续 Reflection 评估”的完整效果闭环。

Qwen v11 的两次 Level 1 私有创新被拒：一次 `invalid_behavior_parameters`（参数没有声明 `type=object`），一次 `invalid_behavior_recipe`（`repair_ferry_v2` 作为复合节点却没有子节点）。认知事务没有被回滚成假事实。beam-free v14 的四次 Level 1 假设分别因 operation scope、`test_plan.horizon_minutes` 越界、未知元动作 `repair_ferry_private` 和 operation scope 被拒；因此该运行没有私有 Overlay。

## 缓存和上下文判断

当前证据不支持“95% 命中”：beam-free 总体只有 20.54%，Qwen 的不同阶段约 32%–55%。这不是删除必要世界数据的理由。缓存率、完整 prompt token 比例和模型质量是三个不同指标。

动态时间、资源、需求、关系、当前活动、刺激、游标后事件和 Adoption 指标必须保留，否则模型无法解释真实世界。可缓存的边界是：固定系统规则、固定工具 schema、内容动作/BT 描述和稳定的协议字段；动态事实应该放在尾部，并以增量证据和有界窗口组织。决策请求现在只把 `protocol`、speech-act catalog 和去掉运行时 utility 的 authored plan catalog 放在 stable prefix；`constraints`、主体身份、角色/策略、需求、世界快照、当前活动和玩家输入全部留在 dynamic tail，避免每次 Tick 的事实变化使“稳定前缀”失效。

下一次有 beam-free 凭据时，必须重新跑同一 7 日命令，按 stage 同时记录 `prompt_tokens`、`cached_tokens`、dynamic tail bytes、finish reason、repair 次数、Overlay/Adoption 和 Replay equality。只有 Provider usage 或 server slot 指标能证明真正命中，不能用请求复用次数推断。

证据原始文件：

- [qwen35-4b-llama-cpp-v11.json](qwen35-4b-llama-cpp-v11.json)
- [beam-free-opencode-7d-v14.json](beam-free-opencode-7d-v14.json)
- [beam-free-auth-preflight-7d.json](beam-free-auth-preflight-7d.json)
