# Co-op GAS 与多人网络必问 Top 20

> **归档题库（不再维护项目状态）**：完整、去重后的当前口述内容只维护在[面试复习资料](GAS_Interview_Completed_Snapshot.md)。本文件仅保留扩展题目，不得新增任务状态或完成矩阵。

> 适用项目：UE5.5 `multiplayer` / `coop-GAS`
>
> 更新日期：2026-08-16
>
> 使用方式：先阅读[《Co-op GAS 已完成内容：面试速查》](GAS_Interview_Completed_Snapshot.md)，再回答每题的“30 秒版本”，并结合项目调用链和证据展开。未完成内容必须使用“计划/待验证”，不能说成已经实现。

---

## 0. 回答原则

每道题使用四层结构：

```text
概念结论
-> 为什么这样设计
-> 当前项目的具体实现
-> 边界、风险和验证证据
```

面试时最危险的回答方式是只背 API 名称。正确回答应该能落到“谁拥有状态、哪端有权修改、复制什么、失败时如何校正”。

---

# 第一部分：GAS 必问 Top 10

## GAS-01：GAS 的核心组件有哪些？

### 30 秒回答

GAS 的核心不是一个组件，而是一套协作模型：ASC 保存和复制能力、Active GE 与 GameplayTag；AttributeSet 定义属性和结算入口；GameplayAbility 表达技能流程；GameplayEffect 表达数值和状态变化；GameplayCue 负责可复制/可预测的表现；AbilityTask 封装异步流程；GameplayTag 负责身份、状态、阻止和查询。ASC 是这些系统的运行枢纽。

### 组件职责

| 类型 | 职责 | 不应该承担 |
|---|---|---|
| `UAbilitySystemComponent` | AbilitySpec、Active GE、Tag、预测、RPC、属性委托 | 具体角色动画资产和关卡规则 |
| `UAttributeSet` | 属性定义、复制、Clamp、Meta Attribute 最终收口 | 输入和目标选择 |
| `UGameplayAbility` | 激活条件、技能时序、Task、Commit、结束/取消 | 客户端决定最终伤害 |
| `UGameplayEffect` | Instant/Duration/Infinite 数值和状态规则 | 复杂异步技能流程 |
| `UGameplayEffectExecutionCalculation` | 服务器复杂数值计算 | 客户端预测的最终权威结算 |
| `UAbilityTask` | TargetData、Montage、等待事件等异步节点 | 永久共享状态 |
| `GameplayCue` | 特效、声音、材质等表现 | Gameplay 权威状态 |
| `GameplayTag` | 能力身份、状态、免疫、Cooldown、输入和查询 | 大量无结构字符串比较 |

### 当前项目

- ASC/AttributeSet 位于 [multiplayerGASPlayerState.cpp](../Source/multiplayer/Player/multiplayerGASPlayerState.cpp)。
- 三个能力位于 [multiplayerGameplayAbility.cpp](../Source/multiplayer/AbilitySystem/Abilities/multiplayerGameplayAbility.cpp)。
- TargetData Task 位于 [multiplayerAbilityTask_TargetActor.cpp](../Source/multiplayer/AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.cpp)。
- GE、Tag、AbilitySet、服务器 ExecCalc、自定义 EffectContext 和 Vulnerability 堆叠均已实现；AttributeSet 还包含复制的 AttackPower、Armor、CriticalChance、CriticalMultiplier、Resistance。
- 7 个业务 GameplayCue 加 1 个仅用于 M6 的 Prediction Pending 实验 Cue，当前由原生 Handler 和 PointLight 技术占位表现消费；正式 Niagara/音效/Montage 资产仍未完成。

### 常见追问

**为什么机关不全部使用 GAS？** 压力板、门和钥匙没有 Cost、Cooldown、预测和 Effect 生命周期需求；继续使用服务器权威 Actor 更简单。使用 GAS 应由需求决定，而不是为了统一技术名词。

---

## GAS-02：技能从按键到生效的完整流程是什么？

### 30 秒回答

输入先映射为 InputTag，ASC 用 Tag 找到 AbilitySpec，调用 `TryActivateAbility`。LocalPredicted 能力在拥有客户端创建 PredictionKey 并预测可预测内容；TargetData 通过 SpecHandle 和 PredictionKey 发送服务器；服务器重新验证目标和 Commit；权威 GE 进入 AttributeSet，最终属性复制给客户端，GameplayCue/UI 再表现结果。

### 当前项目调用链

```text
鼠标左键
-> Character::AbilityInputTagPressed
-> ASC::AbilityInputTagPressed(InputTag.Ability.Damage)
-> 遍历 AbilitySpec Dynamic Source Tags
-> TryActivateAbility
-> DamageAbility::ActivateAbility
-> TargetActor AbilityTask 从相机做本地预览 Sweep
-> FScopedPredictionWindow
-> CallServerSetReplicatedTargetData(DamageIntent: ShotId/Origin/Direction/Time)
-> Server 按 SpecHandle + ActivationPredictionKey 接收
-> 验证 Schema/source/ShotId/频率/时间/Origin/方向
-> Server 在当前世界 Sweep 重建 HitResult
-> 验证 Team.Enemy / 非 Team.Player / 存活
-> CommitAbility(Cost + Cooldown)
-> Apply Damage GE
-> Server ExecCalc 捕获 Source Snapshot 进攻属性与 Target Live 防御/生命属性
-> 计算 (Base+AttackPower) * Armor * Resistance * Vulnerability * Critical
-> 自定义 EffectContext 携带 Critical / HitType / ImpactImpulse
-> IncomingDamage
-> AttributeSet::PostGameplayEffectExecute
-> 权威 Damage.Impact Cue + Health 复制
```

### 为什么 Commit 在服务器目标验证之后

如果服务器已经知道意图非法，先 Commit 再取消会无意义地消费服务器资源状态。Damage 只在服务器意图校验、权威 Sweep，以及权威目标/目标 ASC/DamageSpec 依赖验证通过后才调用 `CommitAbility`。语义拒绝不使用 `ClientActivateAbilityFailed`；`ClientDamageIntentResult` 只报告原因，预测 Cost/Cooldown 交给 PredictionKey 对账收敛，不手工退款。

### 易错点

- `TryActivateAbility` 成功不等于最终 Gameplay 结果一定发生。
- Anim Montage 播放不等于服务器已经接受目标。
- TargetData 是客户端候选意图，不是权威命中结论。

### ExecCalc 的当前取舍

AttackPower/CriticalChance/CriticalMultiplier 在创建 outgoing Spec 时 Snapshot，使同一次施法使用稳定的来源快照；Health/MaxHealth/Armor/Resistance 在执行时 Live Capture，使命中结算看到目标当时的防御和生命。客户端不提交暴击结果，真实 Roll 只在服务器 Execution 中生成；纯函数测试显式传入 Roll，才能稳定覆盖 0%、100% 和低血量 Critical。结果通过 `IncomingDamage` 进入 AttributeSet，自定义 Context 只携带已有 Cue 消费者使用的 Critical、HitType 和 ImpactImpulse。

---

## GAS-03：ASC 应该放在 PlayerState 还是 Character？

### 30 秒回答

取决于状态是否需要跨 Pawn 生存。放 Character 最简单，适合 AI 或死亡后完全重建的单位；放 PlayerState 能跨死亡、重生和换 Pawn 保留能力、Cooldown 或长期 Buff，但必须正确处理 OwnerActor=PlayerState、AvatarActor=Character，并在服务器 Possess 和客户端 PlayerState 复制后重新初始化。

### 当前选择

```text
OwnerActor = multiplayerGASPlayerState
AvatarActor = multiplayerCharacter
```

服务器入口：`PossessedBy`。

客户端入口：`OnRep_PlayerState`。

当前还增加了初始化幂等检查：ASC、AttributeSet、Owner 和 Avatar 组合未变化时不重复广播 `OnAbilitySystemInitialized`。

### 方案比较

| 位置 | 优点 | 成本 | 适用对象 |
|---|---|---|---|
| Character | 初始化直接、复制链短 | 死亡/换 Pawn 会丢失状态 | 普通 AI、一次生命单位 |
| PlayerState | 跨 Pawn 持久化、UI 可稳定绑定 Owner | ActorInfo 重绑复杂；PlayerState 更新和相关性需测量 | 玩家、重生/换角色玩法 |

### 易错点

- ASC 放 PlayerState 不代表所有 GE 都应该跨死亡保留。
- `InitAbilityActorInfo` 不等于重新授予 Ability。
- Mixed 模式下 Owner 链必须正确，否则 Owner-only 数据复制会异常。

---

## GAS-04：Full、Mixed、Minimal 三种复制模式分别适合什么场景？

### 30 秒回答

Full 向所有相关客户端复制完整 GameplayEffect，适合玩家数量少或调试；Mixed 给拥有者完整 GE，其他客户端主要得到 Tag/Cue 等必要信息，适合玩家；Minimal 不向普通客户端复制完整 GE，只保留必要 Tag/Cue，适合大量 AI。属性复制独立于这三种 GE 复制模式。

| 模式 | Active GE | Tag/Cue | 典型用途 |
|---|---|---|---|
| Full | 所有相关客户端完整复制 | 复制 | 少量重要 Actor、调试 |
| Mixed | Owner 完整；非 Owner 精简 | 复制 | 玩家 |
| Minimal | 不给普通客户端完整 GE | 复制必要信息 | 大量 AI |

### 当前项目

- PlayerState ASC 使用 Mixed。
- M0 训练目标使用 Full，便于双端检查；正式大量 AI 应重新评估 Minimal。
- PlayerState 当前 30Hz/10Hz 只是基线，不是已经证明的优化结论。

### 易错点

“Minimal 不复制属性”是错误说法。AttributeSet 属性仍按自己的 Replication 规则复制；模式主要决定 GameplayEffect 复制粒度。

---

## GAS-05：GAS 的客户端预测怎么做？PredictionKey 是什么？

### 30 秒回答

LocalPredicted Ability 会在拥有客户端先激活，由 ASC 为这次预测操作建立 PredictionKey。预测产生的 GE、Cue、TargetData 等都与这个 Key 关联；服务器使用同一激活身份接受或拒绝，客户端才能知道哪些预测修改应该确认、Catch-up 或回滚。PredictionKey 是预测事务身份，不是防作弊凭证，也不是任意 RPC 序号。

### 当前项目链路

```text
LocalPredicted Ability
-> ActivationPredictionKey
-> AbilityTask 创建 FScopedPredictionWindow
-> ServerSetReplicatedTargetData(
     SpecHandle,
     ActivationPredictionKey,
     TargetData,
     ScopedPredictionKey)
-> Server AbilityTargetDataSetDelegate
-> ConsumeClientReplicatedTargetData
```

### 可以预测什么

- Ability 激活响应。
- 合适的 Cost/Cooldown GE。
- Montage 和可回滚 GameplayCue。
- 部分属性修改。

### 不应该由客户端预测为最终结果

- 最终伤害和暴击。
- ExecCalc 权威结果。
- 目标是否合法。
- 死亡、奖励和胜利。
- 无法撤销的任意外部副作用。

### 易错点

PredictionKey 解决的是预测操作关联和校正，不证明请求合法；服务器仍必须验证目标、资源、状态、时间和请求频率。

---

## GAS-06：预测回滚具体回滚什么？

### 30 秒回答

GAS 只能自动协调与 PredictionKey 关联、且系统知道如何逆转的预测状态，例如预测 GE、属性聚合、Cost、Cooldown 和 Cue。服务器拒绝后，这些预测记录可以被删除或校正。普通 Actor Spawn、手写变量、外部存档、任意声音和不可逆 Gameplay 副作用不会自动回滚，必须使用预测代理、Cue 或显式清理。

### 回滚分类

| 内容 | 能否依赖 GAS 自动校正 | 说明 |
|---|---|---|
| 预测 Cost GE | 可以，需正确 PredictionKey | 服务器拒绝后移除预测修改 |
| 预测 Cooldown GE | 可以，需验证体验 | 服务器确认时还要处理预测/权威时间差 |
| 预测属性聚合 | 可以处理受支持修改 | 最终值仍以服务器为准 |
| GameplayCue | 可预测，但必须设计去重/Remove | 循环 Cue 尤其要收口 |
| Montage | 部分由 Ability 流程协调 | 中断、Section 和 Root Motion 需额外验证 |
| 任意 Actor Spawn | 不会自动撤销 | 常用本地假体+权威 Actor 对齐 |
| 手写 bool/文件/奖励 | 不会自动撤销 | 必须避免预测或显式补偿 |
| 最终伤害/死亡 | 不应由客户端决定 | 服务器权威后复制 |

### 当前项目证据边界

M6 使用无 TargetData 的 Immunity 隔离实验触发 `ClientActivateAbilityFailed`，验证激活入口回滚。DamageIntent 则单独验证激活已接受后的语义拒绝：0ms/约 300ms RTT 各52/52 PASS，Duplicate、Origin、Direction、Stale、Future、Miss 不产生权威伤害，Energy/CD 最终收敛。TargetData 等待的 5 秒超时、Task 生命周期清理和 Commit 前依赖验证已有代码收口；`TargetDataTimeout`、`SourceDead`、`InvalidTarget`、`CommitFailed` 仍缺专项双进程端到端分支。两条链不能混为一种回滚。

### 实验标准

必须记录 PredictionKey、客户端预测时间、服务器拒绝原因、Cost/Cooldown 变化、Cue 创建/删除和最终属性一致性。

---

## GAS-07：GameplayEffect 三种 Duration Policy 分别是什么？

### 30 秒回答

Instant 立即修改 Base Value 或 Meta Attribute，应用后不形成长期 Active GE；Duration 在有限时间内持续存在；Infinite 没有自动结束时间，必须显式移除。Periodic 是 Duration/Infinite GE 的周期执行属性，不是第四种 Duration Policy。

| 类型 | 典型场景 | 注意点 |
|---|---|---|
| Instant | 伤害、治疗、一次资源恢复 | 通常通过 Meta Attribute 收口复杂结算 |
| Duration | 5 秒免疫、10 秒 Buff、短期 Debuff | 到期、刷新、堆叠、晚加入剩余时间 |
| Infinite | 装备加成、被动、常驻状态 | 必须保存 Handle 或按 Query/Tag 移除 |
| Periodic | DOT、HOT、Energy 恢复 | 周期是否首次立即执行、叠层和 Tick 成本 |

### 当前项目

- Damage/Healing 是 Instant。
- Immunity 是 Duration。
- AbilitySet 支持保存已应用 GE Handle 并撤销。
- Infinite 和正式 Periodic 恢复尚未形成玩法验收。

---

## GAS-08：冷却和消耗如何实现？

### 30 秒回答

Cost 和 Cooldown 通常使用 GameplayEffect。`CommitAbility` 会检查并提交两者：Cost 修改 Energy 等资源；Cooldown GE 授予 `Cooldown.*` Tag，Ability 在激活条件中检查对应 Tag。这样 Cost/Cooldown 能进入预测、复制和聚合系统，而不是散落在 Timer 和 bool 中。

### 当前项目

```text
Damage:   Energy 10 / Cooldown 1s
Heal:     Energy 20 / Cooldown 3s
Immunity: Energy 30 / Cooldown 8s
```

对应 Tag：

```text
Cooldown.Ability.Damage
Cooldown.Ability.Heal
Cooldown.Ability.Immunity
```

### 设计要点

- 先检查目标还是先 Commit，要按玩法事务决定；当前 Damage 先由服务器验证目标。
- UI 不应该自己倒计时作为权威，应读取 Active GE 剩余时间。
- Cooldown 时长可使用 GE ScalableFloat 或 SetByCaller，但不能信任客户端最终数值。
- Energy 不足时不能只禁用按钮，服务器 Commit 仍要拒绝。

---

## GAS-09：GameplayTag 系统底层是什么？

### 30 秒回答

GameplayTag 是由中央 Tag Manager 注册的层级化标识，运行时通过 `FGameplayTag` 和 `FGameplayTagContainer` 做相等、父子匹配和查询，不应当成任意字符串。Tag 可以来自 ASC Owned Tags、Active GE Granted Tags、Effect Asset Tags、AbilitySpec Dynamic Tags 等不同来源；来源不同决定生命周期和网络语义。

### 关键数据结构

- `FGameplayTag`：单个已注册 Tag。
- `FGameplayTagContainer`：Tag 集合，支持 Exact/父级匹配。
- `FGameplayTagQuery`：AND/OR/NOT 组合查询表达式。
- Native Gameplay Tag：C++ 启动时注册，减少字符串散落。

### 当前项目用法

| Tag | 用途 |
|---|---|
| `InputTag.Ability.*` | 输入找到 AbilitySpec |
| `Ability.*` | 能力身份 |
| `Cooldown.Ability.*` | 冷却阻止 |
| `State.Immune/Dead` | Gameplay 状态 |
| `Team.Player/Enemy` | 合作阵营与友军保护 |
| `Effect.Negative.*` | 免疫 Query |
| `Data.Damage/Heal` | SetByCaller 数值键 |

### 易错点

- Asset Tag 只描述 Effect 身份，不等于给目标授予该 Tag。
- Loose Tag 的生命周期需要调用方管理；GE Granted Tag 随 Active GE 生命周期自动变化。
- `HasTag` 与 `HasTagExact` 语义不同。
- 网络快速复制依赖两端 Tag 字典一致，不能运行时随意生成未注册 Tag。

---

## GAS-10：Buff/Debuff 堆叠规则如何配置？

### 30 秒回答

先决定聚合身份：Aggregate by Source 表示同一来源的层数合并，不同来源分别建栈；Aggregate by Target 表示目标上的同类效果共享一栈。然后配置最大层数、再次应用时是否刷新 Duration、是否重置 Period、Overflow 行为以及栈清零后的移除。堆叠规则必须与 UI、免疫、死亡和晚加入一起验证。

### 方案比较

| 策略 | 示例 | 结果 |
|---|---|---|
| Aggregate by Source | 每个敌人的毒独立叠层 | 不同敌人分别维护 Stack |
| Aggregate by Target | 所有来源共享“破甲”层数 | 目标只维护一组总层数 |

### 必须明确的配置

- `StackLimitCount`。
- Duration Refresh Policy。
- Period Reset Policy。
- Expiration Policy。
- Overflow Effects/是否拒绝溢出应用。
- 移除一层还是整栈。
- Stack Count 如何显示和复制。

### 当前项目证据边界

当前已实现 `State.Vulnerable`：Aggregate by Target、最多 3 层、每次合法施加刷新 8 秒、拒绝第 4 层溢出、整组到期，并将每层增伤交给服务器 ExecCalc。自动化覆盖配置/公式；0ms 与弱网运行证明同一生命周期内叠层不会重复 OnActive/WhileActive，且自然到期会 Removed。存活目标的独立第 4 层溢出、层数 UI、死亡中断和晚加入仍待专门验收。

---

# 第二部分：多人网络必问 Top 10

## NET-01：ENetRole 有哪些角色？

### 30 秒回答

从某一台机器本地看，Actor 的 LocalRole 主要有 Authority、AutonomousProxy 和 SimulatedProxy。Authority 表示该实例拥有权威状态；AutonomousProxy 通常是拥有客户端自己的 Pawn，能产生输入和预测；SimulatedProxy 是其他客户端上的远端代理，主要消费复制状态和插值。Role 是每个实例相对于同一 Actor 的视角，不是给 Actor 永久贴的全局标签。

| LocalRole | 典型位置 | 主要职责 |
|---|---|---|
| Authority | Server | 修改权威 Gameplay 状态 |
| AutonomousProxy | Owning Client | 输入、客户端预测、Server RPC |
| SimulatedProxy | Non-owning Client | 接收复制、模拟和插值 |
| None | 未参与复制/特殊阶段 | 不应承担正常网络 Gameplay |

### 当前项目证据

按 `1` 会输出 NetMode、LocalRole、RemoteRole、Authority、LocallyControlled，以及 ASC Owner/Avatar/能力数量。M0 日志已经观察到 Server Authority、Client AutonomousProxy 和 SimulatedProxy。

### 易错点

Listen Server 本地玩家同时在服务器进程中，不需要经过远程网络才能调用服务器逻辑，但设计仍要保持与远程 Client 相同的权威规则。

---

## NET-02：RPC 有哪些类型？分别适合什么场景？

### 30 秒回答

UE 常用 Server RPC、Client RPC 和 NetMulticast RPC。Server RPC 从拥有客户端向服务器提交意图；Client RPC 由服务器发给某个拥有连接；Multicast 由服务器发给相关客户端。Reliable 保证有序最终送达但会阻塞可靠通道，Unreliable 允许丢失，适合高频、可被新状态覆盖的事件。RPC 能否到达首先取决于 Actor Ownership 和网络连接。

| 类型 | 调用端 | 接收端 | 适用例子 |
|---|---|---|---|
| Server | Owning Client | Server | 交互请求、Session 内 Gameplay 意图 |
| Client | Server | Owning Client | 仅该玩家的确认、错误提示 |
| NetMulticast | Server | Server + relevant clients | 短暂非持久表现；慎用 |

### 当前项目

- Character 的正式业务 Server RPC 当前用于提交合作游戏重开意图；Damage 技能使用 GAS TargetData 内部 Server RPC，而不是自定义“提交伤害 RPC”。
- DeveloperHarness/ASC 的测试 RPC 只在显式实验参数和非 Shipping/Test 路径服务于 Reject、DamageIntent 与训练目标证据。
- Owner-only Client RPC 用于 DamageIntent 业务结果和拒绝实验回执。
- 当前源码没有自定义 NetMulticast；短暂技能表现主要使用 GAS GameplayCue。
- GAS 的 TargetData 通过 ASC 内部 Server RPC/Delegate 链传递，而不是自定义一个“提交伤害 RPC”。

### 易错点

- Client 在不拥有的 Actor 上调用 Server RPC 通常不会按预期到达。
- Multicast 不保存持久状态，晚加入无法重放。
- Reliable 不是“更高级”，高频滥用会造成队头阻塞。

---

## NET-03：属性同步和 RPC 有什么区别？

### 30 秒回答

属性复制描述“当前状态是什么”，服务器把最终值同步给相关客户端，适合持久状态和晚加入；RPC 描述“发生了一次事件或请求”，只在调用时发送，适合输入意图和短暂通知。属性复制会合并中间变化并受相关性影响，RPC 不应当作长期状态存储。

| 需求 | 推荐 |
|---|---|
| Health、钥匙数、门是否打开 | Replicated Property / RepNotify |
| 玩家请求交互/开火 | Server RPC / GAS TargetData |
| 仅 Owner 的失败提示 | Client RPC 或 Owner 状态 |
| 短暂非关键特效 | GameplayCue/必要时 Multicast |
| 晚加入必须知道 | 属性、Active GE/Tag，不是过去 RPC |

### 当前项目

- Health/Energy、ObjectiveState、门和压力板使用属性/语义状态复制；移动平台由服务器移动并通过 `ReplicateMovement` 同步 Transform，同时复制玩家计数等语义状态。
- OnRep/Delegate 将状态变化转给本地 UI/表现。
- 玩家 Gameplay 请求使用 RPC/GAS TargetData；Session 使用 OnlineSubsystem 异步 Delegate 与 Travel，不是项目自定义 Session RPC。
- 胜利由 GameState 复制 `bGameWon`，不是依赖一次 Multicast。

---

## NET-04：什么是客户端预测？

### 30 秒回答

客户端预测是在服务器确认前，拥有客户端先根据相同输入执行可校正的本地结果，降低输入到反馈的等待。服务器仍模拟并决定权威状态；收到结果后客户端确认或校正。预测优化的是响应，不是把权威交给客户端。

### 通用预测链

```text
Client Input
-> 保存输入/事务身份
-> 本地预测移动或技能反馈
-> 上传输入意图
-> Server 权威执行
-> 接受：确认预测
-> 拒绝/不同：校正并重放后续输入
```

### 当前项目

- GAS 三能力使用 LocalPredicted。
- TargetData 带 PredictionKey 上传。
- 最终 Damage/Healing 只由服务器应用。
- 激活入口 Reject 回滚实验已完成；DamageIntent 语义拒绝、ShotId 幂等与当前世界服务器重建命中也已完成核心实现与 0ms/约 300ms RTT 验证。

### 易错点

预测不等于插值；预测解决本地输入响应，插值解决远端对象显示平滑。

---

## NET-05：服务器校正移动是如何处理的？

### 30 秒回答

CharacterMovement 会让 AutonomousProxy 本地预测移动并保存未确认 Move。客户端把压缩输入和时间信息发送服务器；服务器权威模拟并确认或返回位置误差。客户端收到校正后回到服务器状态，移除已确认 Move，再重放尚未确认输入；视觉层通过网络平滑避免瞬移感。服务器不是简单每帧复制 Transform。

### 关键概念

- Saved Move / Pending Move。
- Client timestamp 和 Server response。
- Ack 后删除已确认输入。
- Correction 后 replay 未确认输入。
- SimulatedProxy 使用插值/外推和平滑。

### 项目边界

当前项目使用 UE CharacterMovement 默认预测，没有自定义 SavedMove 或 Movement Mode。作品集不把引擎默认能力写成自己实现；能说明原理和与 GAS PredictionKey 的区别即可。

### GAS 与移动预测的区别

CharacterMovement 用 Move 序列和重放输入校正位置；GAS 用 PredictionKey 关联预测 Ability/GE/Cue。二者可以在一个技能中协作，但不是同一套事务系统。

---

## NET-06：延迟补偿如何实现？

### 30 秒回答

FPS Hitscan 常用服务器回溯：服务器保存一小段目标碰撞历史；客户端提交受限的开火时间、起点、方向和 ShotId；服务器验证时间窗与请求合法性，在历史查询状态上重新 Trace，再用当前权威规则结算。它补偿客户端“开火时看到的位置”，但不能信任客户端命中或伤害。

### 计划中的项目链路

```text
Client LocalPredicted Hitscan
-> ShotId + 同步 ServerTime + Trace Origin/Direction
-> Server 验证能力、状态、Cost、Cooldown、频率
-> 验证时间非未来、非过旧（最大 250～500ms）
-> 验证 Origin 与权威角色位置偏差、方向、射程
-> 查询目标历史碰撞快照
-> Rewind Trace
-> 恢复查询状态
-> ExecCalc 权威伤害
-> ShotId 幂等记录
```

### 必须防的异常

- 未来时间戳。
- 超出最大回溯窗口。
- 起点远离权威角色。
- 非归一化/异常方向。
- 重复 ShotId。
- 请求频率超过技能允许值。
- 目标已经销毁或不属于敌方。

### 当前证据边界

服务器回溯尚未实现，属于 M7。当前已有 DamageIntent 时间/Origin/方向/ShotId 校验与当前世界权威 Sweep；时间字段只用于 freshness 门禁，没有历史快照选择，不能声称已有延迟补偿。

---

## NET-07：什么是实体插值？

### 30 秒回答

实体插值通常让远端代理显示在“当前时间稍后退”的渲染时间，从接收到的两个权威 Snapshot 之间插值位置和旋转。它用少量显示延迟换取平滑，适合 SimulatedProxy。若没有未来样本只能短暂外推，误差过大再校正。拥有者本地输入一般用预测，不用同样的延迟插值。

### 与其他机制区别

| 机制 | 解决的问题 | 典型对象 |
|---|---|---|
| 预测 | 本地输入立即响应 | AutonomousProxy |
| 插值 | 远端状态平滑显示 | SimulatedProxy |
| 服务器回溯 | 服务器按历史时间判定命中 | Hitscan/特定攻击 |
| 属性复制 | 最终 Gameplay 状态一致 | Health、门、目标进度 |

### 当前项目

角色移动使用引擎 CharacterMovement 的网络平滑；门、压力板和移动平台使用复制状态与本地运动表现。尚未编写通用 Snapshot Interpolation 框架，也没有必要为当前两人 Demo 重造一套。

---

## NET-08：Listen Server 和 Dedicated Server 有什么区别？

### 30 秒回答

Listen Server 同时是服务器和一名本地玩家，部署简单但 Host 有零网络往返优势、渲染占用服务器资源，并且 Host 退出会影响会话。Dedicated Server 没有本地玩家和渲染，权威环境更公平稳定，但需要独立构建、部署和运维。代码不能依赖服务器一定有 LocalPlayer、Viewport 或本地 UI。

| 项目 | Listen Server | Dedicated Server |
|---|---|---|
| 本地玩家 | 有 | 无 |
| 渲染/UI | 有 | 无 |
| Host 延迟优势 | 有 | 无本地主机玩家 |
| 部署成本 | 低 | 高 |
| 适合 | 小型合作、原型 | 正式在线服务、公平性要求高 |

### 当前项目

当前主要测试模式是 Listen Server + Client。Dedicated Server 编译和双客户端整局属于 M8，尚未验证。

---

## NET-09：这个 Co-op 项目具体如何做同步？

### 30 秒回答

项目采用服务器权威状态同步：客户端提交交互或技能意图；GameMode 管规则和初始化，不复制；GameState 保存团队钥匙进度和胜利状态；PlayerState 保存玩家 ASC/属性；门、压力板、平台、钥匙和 WinArea 都在服务器判断，复制结果给客户端；表现层通过 OnRep 和 Delegate 更新。集合用角色身份去重，结算使用幂等门禁。

### 核心系统

| 系统 | 权威状态 | 同步方式 |
|---|---|---|
| Session | GameInstance/OnlineSubsystem 异步流程 | Session Delegate + Travel |
| 压力板 | Server 统计唯一 Character 占用 | 复制激活状态 + Delegate |
| 门 | Server 根据所需压力板计算目标 | 复制语义状态，各端本地插值 |
| 移动平台 | Server 根据压力板/人数决定移动 | 服务器移动 + `ReplicateMovement`，另复制玩家计数 |
| 钥匙 | Server 拾取、安装和插槽激活 | RepNotify/引用复制 |
| 团队目标 | GameState 的 ActivatedKeys/RequiredKeys/bGameWon | 结构体属性复制 + OnRep |
| 胜利区 | Server 的玩家弱引用 TSet | 满足条件时调用 GameState 幂等结算 |
| 胜利 UI | 本地 VictoryPresenter 消费复制结果 | C++ 仅一次转发 Character `On Coop Game Won`；蓝图创建 `winandquit`，中文按钮经 Character RPC 提交重开意图 |
| GAS | PlayerState ASC Mixed；客户端预测意图 | GE/Attribute/Tag 复制 + TargetData |

### 关键保护

- 玩家多个碰撞组件不会重复计数。
- 玩家销毁时从压力板、平台和 WinArea 清理。
- `TryCompleteGame` 的 `bGameWon` 保证胜利只结算一次。
- VictoryPresenter 在 EndPlay/Refresh 对称解除 GameState 委托，用 `bVictoryNotified` 防止重复转发；Widget 引用、RemoveFromParent 和输入/鼠标恢复是 Character Blueprint 的本地表现职责。
- 玩家 `Team.Player`，伤害只接受 `Team.Enemy`，服务器二次验证。

### 尚缺证据

阶段 4 新公式曾完成 M5/M6/M6Intent Headless 回归；`20260815_004559` 是胜利 UI 接口重构前的阶段 4 二进制证据。当前 `Presenter -> ReceiveCoopGameWon -> Character Blueprint` 的 C++ Game Target 通过，但 Editor Target、`On Coop Game Won -> Create winandquit`、`重新开始 -> Request Restart Coop Game` 仍需编译/接线验收。完整机关+GAS 可见双窗口点击、晚加入、Dedicated Server、补充弱网矩阵和 Network Insights 数据仍待完成。

---

## NET-10：帧同步和状态同步有什么区别？

### 30 秒回答

帧同步/Lockstep 主要同步每帧或每逻辑 Tick 的输入，各端依靠确定性模拟得到相同世界；带宽低但要求严格确定性，掉线/作弊验证和重连快照复杂。状态同步由服务器运行权威世界，复制 Actor 属性、事件或 Snapshot；客户端可以预测和插值。UE Actor Replication、CharacterMovement 和 GAS 主要属于服务器权威的状态同步体系，而不是确定性帧同步。

| 对比 | 帧同步/Lockstep | 状态同步/权威服务器 |
|---|---|---|
| 网络内容 | 输入/指令 | 状态、事件、Snapshot |
| 确定性要求 | 很高 | 服务器权威即可 |
| 延迟处理 | 常等待输入或预测整套模拟 | 本地预测+服务器校正 |
| 典型游戏 | RTS、部分格斗/回合 | FPS、动作、UE Actor 游戏 |
| 晚加入 | 需要完整快照/重演 | 复制当前持久状态 |

### 当前项目

这是状态同步：服务器维护机关、目标、属性和命中结论；客户端接收当前状态，同时对本地移动和 GAS 技能做有限预测。没有实现确定性帧同步。`PredictionKey` 是 GAS 预测事务的关联标识，不是同步帧号。

---

# 第三部分：20 题项目证据状态

| 主题 | 当前项目状态 | 面试口径 |
|---|---|---|
| ASC/AttributeSet/Ability/GE/Task/Tag | 已实现并通过编译/配置测试 | 可以结合源码完整解释 |
| PlayerState ASC/Mixed | 已实现，M3 重生重绑通过接受路径回归 | 可以解释跨 Pawn 所有权；断线边界待验 |
| TargetData/PredictionKey | 接受、Immunity 真 Reject 和 DamageIntent 语义拒绝都有日志/断言 | 可解释 DamageIntent Schema、ShotId guard、当前世界 Trace 与两类拒绝；loss/快速移动/历史回溯待补 |
| Cost/Cooldown | 已实现，Immunity 真 Reject 回滚通过 | 0ms/约 300ms RTT/一组 5% 样本有断言；精确 HUD 人工验收待补 |
| GameplayCue | 原生 Cue 接受/确认/生命周期及 Pending 拒绝收口通过 | 可解释去重与 Context 消费；瞬时 Cue 不可倒放，正式视听与多轮丢包待补 |
| ExecCalc/EffectContext | M4 战斗属性扩展完成，自动化与双端消费通过 | 可解释 Snapshot/Live Capture、Armor/Resistance/服务器暴击 Roll、Meta Attribute 与 Context 序列化；装备资产和性能数据待补 |
| Buff/Debuff Stacking | 三层 Vulnerability 核心、溢出配置与自然到期通过 | 可解释聚合/刷新/清理；活目标第 4 次溢出的双进程专项、层数 UI、晚加入待补 |
| ENetRole/RPC/属性复制 | 项目已有多处实现 | 可以结合日志和源码解释 |
| CharacterMovement 校正/插值 | 使用引擎默认实现 | 解释原理，不能说自行实现 |
| 延迟补偿/服务器回溯 | 未完成 | M7 计划，不得声称已有结果 |
| Co-op 权威同步 | C++ 权威核心已实现；部分 GAS Listen Server+1 Client 接受/拒绝路径有证据 | Session+机关+胜利整局可见双窗口、Host 反向输入和 Server+2 Clients 自动化待补 |
| Dedicated Server/Insights | 未完成 | 只能描述验收设计 |

---

# 第四部分：快速自测

如果不能脱离文档回答以下追问，说明还没有真正掌握：

1. 为什么 PredictionKey 不是安全令牌？
2. 为什么属性复制不受 Minimal GE 模式完全关闭？
3. 为什么 Instant GE 通常不形成长期 Active GE？
4. 为什么 GameplayCue 不应该修改 Health？
5. 为什么 `Team.Player` 既要影响本地选择，也必须在服务器重新检查？
6. 为什么 Listen Server Host 的成功不能证明远程 Client 路径正确？
7. 为什么 Multicast 不能保存门或胜利状态？
8. 为什么移动校正需要重放未确认 Move？
9. 为什么服务器回溯仍不能接受客户端提交的命中结果？
10. 为什么当前项目属于状态同步而不是帧同步？

---

## 最终表述边界

当前可以说：

> 在 UE5.5 双人合作项目中实现 PlayerState ASC、Mixed 复制、Tag 驱动输入、LocalPredicted 能力、Cost/Cooldown、战斗属性 ExecCalc、自定义 EffectContext、堆叠、死亡复活和 GameplayCue；ExecCalc 区分 Source Snapshot 与 Target Live Capture，暴击 Roll 和最终伤害只由服务器决定。Damage 链只上传 ShotId/Origin/方向/时间意图，由服务器防重放、校验并在当前世界重建命中。Immunity 真 Reject 与 DamageIntent 语义拒绝均有 0ms/约 300ms RTT 证据。

当前不能说：

> 已完成所有技能与所有 Cue 的完整回滚、完整丢包矩阵、服务器回溯延迟补偿、Dedicated Server、晚加入和 Network Insights 优化。

后一组内容仍在 M6～M9 路线上，必须有代码和运行证据后才能进入项目成果；当前只能准确声称 Immunity 激活 Reject 对可逆预测状态的回滚已经通过，不能外推为所有技能、瞬时 Cue 或完整弱网体系。
