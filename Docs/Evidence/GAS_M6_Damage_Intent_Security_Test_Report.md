# GAS M6：Damage Intent 信任边界与安全验证报告

> 分支：`coop-GAS`
> 基线提交：`dc3969f`，叠加本地未提交工作树；因此单独引用提交号不能还原本报告所测源码，文末另列源码与二进制 SHA256。
> 日期：2026-08-13
> 正式运行：`20260813_163052`（双向 `PktLag=0`）与 `20260813_163248`（Host、Client 出站各 `PktLag=150ms`，配置上约 300ms RTT）
> 当前结论：**本报告范围内的 M6 Damage Intent 最小协议、服务器当前世界重建命中、语义拒绝矩阵和拒绝后资源收敛已经通过；这不是完整反作弊、回溯、弱网或性能验收。**
> 2026-08-16 去重更新：服务器场景查询现由 TargetTask 唯一拥有；Ability 已删除第二次距离/LOS Trace，只在 Commit 前复验目标、ASC、敌我和存活不变量。Run `20260816_110557` 对修改后的链路完成 0ms Listen Server + 1 Client 回归，`52/52 PASS`。原 2026-08-13 运行数据保留为历史证据。

## 1. 一句话安全模型

客户端只提交“我在某个估算的服务器时刻，从某个视点沿某个方向发起了编号为 `ShotId` 的射击”这一意图；服务器不接收客户端目标 Actor、`HitResult`、伤害值、暴击结果或 GameplayEffect，而是在自己的当前世界中校验、去重、限速并重新 Sweep，只有服务器生成的 `HitResult` 才能进入伤害 `EffectContext`。

这里的“只提交”专指自定义 Damage Intent payload。GAS 的外层 RPC 仍携带 Ability Spec Handle、Activation PredictionKey、ApplicationTag 和当前 Scoped PredictionKey 等协议字段；不能把“payload 只有四项”误写成“整条 RPC 在网络上只有四个字段”。

## 2. 目标、通过项与明确不证明项

### 2.1 本报告要回答的问题

1. 客户端上传的数据结构是否真的只有 `ShotId / Origin / Direction / ClientFireServerTimeSeconds`。
2. 服务器是否拒绝错误 Schema、重复编号、伪造 Origin、反向 Direction、过旧/未来时间，并把 Miss 当作服务器语义结果。
3. 合法请求是否由服务器重新 Trace、重新选 Actor、重新生成 `HitResult`，随后才 Commit 和应用伤害。
4. 非法请求是否不产生服务器 Commit/伤害，客户端最终 Energy/Cooldown 是否收敛。
5. 0ms 与每方向 150ms 模拟延迟下，完整 Client → GAS TargetData RPC → Server → ClientResult 链是否真实跑通。
6. `ShotId`、GAS `PredictionKey`、激活入口拒绝与激活后的 TargetData 语义拒绝是否被清楚分开。

### 2.2 已有证据支持的结论

- 自定义 `FGameplayAbilityTargetData` 可多态 NetSerialize，落地后仍保持精确 ScriptStruct 类型。
- payload 中仅有非零 `ShotId`、量化 Origin、量化单位 Direction 和客户端估算的服务器时间；没有 Actor、`HitResult`、伤害数值、暴击或 GE。
- 当前服务器顺序是：精确 Schema → 消费业务 ShotId → Source 状态 → 字段约束 → 当前世界 Sweep → 敌我/存活/ASC 校验 → Ability 提交前轻量不变量复验 → 创建有效 Damage Spec → Commit → 服务器 HitResult 写入 Context → Damage Exec。
- 两次正式双进程运行都只提交了 Shot 1 和 Shot 7；Shot 1 重放以及 Shot 2～6 的错误语义均没有服务器 Commit 或伤害。
- 两次正式运行的专用核验器均为 `52/52 PASS`，并且 Host/Client 均无 Fatal、Critical、Ensure 或 Assertion。
- 0ms 样本的两个合法服务器 Trace 年龄为约 `0.048s / 0.048s`；150ms/方向样本为约 `0.358s / 0.401s`。这些是日志中的单次样本，不是延迟分布。

### 2.3 本报告明确不证明

- 不证明客户端四个字段是真实输入。恶意 owning client 仍可在允许窗口内选择 Origin、Direction 和时间；服务器只是缩小信任面并在当前世界重新裁决。
- 不证明密码学防伪、内核级反作弊、连接级 DoS 防护或篡改客户端检测。
- 不证明 M7 Server-Side Rewind；当前只查询服务器收到请求时的世界，没有历史姿态回放。
- 不证明 5% 丢包、抖动、乱序、突发包、断线重连、Late Join、Dedicated Server 或服务器 + 2 个远端客户端。本报告正式矩阵只有 Listen Server + 1 Client、0% 丢包。
- 不证明 `RateLimited`、`StaleSequence`、`InvalidSchema`、`InvalidShotId`、`SourceDead`、`InvalidTarget`、`CommitFailed`、`TargetDataTimeout` 的双进程端到端矩阵；其中一部分只有单元测试或代码审查证据。`TargetDataTimeout` 已实现为 5 秒服务端等待上限，但两组正式 run 都正常收到 TargetData，没有触发该专项分支。
- 不证明所有拒绝都没有任何瞬态预测闪烁。自动化检查的是收到结果后的 Energy/Cooldown 收敛点，没有逐帧录像或帧级状态采样。
- 不证明射击公平性、命中容差合理性或性能预算；Collision、Navigation 人工观察与 Unreal Insights 本轮没有作为正式通过证据。
- 不证明 Shipping/Test 包含测试变异入口。`-GASM6IntentLab` 变异器和自动序列是非 Shipping/Test 实验夹具。

## 3. 协议与信任边界

### 3.1 客户端可发送的四个业务字段

定义位于 `Source/multiplayer/AbilitySystem/multiplayerGameplayAbilityTargetData.h`：

| 字段 | 网络形式 | 服务器用途 | 不允许代表什么 |
|---|---|---|---|
| `ShotId` | `uint32`，packed int | 业务幂等、高水位、速率限制 | 不是 PredictionKey，也不是安全随机数 |
| `Origin` | `FVector_NetQuantize10` | 与服务器眼点做距离容差校验 | 不是服务器最终 Trace 起点 |
| `Direction` | `FVector_NetQuantizeNormal` | 单位长度、AimDot 校验；通过后用于服务器 Trace 方向 | 不是命中 Actor 或 ImpactPoint |
| `ClientFireServerTimeSeconds` | `float` | 过旧/未来窗口过滤；客户端优先取 `GameState->GetServerWorldTimeSeconds()` | 不是可信时间戳，也没有驱动当前阶段的 rewind |

`NetSerialize` 只序列化上述四项。Automation 还显式断言反序列化后的对象 `HasHitResult()==false` 且 `GetActors().IsEmpty()`。

### 3.2 服务器当前参数

| 约束 | 当前值 | 含义 |
|---|---:|---|
| Origin 最大误差 | 150 cm | 客户端视点离服务器眼点更远则 `InvalidOrigin` |
| 最小 AimDot | `0.819152` | 约 35° 的宽容方向锥；运行默认值，不要与单元测试内覆盖的 25° 配置混淆 |
| Direction 单位容差 | `0.05` | 对 `SizeSquared()` 与 1.0 的容差 |
| 最大过去年龄 | 2.0 s | 更旧为 `InvalidTime` |
| 最大未来领先 | 0.25 s | 更未来为 `InvalidTime` |
| 最大 ShotId 前跳 | 64 | 过大前跳或旧序列为 `StaleSequence` |
| 最小意图间隔 | 0.05 s | 新编号过快为 `RateLimited`，且该编号仍被消费 |
| 服务器 Trace | 600 cm、半径 35 cm、`ECC_Visibility` Sweep | 在服务器当前世界中查询 |
| Commit 前不变量 | Target、TargetASC、敌我和 Health | Ability 不再发起第二次场景查询，只防止 Trace 与 Commit 间对象状态变化 |

这些参数是当前实验基线，不是经过玩家命中率、误拒绝率或生产安全数据调优后的最终值。

### 3.3 PredictionKey 不等于 ShotId

二者服务不同问题：

- `PredictionKey` 属于 GAS 预测事务，关联一次 Ability 激活、预测 GE/Cue 的确认或回滚。
- `ShotId` 属于 Damage 业务协议，跨激活识别“是否已经处理过这一发”，由客户端 ASC 单调分配、服务器 ASC 保存高水位。

0ms 正式日志给出了最直接的反例：第一次 `ShotId=1` 的客户端本地 PredictionKey 为 `[2/1]`；重放同一个 `ShotId=1` 时 PredictionKey 已变为 `[4/3]`。服务器对应的激活 Key 又分别为 `[1/0]` 和 `[3/0]`。因此：

- 新 PredictionKey 不能让旧 ShotId 再结算；重放仍是 `Duplicate`。
- 同一个 ShotId 可以出现在不同 GAS 预测事务中。
- 不能拿 PredictionKey 代替持久业务幂等键，也不能拿 ShotId 驱动 GAS 回滚。

## 4. 完整调用链

### 4.1 客户端生成意图与本地预测

1. 输入进入 `UmultiplayerAbilitySystemComponent::AbilityInputTagPressed`，找到带 Damage InputTag 的 Ability Spec 并 `TryActivateAbility`。
2. `UmultiplayerDamageAbility::ActivateAbility` 创建 `UmultiplayerAbilityTask_TargetActor`。
3. Task 在 locally controlled 端执行 `SendLocalTargetData`。
4. 客户端用相机 Sweep 寻找敌对目标，但这个 `LocalTargetDataHandle/HitResult` 只给本地预测表现使用。
5. ASC 分配非零 `ShotId`；Task 从 Avatar 眼点、控制器朝向/本地命中点和同步服务器时间估计构造四字段 Damage Intent。
6. Task 通过 `CallServerSetReplicatedTargetData(SpecHandle, ActivationPredictionKey, IntentHandle, ApplicationTag, ScopedPredictionKey)` 发送自定义 payload。
7. 客户端随后广播本地 `SingleTargetHit`，进入 `HandleTargetData` 做预测 Commit/Cue；正常发送和广播后 Task 立即 `EndTask`，这一步不能授权服务器伤害。
8. 若本地 Avatar 或项目 ASC 初始化失败，Task 不再静默退出，而是在允许广播 delegate 时先广播空 TargetData 让 Ability 走正常取消路径，再 `EndTask`；这是代码审查收口，不是本轮正式运行复现过的玩家故障。

### 4.2 服务器接收、消费、验证与重建

1. 服务器 Ability Task 使用 `(SpecHandle, ActivationPredictionKey)` 注册 `AbilityTargetDataSetDelegate`；若数据尚未到达，进入等待并启动 5 秒一次性定时器。
2. `OnTargetDataReplicated` 先清除定时器，再进入 `ProcessAuthorityIntent` 并 `ConsumeClientReplicatedTargetData`，避免缓存重复留存。
3. `ValidateMultiplayerDamageIntentSchema` 要求：Handle 恰好一个条目、非空、ScriptStruct 精确等于 Damage Intent、`ShotId != 0`；非空 ActivationTag 也按 `InvalidSchema` 处理。
4. Schema 合法后进入 `ResolveAuthorityIntent`。服务器先取自己的眼点和 `World->GetTimeSeconds()`。
5. **先消费 ShotId，再进入 Source/字段/场景语义。** 这样一个合法 Schema 的新请求即便随后因死亡、伪造字段或 Miss 被拒绝，也不能用同一个 ID 改参数后再问一次服务器。
6. 服务器检查 Source 未死亡、Health 大于 0。
7. 服务器比较 Origin、Direction、客户端时间与自己的眼点、朝向、当前时间。
8. 服务器从自己的眼点出发，沿校验后的方向做 600cm、35cm 半径、Visibility Sweep。客户端 Origin 不作为最终起点。
9. 命中后检查 Candidate 非自身、未销毁、有 ASC、敌对且 Health 大于 0。TeamId/Team 接口是目标裁决来源，Team Tag 仅保留为 GAS 镜像。
10. 服务器把自己的 `FHitResult` 包装成新的 `FGameplayAbilityTargetData_SingleTargetHit`；绝不把客户端本地 `SingleTargetHit` 转发到权威链。无论接受还是语义拒绝，广播 resolved/空 handle 后都 `EndTask`。

### 4.3 Ability 结算与结果回传

1. `UmultiplayerDamageAbility::HandleTargetData` 只在服务器 resolved handle 非空时继续。
2. `IsResolvedTargetStillValid` 只复验 Actor、队伍、存活和 ASC；距离、方向、遮挡和服务器 HitResult 均由 TargetTask 的唯一权威 Sweep 决定，不再做第二次 Scene Query。
3. 目标有效后，服务器复用上述 `TargetASC` 并在 Commit 前创建有效的 Damage Spec；TargetASC 缺失返回 `InvalidTarget`，Spec 无效返回 `CommitFailed`，两者都立即结束，避免先消费 Cost/Cooldown 后才发现无法落伤害。
4. 只有上述对象齐备才 `CommitAbility`；Commit 本身失败返回 `CommitFailed`。服务器不会为前面的语义拒绝或不可结算终点消耗权威 Cost/Cooldown。
5. 服务器把**服务器 HitResult** 写入已验证的 EffectContext，设置 SetByCaller Damage，再 Apply 到服务器选中的 Target ASC。
6. Damage Execution、AttributeSet 和 GameplayCue 读取服务器 Context；服务器记录 `Committed` 并通过 Reliable Client RPC 返回 `Accepted`。
7. 任何前置语义失败由 Task 记录 `AuthorityRejected`、发送精确 `ClientDamageIntentResult(ShotId, Result)`，并向 Ability 广播空 resolved handle；Ability 随后取消结束。当前实现为正常项目 ASC 下的每个服务器业务终点给出明确 Result：Task 语义拒绝返回对应原因，目标/ASC 失效返回 `InvalidTarget`，Spec/Commit 失败返回 `CommitFailed`，成功返回 `Accepted`。

### 4.4 一次性 TargetTask 生命周期收口

- 服务端远端 TargetData 等待上限为 5 秒；超时会消费该 `(SpecHandle, PredictionKey)` 的复制缓存、以 `ShotId=0` 返回 `TargetDataTimeout`、广播空 handle 并 `EndTask`。
- `OnDestroy` 作为统一收口：ASC/World 仍有效时移除本 Task 在对应 Spec/PredictionKey 上的 TargetData delegate，并清除 timeout timer，避免 Ability 提前结束后残留回调或悬空计时器。
- 客户端初始化失败在允许 delegate 广播时先广播空 handle，再 `EndTask`；正常发送/广播、服务端正常处理、语义拒绝和服务端超时也都显式 `EndTask`。
- 以上生命周期路径已通过源码审查且已进入本轮编译产物；两组正式 run 覆盖正常处理及已有语义拒绝路径，但日志中没有 `TargetDataTimeout`，所以“超时专项分支行为正确”仍是待验证项，不能由 52/52 推导。

可压缩为：

```text
Client input
  -> predicted Damage Ability
  -> local crosshair HitResult (presentation only)
  -> DamageIntent{ShotId, Origin, Direction, EstimatedServerTime}
  -> GAS replicated TargetData RPC
Server TargetTask
  -> exact schema
  -> consume ShotId / rate gate
  -> source + origin + direction + time
  -> current-world authority Sweep
  -> hostile/alive/ASC validation
  -> server-generated SingleTargetHit
  -> explicit EndTask (or 5s TargetDataTimeout)
Server Damage Ability
  -> second target validation
  -> TargetASC + DamageSpec preflight
  -> Commit
  -> server HitResult into EffectContext
  -> Damage Exec / AttributeSet
  -> ClientDamageIntentResult
```

## 5. 语义拒绝不是 ClientActivateAbilityFailed

这是本报告最容易被误述的边界。

### 5.1 激活入口拒绝

`ClientActivateAbilityFailed` 表示服务器在 Ability 激活入口拒绝客户端预测激活。已有 M6 Immunity Reject Lab 专门验证了这条链：服务器在 `InternalServerTryActivateAbility` 中拒绝，GAS 根据 PredictionKey 回滚预测 Cost/Cooldown/持续 GE/Cue。

### 5.2 Damage Intent 语义拒绝

本报告的 Duplicate、InvalidOrigin、InvalidDirection、InvalidTime、Miss 发生在 Damage Ability 已经激活、服务器等待/收到 TargetData 之后。它们是**激活后的业务结果拒绝**：

- 结果通道是 `ClientDamageIntentResult(ShotId, Result)`。
- 服务端 TargetTask 广播空 resolved TargetData，Ability 取消结束。
- 这不是一次新的 `ClientActivateAbilityFailed`，也不应伪造该回调来表达业务结果。
- 两组正式 M6Intent 日志中 `ClientActivateAbilityFailed` 和 `GAS_M6_REJECT` 均为 0 次；这正符合设计，不是漏测报错。

客户端检查点显示第一次合法请求后 Energy=90；后续六个语义拒绝均保持 Energy=90、Damage Cooldown=0；恢复请求后 Energy=80、Cooldown=1。它证明的是**结果到达后的最终收敛和没有额外服务器结算**，不等于逐帧证明“客户端从未短暂预测过 Cost/Cooldown/表现”。

## 6. ShotId 消费顺序与速率限制

### 6.1 当前顺序

精确 Schema 和非零 ID 是进入 Guard 的前提；随后 Guard 在 SourceDead、字段验证、Trace 之前执行。结果如下：

| 情况 | 是否消费/推进高水位 | 理由 |
|---|---|---|
| 空/错误/多条 Schema、ShotId=0、非法 ActivationTag | 否 | 还没有可归属的合法业务 ID |
| 同一 ID | 否，返回 `Duplicate` | 已经消费过 |
| 旧 ID 或前跳超过 64 | 否，返回 `StaleSequence` | 不允许攻击者推进高水位 |
| 新 ID 但小于 50ms | **是**，返回 `RateLimited` | 禁止等窗口结束后用同一 ID 重试采样 |
| 新 ID，随后 SourceDead/字段非法/Miss/InvalidTarget | **是** | 禁止“改一个字段后同 ID 再问”形成语义/场景查询 oracle |
| 新 ID且合法命中 | 是 | 正常结算 |

### 6.2 为什么这是有意取舍

先做语义验证再消费 ID，表面上能让误填请求“修正后重试”，但也允许攻击者固定一个 ID 反复修改 Origin/Direction/Time 探测服务器边界。当前选择是 fail-closed：一个尝试就是一个 ID；失败后只能申请新 ID。

代价是错误字段也会烧号，而且 Guard 返回顺序可能优先给出 `RateLimited`，而不是更具体的 InvalidOrigin/Time。这是安全与诊断之间的取舍，不是协议错误。客户端必须把每次尝试视为不可变事件，不能在失败后复用 ID。

### 6.3 当前限流不是最终生产方案

当前是每个服务器 ASC 一条高水位 + 50ms 最小间隔，不是 token bucket：

- 优点：状态小、行为确定、单元测试容易、重复/过快编号都不可重试。
- 局限：没有 burst credit；20Hz 以上合法武器会被误拒；只能在完整 GAS 激活和 TargetData 反序列化之后挡住请求；不能替代连接层限速。
- M6 剩余项应按武器/Ability 定义 fire cadence 或 token bucket，并把 accepted/rejected counters 暴露给 Insights。

## 7. 方案取舍

| 决策 | 采用方案 | 收益 | 成本/边界 |
|---|---|---|---|
| TargetData | 自定义最小 Intent，而非客户端 Actor/HitResult | 大幅降低客户端权威面；可精确 Schema 校验 | 需要服务端重新查询与结果协议 |
| 网络通道 | 复用 GAS TargetData RPC 与 Ability Spec/PredictionKey 关联 | 复用所有权、预测窗口和 AbilityTask 生命周期 | 业务拒绝不能混同激活失败 |
| 命中裁决 | 服务器当前世界 Sweep | 实现简单、服务器权威、无需历史缓存 | 高延迟玩家没有 rewind；M7 才解决公平性 |
| Trace 形状 | 35cm 球形 Sweep 而非零宽 LineTrace | 对第三人称瞄准和量化误差更宽容 | 命中体积变大，需用命中率数据调优 |
| ID 防重放 | 单调 ShotId + 最大前跳 + 先消费后语义 | 防重复结算与同 ID 查询 oracle | 会烧掉失败 ID；会话/重连边界需补测 |
| 速率 | 50ms 硬间隔 | 简洁且 fail-closed | 不支持 burst；不是生产级 DoS 防护 |
| 结果 | Reliable `ClientDamageIntentResult` | 明确、可观测、便于自动断言 | 高频拒绝可能增加 Reliable backlog，需 M8 测量 |
| Commit 时机 | 目标、TargetASC、Damage Spec 预检后才 Commit | 拒绝和不可结算终点不消费权威资源；每个服务端业务终点有明确 Result | 客户端预测资源仍需等待网络收敛 |
| 目标校验 | TargetTask 独占 Schema、字段与权威场景查询；Ability 只复验易变对象不变量 | 每个请求只有一套几何策略，避免不同起点/形状导致规则漂移 | Commit 前不再重新判断几何；依赖一次性 Task 立即交付 resolved target |
| Task 生命周期 | 5 秒远端数据上限 + 所有终点 EndTask + OnDestroy 清理 | 防止永远等待、残留 delegate/timer 和活跃 Ability 泄漏 | 超时阈值需按网络分布调优；超时专项分支尚待运行验证 |

## 8. 验证分层与正式结果

### 8.1 分层结论：每个工具回答什么问题

| 层 | 工具 | 回答的问题 | 本轮状态 |
|---|---|---|---|
| L0 静态审查 | `rg`、源码逐行审查、Git diff | 信任边界和调用顺序在代码里是否成立 | 已审查 |
| L1 编译 | UnrealBuildTool `Build.bat` | 当前源码能否生成 Editor 模块与 Development Game，UHT/链接是否通过 | 两个 Target 均 Exit 0 |
| L2 纯逻辑 | Unreal Automation | 序列化、Schema、字段边界、Guard 状态机是否符合预期 | 1/1 成功，Exit 0 |
| L3 双进程 0ms | Listen Server + Client 自动序列 | 网络 RPC、服务器 Trace、语义拒绝、伤害与资源收敛是否贯通 | `20260813_163052`，52/52 PASS |
| L4 双进程模拟延迟 | 两端各 `PktLag=150` | 相同链在配置约 300ms RTT 下能否贯通 | `20260813_163248`，52/52 PASS |
| L5 人工视觉/Collision | 非 Headless Editor | Trace 体积、遮挡和表现是否符合设计直觉 | 未作为正式证据执行 |
| L6 Dedicated/多客户端/丢包 | Dedicated + 2 Clients、loss/jitter matrix | 更广网络拓扑与可靠性 | 未执行 |
| L7 性能 | Unreal Insights | CPU、网络字节、p95/p99、单次 Sweep 与结果 RPC 成本 | M8，未执行 |

### 8.2 Build

实际执行：

```powershell
& 'E:\program\ue554\UE_5.5\Engine\Build\BatchFiles\Build.bat' `
  multiplayerEditor Win64 Development `
  'E:\ueprojrct\multiplayer\multiplayer.uproject' `
  -WaitMutex -NoHotReloadFromIDE
```

结果：进程 Exit Code 0；当前 `Binaries/Win64/UnrealEditor-multiplayer.dll` 本地时间为 `2026-08-13 16:29:20`，晚于最后一处本报告相关 C++ 修改 `16:28:55`，也早于两组正式 run。Build 回答“当前源码是否能通过 UHT、编译与链接”，不回答网络行为或安全语义是否正确。

同一份源码还执行了 Game Target：

```powershell
& 'E:\program\ue554\UE_5.5\Engine\Build\BatchFiles\Build.bat' `
  multiplayer Win64 Development `
  'E:\ueprojrct\multiplayer\multiplayer.uproject' `
  -WaitMutex -NoHotReloadFromIDE
```

结果同样为 Exit Code 0，并生成本地时间为 `2026-08-13 16:34:37` 的 `Binaries/Win64/multiplayer.exe`。这关闭了“只在 Editor Target 编译”的缺口；它仍不等于打包版、Shipping 或 Dedicated Server 已验收。两组正式双进程 run 使用 UnrealEditor/Editor DLL，Game EXE 在 run 之后生成，不能把其哈希误写成那两组网络运行的二进制身份。

### 8.3 Automation

实际执行：

```powershell
& 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' `
  'E:\ueprojrct\multiplayer\multiplayer.uproject' `
  -unattended -nop4 -nosplash -NullRHI `
  -DDC=InstalledNoZenLocalFallback `
  '-ExecCmds=Automation RunTests multiplayer.GAS.DamageIntent.Unit;Quit' `
  '-TestExit=Automation Test Queue Empty' `
  -log
```

`Saved/Logs/multiplayer.log` 记录 Found 1、`Test Completed. Result={成功}`、`**** TEST COMPLETE. EXIT CODE: 0 ****`。该测试覆盖：

- 多态 Handle 的写入/读取和精确 ScriptStruct。
- 四个字段的量化/序列化，以及无 Actor/HitResult。
- 空、错误类型、两条 Intent、ShotId 0 Schema。
- Origin 150cm 边界、反向/零 Direction、过去 2.0s 和未来 0.25s 边界。
- 首个 ID、Duplicate、50ms 限速、被限速 ID 仍被消费、恢复、旧号、过大前跳、被拒前跳不推进、NaN server time、Reset。

它不创建真实网络连接，不证明服务端场景命中，也不证明当前默认 35° AimDot 的精确边界；字段测试中测试夹具将 MinAimDot 改为 25°。

### 8.4 双进程启动命令

0ms 正式运行：

```powershell
& '.\Scripts\StartGASM5TwoPlayers.ps1' `
  -Stage M6Intent -PktLagMs 0 -PktLossPercent 0 `
  -AutoSequence -Headless -Port 17784
```

每方向 150ms 正式运行：

```powershell
& '.\Scripts\StartGASM5TwoPlayers.ps1' `
  -Stage M6Intent -PktLagMs 150 -PktLossPercent 0 `
  -AutoSequence -Headless -Port 17785
```

脚本具体做了四件事：启动带 `?listen` 的 UnrealEditor Game 进程；等 Host 日志出现监听端口；启动连接 `127.0.0.1:<Port>` 的 Client；等 Host 出现 `Join succeeded`。Host 与 Client 都带 `-GASM6IntentLab`，Client 额外带 `-GASM6IntentAuto`，并分别落 `RunInfo.txt / Host.log / Client.log`。

`PktLag=150` 是 Host 和 Client 各自的**出站**模拟延迟；“约 300ms RTT”来自两方向配置相加，不是本轮测得的真实 RTT P50/P95。150ms run 的 Host/Client 原始日志均明确记录 `PktLag set to 150`，丢包为 0。

### 8.5 专用 Verifier 命令

实际执行：

```powershell
& '.\Scripts\VerifyGASM6IntentLogs.ps1' -RunId 20260813_163052
& '.\Scripts\VerifyGASM6IntentLogs.ps1' -RunId 20260813_163248
```

Verifier 是行为核验器，不是关键词数量展示器。它读取三份落盘证据，生成 `M6IntentSummary.json/.md`，任一断言失败即 `exit 1`。52 项覆盖：文件/Stage/连接状态、严重错误扫描、唯一 Pass/零 Fail、精确的 8 个 ClientResult、只允许 Shot 1/7 Commit、精确的 6 个 Reject、Spec 稳定、8 个资源检查点及顺序、ClientResult 先于检查点、仅合法 Shot 有服务器 Trace，以及每个合法事务内恰有一个 DamageExec 和 Dummy DamageContext。

Verifier **没有**自行断言 `PktLag` 值；网络条件由 `RunInfo.txt` 与两端原始 `LogNet: PktLag set ...` 交叉确认。Verifier 也不读源码，所以不能单独证明 payload 中没有别的字段。

### 8.6 两次正式运行结果

| RunId | 条件 | Client 结果序列 | Host 结算 | Verifier |
|---|---|---|---|---|
| `20260813_163052` | 双向 0ms、0% loss；端口 17784 | `1 Accepted, 1 Duplicate, 2 InvalidOrigin, 3 InvalidDirection, 4 InvalidTime, 5 InvalidTime, 6 Miss, 7 Accepted` | 仅 Shot 1、7；伤害 25.0、27.5 | 52/52 PASS |
| `20260813_163248` | 每方向 150ms、0% loss；配置约 300ms RTT；端口 17785 | 同上 | 仅 Shot 1、7；伤害 25.0、27.5 | 52/52 PASS |

两组的客户端检查点均为：

```text
ValidAccepted      Shot1 Accepted        Energy=90 Cooldown=0
DuplicateRejected  Shot1 Duplicate       Energy=90 Cooldown=0
OriginRejected     Shot2 InvalidOrigin   Energy=90 Cooldown=0
DirectionRejected  Shot3 InvalidDirection Energy=90 Cooldown=0
StaleTimeRejected  Shot4 InvalidTime     Energy=90 Cooldown=0
FutureTimeRejected Shot5 InvalidTime     Energy=90 Cooldown=0
MissRejected       Shot6 Miss            Energy=90 Cooldown=0
RecoveryAccepted   Shot7 Accepted        Energy=80 Cooldown=1
```

自动序列有意等待 Shot 1 的一秒合法 Cooldown 到期后才开始非法矩阵，因此后续拒绝来自服务器语义，不是本地 Cooldown 门。

## 9. UE_LOG 字段与每条日志回答的问题

| 日志 | 关键字段 | 回答的问题 |
|---|---|---|
| `GAS_M6_INTENT Phase=LocalIntent` | ShotId、Mutation、Origin、Direction、LocalTarget、PredictionKey | 客户端构造了什么；ShotId 与 PredictionKey 是否为两个域；LocalTarget 只用于本地观察 |
| `GAS_M6_INTENT Phase=AuthorityTrace` | ShotId、Age、OriginDelta、AimDot、Target、Impact | 服务器是否用自己的世界命中；字段误差是多少；实际命中谁/哪里 |
| `GAS_M6_INTENT Phase=AuthorityRejected` | ShotId、Reason、Spec、PredictionKey、Avatar | 哪个服务器 Ability 事务在何种业务原因下拒绝 |
| `GAS_M6_INTENT Phase=Committed` | ShotId、Spec、PredictionKey、Target、RemainingHealth | 哪个 Shot 真正完成权威结算；最终 Target/Health 是什么 |
| `GAS_M6_INTENT Phase=ClientResult` | ShotId、Result、Owner | owning client 收到的明确业务结果；不是激活失败回调 |
| `GAS_M6_INTENT_AUTO Phase=Input` | Mutation、Expected、SerialBefore、Energy | 自动化将要发什么，避免用旧结果误判 |
| `GAS_M6_INTENT_AUTO Phase=Checkpoint` | Name、ShotId、Result、Energy、Cooldown | 结果与资源状态是否最终收敛 |
| `GAS_M6_INTENT_AUTO Phase=SequenceComplete` | Result/Reason | 唯一总门禁；超时会记录 Fail |
| `GAS_DAMAGE_EXEC` | Base、Health、Vulnerability、Critical、Final | 服务器实际计算了几次、伤害数值如何形成 |
| `GAS_DAMAGE_CONTEXT` | Target、Damage、Critical、HitType、Impulse | 服务器 Context 最终作用于谁、携带什么命中表现数据 |
| `GAS_TARGET_TRACE` | Start、End、Hits、Selected | 客户端相机查询的调试视角；不能作为权威命中证据 |
| `LogNet` | PktLag、PktLoss、listen、join | 网络模拟是否真的应用、连接是否建立 |

若服务端等待 5 秒仍未收到 TargetData，现有 `ReportAuthorityIntentResult` 会记录 `Phase=AuthorityRejected ShotId=0 Reason=TargetDataTimeout`，并经同一 Reliable 结果 RPC 返回 `ShotId=0 / TargetDataTimeout`。该字段组合回答“是否由服务端等待超时终止，而不是 Schema、命中或 Commit 失败”；两组正式日志中均没有该记录，所以目前只能说明实现存在，不能说明专项分支已跑通。

证据限制：`GAS_DAMAGE_EXEC` 与 `GAS_DAMAGE_CONTEXT` 当前没有 ShotId。Verifier 通过“全局恰好两次”以及每个 `AuthorityTrace -> DamageExec -> DamageContext -> Committed` 窗口内恰好一次来归因 Shot 1/7。这是严格的顺序归因，但不如端到端 CorrelationId 直接；M6 剩余应把 ShotId 写入自定义 EffectContext 和两条日志。

## 10. Visual Studio 断点操作手册

本节是复现/诊断方法，不是本轮 Headless 正式通过的额外证据。用 Development Editor 构建，先启动可见 Host/Client，再在 VS 2022 选择 `Debug > Attach to Process`，按 `RunInfo.txt` 的 PID 区分两个 `UnrealEditor.exe`，Code Type 选 Native。

### 10.1 客户端断点

| 位置（当前行号仅作导航） | 条件/Watch | 回答的问题 |
|---|---|---|
| `TargetActor.cpp:87 SendLocalTargetData` | `ActorInfo->IsLocallyControlled()`、NetMode、LocalRole | 当前到底是 owning client 还是 authority 本地路径 |
| `TargetActor.cpp:99-108` | AvatarActor、ProjectASC、`ValidData` 绑定 | 初始化失败是否先广播空数据，再 `EndTask` 让 Ability 取消 |
| `TargetActor.cpp:125` ShotId 赋值后 | `Intent->ShotId`、`NextLocalDamageShotId`、TestMutation | ID 是否单调；Duplicate 夹具是否真的复用旧 ID |
| `TargetActor.cpp:160-196` | `IntentHandle` 与 `LocalTargetDataHandle` 各自内容；Task State | 发到服务器的是 Intent，而本地广播的是预测 HitResult；正常路径是否 `EndTask` |
| `multiplayerGameplayAbilityTargetData.cpp:129 NetSerialize` | `Ar.IsSaving()/IsLoading()`、四字段 | 网络边界实际序列化了什么 |
| `multiplayerGameplayAbility.cpp:311 HandleTargetData` | `bIsAuthority=false`、ShotId、TargetData | 客户端预测 Commit 使用了什么，本地结果不能越权到服务器 |
| `ASC.cpp:99 ClientDamageIntentResult_Implementation` | ShotId、Result、ResultSerial | 精确业务结果何时返回；是否与当前等待事务匹配 |

### 10.2 服务器断点

| 位置 | 条件/Watch | 回答的问题 |
|---|---|---|
| `TargetActor.cpp:45-65 Activate` | delegate handle、`bDataWasAlreadyReceived`、timer remaining | 服务端是否按 Spec/PredictionKey 等待；没有现成数据时是否启动 5 秒上限 |
| `TargetActor.cpp:71 OnDestroy` | delegate 绑定数、timer active | 任一结束路径是否移除回调并清定时器 |
| `TargetActor.cpp:199 OnTargetDataReplicated` | SpecHandle、ActivationPredictionKey、TargetData ScriptStruct、timer | GAS 是否把数据交给正确激活；正常到达是否先清 timeout |
| `TargetActor.cpp:210 HandleRemoteTargetDataTimeout` | 条件断点；ShotId=0、Result | 5 秒未到数据时是否返回 `TargetDataTimeout`、广播空 handle 并 EndTask |
| `TargetActor.cpp:229 ProcessAuthorityIntent` | `bConsumeReplicatedData=true`、ActivationTag | 是否消费缓存；额外 ActivationTag 是否被拒；处理后是否 EndTask |
| `TargetData.cpp:145 Validate...Schema` | `Handle.Num()`、`GetScriptStruct()`、ShotId | 空/错误/多条/零 ID 如何被拒绝 |
| `TargetActor.cpp:277 ResolveAuthorityIntent` | ServerOrigin、ServerNow、Intent | 服务器与客户端字段的基准分别是什么 |
| `TargetActor.cpp:300 TryConsumeDamageIntent` | 条件 `Intent.ShotId==2` 等；Guard 前后状态 | ID 是否在字段/Trace 之前消费；拒绝请求能否复用 |
| `TargetData.cpp:67 Guard::TryConsume` | LastProcessedShotId、Delta、Interval | Duplicate/Stale/RateLimited 的精确原因；限流 ID 是否推进 |
| `TargetData.cpp:164 Validate...Fields` | Dist、SizeSquared、AimDot、RequestAge | 哪个字段越界，边界值是否一致 |
| `TargetActor.cpp:334 SweepSingleByChannel` | Start、End、Shape、Channel、OutServerHit | 权威命中是否从服务器眼点、在当前世界执行 |
| `TargetActor.cpp:380 ReportAuthorityIntentResult` | ShotId、Result | 拒绝是否在 Commit 前结束并返回精确原因 |
| `GameplayAbility.cpp:311 HandleTargetData` | `bIsAuthority=true`、TargetActor | Task 是否只把服务器生成的目标交给 Ability |
| `GameplayAbility.cpp:354-384` | TargetASC、DamageSpec、Client Result | TargetASC/Spec 是否在 Commit 前验证；两个失败终点是否分别返回 `InvalidTarget/CommitFailed` |
| `GameplayAbility.cpp:386 CommitAbility` | Energy/Cooldown 前后 | 权威资源是否只在语义和结算对象通过后消费；失败是否返回 `CommitFailed` |
| `GameplayAbility.cpp:408 AddHitResult` | `TargetHit` 指针和 Impact | 写入 Context 的 HitResult 是否来自服务器 resolved handle |
| `GameplayAbility.cpp:413 ApplyGameplayEffectSpecToTarget` | SourceASC、TargetASC、Spec Context | 最终伤害目标和 Context 是否匹配服务器命中 |

如需进入引擎源码，可在 `UAbilitySystemComponent::ServerSetReplicatedTargetData_Implementation`、`CallReplicatedTargetDataDelegatesIfSet`、`ClientActivateAbilityFailed` 设函数断点。前两者回答 TargetData 怎样按 Spec/PredictionKey 路由；最后一个应在 M6Intent 语义拒绝矩阵中保持不命中，在 Immunity 激活拒绝实验中才应命中。

调试时不要只比较客户端与服务器日志里的 Key 文本是否相同；网络两端显示形式可能不同。应分别 Watch `SpecHandle`、ActivationPredictionKey 和业务 ShotId，并按各自职责关联。

## 11. Collision、Navigation 与 Insights 操作

### 11.1 Collision：人工场景诊断，尚未列入正式门禁

操作：

1. 不加 `-Headless` 启动 M6Intent Host/Client，暂不加 `-AutoSequence`，避免断点前序列跑完。
2. 在客户端按 `7` 让服务器生成/重置 Dummy，按 `Alt+C`（或控制台 `show Collision`）显示碰撞。
3. 选中 `multiplayerGASTargetDummy`，核对 C++ 设置的 `BlockAllDynamic` 对 `Visibility` 为 Block。
4. 打开 `Tools/Window > Developer Tools > Collision Analyzer`，Start Recording，触发一次合法射击和一次遮挡射击，Stop。
5. 过滤 `GASDamageAuthorityTrace`，检查 600cm/35cm Sweep、Ignored Actor、实际阻挡体和 `AuthorityTrace Impact`。Ability 不应再出现 `GASDamageTargetValidation` 查询。

该工具回答：“服务器查询到底被哪个碰撞体、Profile、Channel 和形状阻挡？”它不回答网络包是否安全，也不能替代服务器日志与自动断言。

### 11.2 Navigation：空间夹具检查，不是命中算法依赖

操作：在 Editor Viewport 按 `P` 显示 NavMesh，或运行时控制台 `show Navigation`；检查玩家前方 300cm 的 Dummy 生成点、地面和遮挡体关系。

当前 `ServerRequestBaselineEnemyTarget` 直接在角色前方 300cm Spawn，使用 `AdjustIfPossibleButAlwaysSpawn`；没有 `ProjectPointToNavigation`、AI MoveTo 或 Nav Query。Navigation 可回答“测试夹具是否落在合理可行走空间、碰撞调整是否把它挪到意外位置”，但不能证明 Damage Intent 安全，也不应成为当前 Trace 的 Pass 条件。若 M7 加移动目标/历史姿态，再把 Nav 路径与 rewind 样本对齐。

### 11.3 Unreal Insights：M8 性能路线，本轮未采集

建议在两进程参数追加：

```text
-trace=cpu,frame,bookmark,net -statnamedevents
-tracefile=E:\ueprojrct\multiplayer\Saved\Traces\M8_M6Intent_<Host|Client>.utrace
```

然后用：

```powershell
& 'E:\program\ue554\UE_5.5\Engine\Binaries\Win64\UnrealInsights.exe'
```

打开两份 `.utrace`：

- Timing Insights：过滤 `GASDamageAuthorityTrace` 和相关 AbilityTask，比较 Accepted、字段拒绝、Duplicate、Miss 的 CPU 路径，并确认每个合法请求只有一次权威场景查询。
- Network Insights：检查 TargetData RPC 和 `ClientDamageIntentResult` 的次数、大小、Reliable backlog；确认最小 payload 的实际带宽收益。
- Frames/Stats：统计每帧意图数量、Trace 时间 P50/P95/P99、候选过滤成本和峰值。
- Bookmarks：M8 应在 `LocalIntent / GuardResult / AuthorityTrace / Committed / ClientResult` 写同一个 ShotId bookmark，避免只靠日志文本对齐。

Insights 回答“这条安全链花了多少 CPU/带宽、尾延迟在哪里”，不回答命中是否公平；公平性属于 M7 rewind 设计和命中验证。

## 12. 真实问题、风险与证据缺口登记

类型定义：**代码审查风险（已修正）**表示源码审查发现了风险并在运行触发前修正，不能冒充实际发生过的玩家 Bug；**开放风险**表示当前设计在某场景仍可能失败；**证据缺口**表示实现可能存在但没有相应运行证据；**方案取舍**表示有意识的利弊，不按 Bug 统计。

| ID | 类型 | 状态 | 内容 | 证据/处置 |
|---|---|---|---|---|
| M6-DI-001 | 代码审查风险（已修正） | 已修正、已重编译并重跑 | 旧实现中 ShotId 在字段验证后才消费；审查发现同一 ID 可改字段重试，形成验证边界 oracle。该问题没有作为玩家 Bug 在正式运行中复现 | 当前 Guard 在 Source/字段/Trace 前；最终 Build、Automation、0/150ms run 均晚于修改。仍应补“同 ID 首次 InvalidOrigin、第二次修正后 Duplicate”的端到端用例 |
| M6-DI-002 | 开放风险 | 开放 | 50ms 硬限流无 burst credit，且位于 GAS 激活后，不能抵御连接级洪泛 | M6 剩余：按武器 cadence/token bucket；加 connection/ASC counters 与阈值遥测 |
| M6-DI-003 | 证据缺口 | 开放 | `RateLimited/StaleSequence/InvalidSchema/InvalidShotId/SourceDead/InvalidTarget/CommitFailed/TargetDataTimeout` 未做双进程矩阵 | 在当前 52 项之外扩充 `GASM6IntentAuto` 与 verifier；原始畸形 Schema 和“客户端完全不发 TargetData”需测试专用 RPC/网络 fixture |
| M6-DI-004 | 开放风险 | 开放 | Client server-time 只是估计；2s 内的伪造时间仍可能通过 | 当前时间仅做 freshness filter且不 rewind；M7 用服务器保存历史、clamp rewind window 和 server receipt time |
| M6-DI-005 | 方案取舍 | 待数据调优 | 150cm Origin、35° Aim cone、35cm Sweep 比较宽容 | M8 记录分布；按 P99 正常误差和误命中率收紧，不凭直觉修改 |
| M6-DI-006 | 开放风险 | 开放 | Guard/Local allocator 的 ASC 生命周期、重连、Seamless Travel、uint32 回卷缺少会话级协议证明；当前代码没有业务路径显式调用 Guard Reset | 保持 PlayerState ASC 跨普通 respawn 的高水位是合理的；为新会话定义 epoch/nonce 或重建规则并做重连测试 |
| M6-DI-007 | 证据缺口 | 开放 | 正式 run 是 Listen + 1 Client、Headless、0% loss；没有 Dedicated、2 Clients、loss/jitter/reorder | M6 剩余补拓扑和弱网矩阵，不能借用 Immunity Reject Lab 的丢包结论 |
| M6-DI-008 | 证据缺口 | 开放 | Checkpoint 只证明结果后的资源收敛，不证明拒绝过程无一帧闪烁 | 可见窗口录像 + 帧级状态日志；必要时给 ShotId 驱动的 pending presentation 明确清理 |
| M6-DI-009 | 可观测性风险 | 开放 | DamageExec/Context 没有 ShotId，Verifier 只能用精确计数和窗口顺序归因 | 把 ShotId 写入自定义 EffectContext并贯穿 Exec/Attribute/Cue/Insights |
| M6-DI-010 | 代码审查风险（已修正） | 已修正、已编译并完成网络回归 | 旧实现中 TargetTask Sweep 后 Ability 又做另一套距离/LOS Trace，存在重复查询成本与不同几何规则漂移 | 当前由 TargetTask 唯一拥有几何查询，Ability 只复验对象不变量；Run `20260816_110557` 为 52/52 PASS。Reliable Result 在拒绝洪峰下的 backlog 仍需 Insights 测量 |
| M6-DI-011 | 证据管理风险 | 开放 | 当前工作树未提交，`dc3969f` 不能唯一定位所测内容 | 使用文末 SHA256；合并前提交源码、脚本和证据索引，并在 CI 绑定 commit/build artifact/run |
| M6-DI-012 | 代码审查风险（已修正） | 已修正、已重编译并重跑正常/语义路径 | 审查发现一次性 TargetTask 需要显式收口：服务端远端数据等待不能无上限，Task 销毁必须清 delegate/timer，客户端初始化失败必须通知 Ability，正常处理也必须结束 Task。该风险没有作为玩家 Bug 在正式运行中复现 | 已实现 5 秒 `TargetDataTimeout`、`OnDestroy` 清理、客户端失败广播空 handle、所有终点 `EndTask`。最终 Editor Build/Automation/两组 run 晚于修改；但 timeout 和客户端初始化失败专项分支尚未运行验证 |
| M6-DI-013 | 代码审查风险（已修正） | 已修正、已重编译并重跑成功路径 | 审查发现 Damage 若在确认 TargetASC/有效 Spec 前 Commit，存在先消费权威资源、随后无法落伤害或缺少明确 Result 的风险。该风险没有作为玩家 Bug 在正式运行中复现 | 已把 TargetASC/Spec 验证移到 Commit 前，并让 InvalidTarget、Spec/CommitFailed、Accepted 等服务器业务终点有明确 Result。最终两组 run 覆盖 Accepted 和 Task 语义拒绝；TargetASC/Spec/Commit 失败专项分支仍属 M6-DI-003 证据缺口 |

## 13. 客户端 HitResult 信任风险说明

如果服务器直接接受 `FGameplayAbilityTargetData_SingleTargetHit`，恶意客户端可以伪造 Target Actor、ImpactPoint、Normal、Bone、PhysicalMaterial 等字段；这些字段进一步进入 Damage、暴击、弱点、穿透或 GameplayCue，就会把表现数据升级为权威游戏数据。

当前实现的防线是：

1. 精确 ScriptStruct 只接受 Damage Intent；错误 `SingleTargetHit` Schema 会拒绝。
2. Intent 自身 `HasHitResult=false`、Actors 为空。
3. Client local HitResult 只走本地预测广播；网络上传的是另一份 Intent handle。
4. Server Sweep 生成全新的 `FHitResult`，服务器再从该 Hit 取 Candidate。
5. Authority Ability 只有在收到服务器 resolved handle 后才 `AddHitResult` 到 Context。

需要长期保持的代码审查不变量是：任何服务器路径看到 `TargetData.Get(0)->GetHitResult()` 时，都必须能证明该 Handle 是本服务器刚生成的，而不是客户端反序列化对象。以后新增武器、穿透、多目标或 projectile 时要重复审查这一点。

## 14. 后续路线

### 14.1 M6 剩余：先关闭安全矩阵

1. 给 InvalidSchema、ShotId 0、StaleSequence、RateLimited、SourceDead、InvalidTarget、CommitFailed、TargetDataTimeout 增加双进程行为用例；对 timeout 要用“客户端激活成功但故意不提交 TargetData”的专用夹具，断言约 5 秒后 `ShotId=0 / TargetDataTimeout`、空 handle、Ability/Task 都结束且 delegate/timer 清零。
2. 增加“非法字段已消费，同 ID 修正后必须 Duplicate”和“RateLimited ID 不可重试”。
3. 用 token bucket/武器 cadence 代替通用 50ms 硬阈值，并测试 burst、长时间 idle、不同 Ability。
4. 定义会话 epoch、重连、Seamless Travel、PlayerState/ASC 重建与 uint32 回卷策略。
5. ShotId 进入 EffectContext、DamageExec、Attribute、Cue、ClientResult 和 verifier；日志带 NetMode/Connection/Role。
6. 补 Dedicated + 2 Clients、0/150/300ms 每方向、loss/jitter/reorder 多轮矩阵，以及非 Headless 表现门禁。

### 14.2 M7：有限 Server-Side Rewind

1. 服务器按固定频率保存可命中 Actor 的时间戳、位置、旋转和必要碰撞体历史，不接受客户端历史变换。
2. 以 server receipt time 和经过 clamp 的客户端估算时间计算 rewind 时刻；拒绝窗口外时间。
3. 在隔离 scene/query 或临时历史碰撞体上验证 LOS/命中，再恢复当前世界；避免修改真实 Actor 导致并发副作用。
4. ShotId/Source/Weapon 与历史查询一一关联；当前世界命中、rewind 命中、超窗拒绝都要结构化记录。
5. 测移动目标、遮挡出现/消失、极端方向、客户端时钟误差和多玩家交叉射击；公平性与安全性分别统计。

### 14.3 M8：Insights 驱动优化

1. 在 Insights 中测每类意图的 CPU、Trace/LOS 次数、RPC bytes、Reliable backlog、P50/P95/P99，而不是先做无数据优化。
2. 测量唯一 authority sphere sweep 是否需要 broadphase/批处理，以及未来历史缓存成本是否可控；不得在无数据时重新增加第二套几何策略。
3. 暴露 Accepted、Duplicate、Stale、RateLimited、Invalid*、Miss counters；按玩家/Ability/时间窗聚合，避免日志洪泛。
4. 建立性能预算与回归门禁后，再讨论 Iris/Replication Graph 或更大的网络架构改造。

## 15. 证据索引、哈希与最终边界

正式原始证据：

- `Saved/GASBaseline/20260813_163052/{RunInfo.txt,Host.log,Client.log,M6IntentSummary.json,M6IntentSummary.md}`
- `Saved/GASBaseline/20260813_163248/{RunInfo.txt,Host.log,Client.log,M6IntentSummary.json,M6IntentSummary.md}`
- `Saved/Logs/multiplayer.log`（DamageIntent Unit，成功且 Exit 0）
- `Scripts/VerifyGASM6IntentLogs.ps1`

以下时间均为报告机本地时间（Asia/Shanghai），SHA256 为本次核对时的完整值。当前相关源码快照：

| 文件 | 字节 | 最后写入时间 | SHA256 |
|---|---:|---|---|
| `Source/multiplayer/AbilitySystem/multiplayerGameplayAbilityTargetData.h` | 3,612 | 2026-08-13 16:27:19 | `3364EBE4ADF063EE1F56C6292138A73C35A06D67FD5DCC11E0450F2B40DB104E` |
| `Source/multiplayer/AbilitySystem/multiplayerGameplayAbilityTargetData.cpp` | 6,536 | 2026-08-13 16:27:22 | `FCC98FC3CF0A51F06E8E272107E3D8ADDBCCCF431C0CAA6D5B783D248EAC791F` |
| `Source/multiplayer/AbilitySystem/multiplayerAbilitySystemComponent.h` | 3,539 | 2026-08-13 15:35:00 | `9A485E226536824A50985770D63432BB7BF3BB86981B2183731FA34C8BEE37D2` |
| `Source/multiplayer/AbilitySystem/multiplayerAbilitySystemComponent.cpp` | 8,962 | 2026-08-13 15:35:05 | `8D31FA894E9C39B1DA398BE685D628F636257E0446555542CFF47E1EA467D24D` |
| `Source/multiplayer/AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h` | 1,990 | 2026-08-13 16:27:26 | `E80CCC44E7CDBCCE20905E48553CBE0307C356439BAD68DF7E4F41DA669299EB` |
| `Source/multiplayer/AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.cpp` | 16,489 | 2026-08-13 16:27:29 | `364B6DE6FAC6480A7FB3F26EC8F0FDA2604A03E4FD9C0B574CA45CEA3EA9A6C9` |
| `Source/multiplayer/AbilitySystem/Abilities/multiplayerGameplayAbility.h` | 2,744 | 2026-08-13 15:30:34 | `D002AA20A12AF95F02D0BD804580395ED7CF7578216B77747382AE893D352781` |
| `Source/multiplayer/AbilitySystem/Abilities/multiplayerGameplayAbility.cpp` | 20,291 | 2026-08-13 16:28:55 | `020949FB11647D5FAB8CA427D9E1451A78E2BBD30423CCCA7B6F33AF77D29372` |
| `Source/multiplayer/multiplayerCharacter.h` | 9,754 | 2026-08-13 15:37:27 | `AE7AFDD1DE90AEF53FFABEFFF60B9ABA93F62F439DFF040CA65ECF7D5A40648E` |
| `Source/multiplayer/multiplayerCharacter.cpp` | 48,511 | 2026-08-13 15:43:24 | `0C3797DD590AA329A2F78583F8F0C4539B6451866ECD6311529C28A8B851B055` |
| `Source/multiplayer/Tests/multiplayerGASAutomationTests.cpp` | 24,901 | 2026-08-13 15:31:54 | `53E28B3798620F771DAA2470B33B74C43CFC5C158EA7BDF6E64C69C4EDDD2C15` |

编译、Automation 与核验器快照：

| 文件 | 字节 | 最后写入时间 | SHA256 | 证据边界 |
|---|---:|---|---|---|
| `Binaries/Win64/UnrealEditor-multiplayer.dll` | 1,321,472 | 2026-08-13 16:29:20 | `1B5858B77287E80423B1C42CAB1B6692B15C42BCC2609AA0F26C787130980D52` | 两组正式 UnrealEditor 双进程 run 使用的项目模块，时间晚于相关源码 |
| `Binaries/Win64/multiplayer.exe` | 303,550,464 | 2026-08-13 16:34:37 | `8D1C8FF2C0E87A694D3217A16A4D53594F5EA4AB01E3846662758124A8B40504` | Development Game Build 产物；生成于正式 runs 之后，不是两组 run 的可执行文件 |
| `Saved/Logs/multiplayer.log` | 125,634 | 2026-08-13 16:29:59 | `49AC38A599FCF41F04126115A0333B5DD581D64671CBD55E4453A224641F0510` | 记录 DamageIntent Unit Found 1、成功、Exit 0 |
| `Scripts/VerifyGASM6IntentLogs.ps1` | 25,933 | 2026-08-13 15:59:21 | `07CAC55AC2E17E1126A0D3386EA8EE6190AE98C46FD1B35F232E8C6FDD2CBDEC` | 对下列两组证据产生 52 项判定 |

正式运行文件快照：

| Run | 文件 | 字节 | 最后写入时间 | SHA256 |
|---|---|---:|---|---|
| 20260813_163052 | `RunInfo.txt` | 867 | 2026-08-13 16:31:36 | `8802C66C9481DCCC19C14395252E1EE59C59A8EF0E927E144B9DC7CA188495E2` |
| 20260813_163052 | `Host.log` | 157,354 | 2026-08-13 16:31:50 | `C06C86A48E1C4F171E922BC8230EF542C99806880A0D44C71A196B02ACBA57D2` |
| 20260813_163052 | `Client.log` | 170,960 | 2026-08-13 16:31:50 | `17B7B85EAADB682F19D0C6E9D49017CA148759573B5527F0E1442E7208BFEE26` |
| 20260813_163052 | `M6IntentSummary.json` | 27,767 | 2026-08-13 16:39:42 | `FA501A7A913B5DC8581F9DA1DD633EAAFDF15F996E7DA7F953C275F4AF10871D` |
| 20260813_163052 | `M6IntentSummary.md` | 8,101 | 2026-08-13 16:39:42 | `4425565675E4EB9603CC9C3783E15313AC9DB24ED2C0427D6D04D473A42E8B32` |
| 20260813_163248 | `RunInfo.txt` | 877 | 2026-08-13 16:33:37 | `F044CE27ABFF488CC0345970D4BF48AEBD91CC6DA5B9F0373B093B4CC13BD4F8` |
| 20260813_163248 | `Host.log` | 156,985 | 2026-08-13 16:33:52 | `5F4931799CC2DF5ABAC3902435729DE6E219A5322B9EB6F5563450C0C7A61700` |
| 20260813_163248 | `Client.log` | 173,908 | 2026-08-13 16:33:52 | `1DEBE3326C1559E43154F2F4BFD3B111BD6183663DD7FB981B10D51527435B03` |
| 20260813_163248 | `M6IntentSummary.json` | 27,767 | 2026-08-13 16:39:48 | `393B2308952E41C82229B2838230C0E06899F8092E914A15E5960EA68F341351` |
| 20260813_163248 | `M6IntentSummary.md` | 8,101 | 2026-08-13 16:39:48 | `10A60291C68623AA0D37620D18B3DE5DFDAEE8DF4482E81216A69A9564485F39` |

最终可对外表述为：

> M6 Damage 已从“信任客户端目标/HitResult”改为最小四字段意图；服务器在消费业务 ShotId 后校验字段并用当前世界重新 Sweep，只把服务器命中写入 EffectContext。一次性 TargetTask 已加入 5 秒等待上限、销毁清理和各路径显式结束，Damage 在 Commit 前验证 TargetASC/Spec 并为服务器业务终点给出明确 Result。最终源码通过 Editor/Game Development Build 和 DamageIntent 单元测试；Listen Server + 1 Client 在 0ms 与每方向 150ms、0% loss 下，合法、重复、伪造 Origin、反向方向、过旧/未来时间、Miss、恢复矩阵均完成，专用核验器两组都是 52/52 PASS。`TargetDataTimeout` 已实现，但专项运行分支仍待验证。

不得扩写为：

> 已完成完整反作弊、所有拒绝类型、丢包/Dedicated/双客户端、Server-Side Rewind、视觉验收或 Insights 性能优化；也不得把 `TargetDataTimeout` 的源码实现写成专项运行覆盖。
