# GAS M6：预测拒绝与回滚实验报告

> 后续推进：DamageIntent 的 Schema、ShotId 幂等、语义拒绝与服务器当前世界 Trace 已在 [M6 DamageIntent 安全验证报告](GAS_M6_Damage_Intent_Security_Test_Report.md) 继续实施和验收；本文保留 Immunity 激活入口 Reject 的历史结论。

> 分支：`coop-GAS`
> 日期：2026-08-13
> 当时结论：**M6 的“真实激活拒绝与预测状态回滚”子阶段通过；当时目标意图校验、ShotId 幂等、限流和服务器重建命中尚未完成。** 后续状态以 [M6 DamageIntent 安全验证报告](GAS_M6_Damage_Intent_Security_Test_Report.md) 为准。

## 1. 本报告证明什么

本轮使用状态免疫技能作为无 TargetData 的回滚探针，真实执行：

```text
Owning Client 预测激活 Immunity
-> 预测 Energy Cost -30
-> 预测 8s Cooldown
-> 预测 5s Immunity GE / State.Immune / GameplayCue
-> 预测 30s Pending 实验 GE / GameplayCue
-> Server 在激活入口拒绝
-> ClientActivateAbilityFailed
-> PredictionKey Rejected
-> Cost / Cooldown / Immunity / Pending 全部清理
-> 使用新 PredictionKey 再激活
-> Server 接受并 CatchUp
-> 权威 Cost / Cooldown / Immunity 保留，Pending 清理
```

本报告不把以下内容算作已完成：

- Damage TargetData 语义拒绝；它发生在服务器已经接受 Ability 之后，不等于 `ClientActivateAbilityFailed`。
- 瞬时 `ExecuteGameplayCue` 的倒放；瞬时声音/闪光已经发生后无法由 GAS 自动撤销。
- ShotId、防重放、请求限流、方向/时间/Origin 校验和服务器权威 HitResult。
- Dedicated Server、晚加入、服务器 + 两客户端功能自动化和 Network Insights。
- 单次 5% 丢包样本所不能代表的统计稳定性。

## 2. 为什么选择 Immunity，而不是 Damage

Damage 使用 TargetData AbilityTask。客户端上传 TargetData 时会创建依赖 PredictionKey：

```text
激活 Key A
-> TargetData 预测窗口创建依赖 Key D
-> Server 拒绝 A
-> 已排队 TargetData(D) 仍可能到达
```

这会混入两个额外变量：依赖 Key 的 Reject/CatchUp 时序，以及拒绝后 TargetData 缓存清理。它适合后续“目标语义拒绝和反滥用”实验，不适合用来证明最基础的 GAS 激活回滚。

Immunity 没有 TargetData，Cost、Cooldown、持续 GE 和 Cue 都绑定同一个激活 Key，因此能把问题收敛为：

```text
同一 PredictionKey
-> Server 接受，或
-> ClientActivateAbilityFailed + Rejected
```

这不是为了绕开复杂问题；而是先隔离并证明 GAS 原生回滚，再单独验证 Damage 请求协议。

## 3. 实现边界

### 3.1 非 Shipping 强制拒绝入口

`UmultiplayerAbilitySystemComponent::InternalServerTryActivateAbility` 在调用 `Super` 之前检查：

- 命令行包含 `-GASM6Lab`。
- 请求来自远端预测客户端。
- Ability AssetTag 是 `Ability.Immunity`。
- 服务器 ASC 持有非复制 LooseTag `Debug.Prediction.ForceReject.Immunity`。

命中后先消费一次性 Tag，再调用：

```cpp
ClientActivateAbilityFailed(AbilityToActivate, PredictionKey.Current);
```

该位置与 UE5.5 自带的 `AbilitySystem.DenyClientActivations` 测试入口处于同一层：服务器尚未建立接受用 PredictionWindow，因此不会把业务 TargetData 校验失败冒充成激活 Reject。

该入口只用于 Development/Editor 证据实验，不是正式玩法逻辑，也不是商业反作弊实现。

### 3.2 同 Actor Channel 的 Arm 与确认

Arm RPC 放在 PlayerState 持有的 ASC 上，与 GAS 激活 RPC 使用同一个复制 Actor Channel。服务器回传 `ClientConfirmImmunityPredictionRejectionArmed(TrialId, bArmed)`；自动序列收到确认后才发起技能，不依赖固定 RTT 猜测。

`TrialId` 关联以下事件：

- AuthorityArmed / ArmResult / ClientArmConfirmed。
- AuthorityRejected。
- ForcedRejectInput / RecoveryInput。
- PostReject / PostRecovery / SequenceComplete。

### 3.3 可逆状态和可见 Pending

实验 Pending 是仅预测客户端应用的 30 秒 Duration GE：

- AssetTag：`Effect.Debug.PredictionPending`。
- Cue：`GameplayCue.Coop.Prediction.Pending`。
- 30 秒只作为超时兜底，远长于单次实验；不能把自然到期误判为 Reject 清理。
- 洋红 Pending 状态优先显示于蓝色 Immunity 状态，使弱网等待窗口可见。

拒绝或接受回调都会幂等调用本地表现收口；快照同时断言：

- `PendingGECount=0`。
- `PendingCue=0`。
- `PendingVisual=0`。

玩法状态仍由 GE/Tag 决定，本地表现 bool 只负责 PointLight 技术占位表现。

### 3.4 事件驱动自动序列

M6 自动序列不再用固定 1～3 秒假设网络已经完成，而是轮询真实状态并设置 20 秒超时：

```text
Initial
-> ServerArm
-> 等 ClientArmConfirmed
-> ForcedRejectInput
-> 等 RejectedKey 且 Energy/CD/Immune/Pending 全部回滚
-> PostRejectCheckpoint
-> RecoveryInput
-> 等不同 CaughtUpKey 且权威状态收敛
-> PostRecoveryCheckpoint
-> SequenceComplete Result=Pass
```

任何超时都会输出 `Result=Fail`，不会因为计时步骤走完而假绿。

## 4. 自动核验器

`Scripts/VerifyGASM6Logs.ps1` 对已经执行的双进程日志做行为断言，而不是只统计关键词。增强后的核验器每组正式运行包含 95 项检查：

- RunInfo 为 M6、Host Ready、Client Joined。
- `PktLag/PktLoss` 在两端日志中真实生效。
- 无 Fatal、Critical Error 或 Ensure。
- Arm、拒绝、恢复激活次数精确。
- Host Reject、Client `ClientActivateAbilityFailed`、Rejected delegate 的 Spec/Key 一致。
- 被拒绝 Key 没有 Authority Commit，也不记为正常 CatchUp。
- Recovery 使用不同 Key，并在 Host Commit、Client CatchUp 中一致。
- PostReject 和 PostRecovery 的属性、GE、Tag、Cue、视觉状态逐字段匹配。
- 客户端与服务器事件顺序正确。

任何断言失败时脚本 `exit 1`；通过时生成 `M6Summary.json` 和 `M6Summary.md`。

它仍是 Listen Server + 1 Client 的双进程实验核验器，不应称为服务器 + 两客户端功能自动化框架。

## 5. 正式运行结果

| RunId | 每方向延迟 | 近似 RTT | 每方向丢包 | 断言 | 结果 |
|---|---:|---:|---:|---:|---|
| `20260813_144121` | 0ms | 0ms | 0% | 95/95 | 通过 |
| `20260813_144324` | 150ms | 约 300ms | 0% | 95/95 | 通过 |
| `20260813_144547` | 150ms | 约 300ms | 5% | 95/95 | 通过（单次样本） |

两端都传 `-PktLag=150` 时是每方向约 150ms、RTT 约 300ms；报告没有把它写成“150ms RTT”。

### 5.1 拒绝前的预测状态

三组运行均记录：

```text
PredictionKey=1
EnergyBase=100
EnergyCurrent=70
CostGECount=1
CooldownGECount=1
CooldownTagCount=1
PersistentGECount=1
ImmuneCount=1
PendingGECount=1
PendingCueCount=1
```

这证明客户端确实先预测了资源、冷却、持续状态与 Cue，不是服务器拒绝一个尚未产生副作用的空 Ability。

### 5.2 拒绝后的稳定状态

三组 PostRejectCheckpoint 均为：

```text
RejectedKey=1
CaughtUpKey=0
EnergyBase=100
EnergyCurrent=100
CostGECount=0
CooldownGECount=0
ImmunityCooldown=0
PersistentGECount=0
ImmuneCount=0
PendingGECount=0
PendingCue=0
PendingVisual=0
```

Host 对 Key 1 只输出 `AuthorityRejected`，没有 `AuthorityCommitted`；客户端同时出现引擎 `ClientActivateAbilityFailed_Implementation` 和项目 Rejected delegate。

### 5.3 Recovery 接受后的稳定状态

三组 PostRecoveryCheckpoint 均为：

```text
RejectedKey=1
CaughtUpKey=2
EnergyBase=70
EnergyCurrent=70
CostGECount=0
CooldownGECount=1
ImmunityCooldown=1
PersistentGECount=1
ImmuneCount=1
PendingGECount=0
PendingCue=0
PendingVisual=0
```

Host 对 Key 2 输出一次 `AuthorityCommitted`；Client 对 Key 2 输出一次正常 `CaughtUp`。这证明一次性拒绝 Tag 已消费，并且 Reject 没有污染下一次合法激活。

## 6. 真实问题复盘

### M6-PRED-001：被拒绝 Key 后续仍收到 CatchUp 通知

#### 1. 现象

第一次 0ms 运行中，Key 1 已出现 `ClientActivateAbilityFailed` 和 Rejected，状态也全部回滚，但稍后同一 Key 又触发项目绑定的 CaughtUp delegate。若只按回调名字统计，会把一个事务同时写成 Rejected 和 Accepted。

#### 2. 复现条件

1. 远端 Client 预测激活 Immunity。
2. Server 在 `InternalServerTryActivateAbility` 入口拒绝 Key 1。
3. Client 收到 `ClientActivateAbilityFailed`。
4. 继续观察 replicated prediction key map 的后续推进。

#### 3. 为什么难

`BroadcastRejectedDelegate` 会广播拒绝回调，但不会像内部 `Reject()` 那样立即删除整个 Key 的 delegate map；随后复制 Key Map 推进仍可能触发 CaughtUp bookkeeping。回调名称不是业务结果本身，必须结合已记录 Outcome 和最终状态解释。

#### 4. 初始假设

| 假设 | 验证 | 结果 |
|---|---|---|
| 自定义拒绝入口错误地建立了服务器接受窗口 | 拒绝发生在 `Super::InternalServerTryActivateAbility` 前；Host 无 Key 1 AuthorityCommitted | 排除 |
| Recovery 错用了 Key 1 | 第二次预测、Host Commit 和正常 CatchUp 都是 Key 2 | 排除 |
| UE 的 Rejected 广播后仍可能收到 Key Map CatchUp | 阅读 UE5.5 `GameplayPrediction.cpp` 和实际日志 | 确认 |

#### 5. 最终解决

ASC 保存 `LastPredictionLabRejectedKey`。CaughtUp 回调若收到同一 Key：

- 输出 `RejectedKeyCatchUpIgnored`。
- 记录为 `PostRejectCatchUp`，而不是正常 `CaughtUp`。
- 不写入 `LastPredictionLabCaughtUpKey`。
- 再次断言回滚状态仍为 100/0/0/0。

#### 6. 可复用经验

PredictionKey 的网络追平通知不等于业务接受；最终 Outcome 必须由明确的 Reject/Accept 状态机和权威副作用共同决定。

### M6-CUE-001：接受路径 Pending Tag 已清，但占位灯可能滞留

#### 1. 现象

0ms 日志显示两次 Pending OnActive/WhileActive，但只有 Reject 路径出现一次 Removed。Recovery 接受后 `PendingGECount=0`、`PendingCue=0`，却没有第二个 native Removed Handler；仅凭 Tag 已清不能证明本地表现 bool 已清。

#### 2. 根因

预测 Pending GE 在 CatchUp 时完成内部 reconcile，但当前原生占位灯使用项目 bool 持有；不能假设每种预测 reconcile 时序都会再次产生项目期待的 Removed Handler。

#### 3. 候选方案

| 方案 | 取舍 |
|---|---|
| 只依赖 Cue Removed | 代码少，但本次运行已证明接受路径不稳定触发该回调 |
| Tick 中读取 Tag | 能纠正，但增加永久轮询 |
| 在明确 Rejected/Accepted 回调幂等收口本地表现 | 与事务结果同点发生，不改变玩法状态；采用 |

#### 4. 最终解决与验证

`ReconcilePredictionLabPendingPresentation` 在 Reject 与合法 CatchUp 时清除本地 Pending 表现。最终 0ms、约 300ms RTT、约 300ms RTT + 5% 样本均通过 `PendingVisual=0` 硬断言。

#### 5. 可复用经验

玩法 GE/Tag 与本地表现缓存是两条链；Gameplay 状态正确不自动等于项目自定义视觉状态已经收口。

## 7. 验证分层

| 验证层级 | 结果 |
|---|---|
| Editor Development 编译 | 通过 |
| Game Development 编译 | 最终视觉收口后重新构建通过 |
| `multiplayer.GAS.Configuration` | 1/1 通过 |
| 0ms Listen Server + Client | 95/95 通过 |
| 每方向 150ms、0% loss | 95/95 通过 |
| 每方向 150ms、5% loss | 95/95 通过，只有一次样本 |
| 非 Headless 视觉录屏 | 待验证 |
| 300ms 每方向 / 约 600ms RTT | 待验证 |
| 多次 5% loss 统计 | 待验证 |
| Dedicated + 两客户端 | 待实现 |

## 8. 当前可以和不可以怎么说

可以说：

> 在 UE5.5 Listen Server + 1 Client 下实现了非 Shipping 的真实 `ClientActivateAbilityFailed` 实验，并用 95 项日志断言验证预测 Energy Cost、Cooldown、持续 Immunity GE/Cue 和 Pending 表现，在 0ms、约 300ms RTT 与一组 5% 丢包样本中完成拒绝事务清理；下一次合法激活使用新 PredictionKey 并与服务器权威状态一致。

不可以说：

- “所有 GameplayCue 都能回滚。”瞬时 Execute Cue 不能倒放。
- “TargetData 非法就是激活 Reject。”两者属于不同阶段。
- “已完成商业级反作弊。”后续 DamageIntent 已补充 Schema、ShotId 幂等和服务器当前世界 Trace，但完整限流、攻击遥测与全部负面矩阵仍未完成。
- “已通过完整丢包矩阵。”目前只有一组 5% 样本。
- “M6 全部完成。”DamageIntent 核心守卫已在后续报告通过两组测试；token bucket、更多非法请求分支、丢包/Dedicated Server/晚加入矩阵仍是后续边界。

## 9. 下一步

后续 DamageIntent 已完成自定义 TargetData、ShotId 幂等、Origin/方向/时间校验、服务器当前世界 Trace 与权威 HitResult；详细实现和证据见链接报告。尚需继续：

1. 为 `TargetDataTimeout`、`SourceDead`、`InvalidTarget`、`CommitFailed` 等分支补充专项双进程端到端测试。
2. 将当前最小请求间隔升级为宽松 token bucket，并补充攻击遥测与洪泛压力测试。
3. 扩展丢包、Dedicated Server、晚加入、Travel/断线和快速移动目标矩阵。
4. 保持“激活入口 Reject”和“激活后 TargetData 语义拒绝”的证据与措辞分离。
5. 完成 M6 遗留边界后再进入 M7 有限 Server-Side Rewind。
