# GAS M5 GameplayCue 预测、确认与生命周期验收报告

> 分支：`coop-GAS`
>
> 日期：2026-08-13

## 1. 已实现并完成双进程验证的权威边界

```text
Damage 输入
-> Owning Client TargetData 预测窗口
-> Source ASC 执行 GameplayCue.Coop.Damage.Cast
-> 本地立即播放
-> Server 使用同一 PredictionKey 执行并 multicast
-> Owning Client 吸收回放，Observer 播放一次

Server Damage GE
-> ExecCalc 写 Critical / HitType / ImpactImpulse
-> Target ASC 执行 GameplayCue.Coop.Damage.Impact
-> Target 的原生 Cue Handler 消费权威 Context
```

`Damage.Cast` 只表达施法响应，不决定命中、伤害或暴击。`Damage.Impact` 只由服务器结算后的 Damage GE 触发。

## 2. Cue 映射

| Cue | 触发端 | 类型 | 消费者 |
|---|---|---|---|
| `GameplayCue.Coop.Damage.Cast` | Source ASC，预测 + 服务器 | 瞬时 Execute | Character 黄色闪光 |
| `GameplayCue.Coop.Damage.Impact` | Target ASC，服务器确认 | 瞬时 Execute | Character/TargetDummy 普通红色或暴击橙色闪光 |
| `GameplayCue.Coop.Heal.Cast` | Source ASC，预测 + 服务器 | 瞬时 Execute | Character 蓝色闪光 |
| `GameplayCue.Coop.Heal.Result` | Target ASC，服务器确认 | 瞬时 Execute | Character 绿色闪光 |
| `GameplayCue.Coop.State.Immunity` | 预测 Immunity GE | OnActive/WhileActive/Removed | Character 蓝色持续状态灯 |
| `GameplayCue.Coop.State.Vulnerability` | 服务器 Vulnerability GE | OnActive/WhileActive/Removed | Target 紫色持续状态灯 |
| `GameplayCue.Coop.Death` | 服务器死亡事务 | 瞬时 Execute | Character/TargetDummy 红色死亡反馈 |

当前项目约束是“同一个 Cue Tag 只有一个表现所有者”。UE 的原生 DefaultHandler 是否继续执行取决于 GameplayCueNotify 的 consume/forward 行为；加入正式 CueNotify 前必须明确该策略，不能假设资产与原生 fallback 会自动叠加或自动去重。本阶段只使用原生 Handler。

## 3. 预测与去重的准确表述

- GAS 去重的是“服务器对预测发起者的同 PredictionKey 回放”。
- Observer 没有该本地 PredictionKey，因此仍播放服务器 Cue 一次。
- GAS 不会消除同一端业务代码错误调用两次；每次激活必须只发射一次。
- `ExecuteGameplayCue` 是瞬时反馈，不能倒放回滚。
- Immunity 的持续 Cue 由预测 GE 管理；预测拒绝或状态追平会清理临时 Cue Tag。
- TargetData 被服务器判无效只代表“不结算目标结果”，不等于 `ClientActivateAbilityFailed`。真正激活拒绝与 Cost/Cooldown 回滚属于 M6 强制拒绝实验。

## 4. 自动化矩阵

| 编号 | 检查 | 状态 |
|---|---|---|
| M5-AUTO-01 | 7 个 GameplayCue Tag 已注册 | 通过 |
| M5-AUTO-02 | Character/TargetDummy 实现原生 Cue 接口 | 通过 |
| M5-AUTO-03 | DeveloperSettings 包含 `/Game/GAS/GameplayCues`；当前原生 Handler 不依赖 Cue 资产 | 通过 |
| M5-AUTO-04 | Damage/Heal 的结果 GE 包含预期 Cue 静态映射 | 通过 |
| M5-AUTO-05 | Immunity/Vulnerability 的持续 GE 包含预期 Cue 静态映射 | 通过 |
| M5-AUTO-06 | Vulnerability 配置 `bSuppressStackingCues=true`，抑制叠层重触发 | 通过；0ms/弱网运行也证明同一生命周期内叠层不重触发 |

自动化只能证明静态配置，不能证明网络时序、回放去重或肉眼表现。

自动化证据：2026-08-13 最终执行 `multiplayer.GAS.Configuration`，发现 1 个测试，结果 `成功`，进程退出码 0。Editor 与 Game Development 目标均在全部 M5 修改后编译通过。日志中的 Zen/DDC 告警来自本机缓存不可用，不属于测试断言失败。

双进程运行可显式向 Client 传入 `-GASM5Auto`。该非 Shipping 测试序列调用与键盘相同的输入函数和真实 Server RPC，不绕过 GAS 权威链；它只解决本地窗口焦点不稳定的问题。日志后处理器只对已执行实验计数，不能替代网络时序审查或肉眼表现验收。

## 5. 0ms 双窗口矩阵

| 编号 | 操作 | 预期 | 状态 |
|---|---|---|---|
| M5-RUN-01 | Client LMB 命中一次 | Owner Client Cast Handler 1 次；Host（Observer）1 次；Owner 无服务器重复回放 | 通过；5 次合法 Damage 激活，Host/Client Cast Handler 均为 5 |
| M5-RUN-01B | Host LMB 命中一次 | Host Owner 1 次；Client（Observer）1 次 | 待人工反向输入；不影响 Client 预测链结论 |
| M5-RUN-02 | 同一次命中 | Target Impact 每端 1 次，且只在服务器结算后出现 | 通过；5 次 TargetData 选择对应 Host/Client 各 5 次 Dummy Impact |
| M5-RUN-03 | 连续命中到半血以下 | 前两次普通，第三次权威 Critical；消费自定义 Context | 通过；伤害 `25.0 -> 27.5 -> 45.0`，第三击双端 `Critical=true/HitType=Critical/ImpactDir!=0`；颜色需可视录屏补证 |
| M5-RUN-04 | 受伤后按 Q | Cast 预测一次；Result 确认一次；服务器执行 Healing | Cue 链通过；Host/Client Cast 与 Result 各 1 次，服务器 Healing=30；本轮未单独导出最终 Health 数值断言 |
| M5-RUN-05 | 按 E | Owner 立即显示状态；OnActive/WhileActive/Removed 生命周期完整；约 5 秒清理 | 通过；双端分别为 `1/1/1`，持续约 5 秒，无第二个实例 |
| M5-RUN-06 | 命中一次后等待 8 秒 | Vulnerability 状态清理；Removed 一次 | 通过；第二个生命周期约 7.85 秒后双端各 Removed 1 次 |
| M5-RUN-07 | 按 7 Reset | 旧 Vulnerability Cue 清理，无残留状态 | 通过；Reset 前先出现 Removed，随后 Target Reset；双端一致 |
| M5-RUN-08 | 玩家死亡/复活 | Death 每次事务一次；新 Avatar 无旧状态 | 核心通过；双端 Death 各 1 次，3 秒后新 Avatar 初始化；“持有持续 Cue 时死亡”仍待独立运行 |

0ms 证据：`Saved/GASBaseline/20260813_131659/`。结构化结果见同目录 `M5Summary.md` 与 `M5Summary.json`。该运行由 Client 发起 5 次合法 Damage、1 次 Immunity、1 次 Heal，并完成 Vulnerability 自然到期、Reset、玩家死亡与复活；Host/Client 均无 Fatal/Ensure。

## 6. 150ms 单向延迟矩阵

若 Host 和 Client 都设置 `PktLag=150`，每个方向约 150ms，RTT 约 300ms，报告不得写成“150ms RTT”。日志必须出现延迟注入配置才有效。

| 编号 | 预期 | 状态 |
|---|---|---|
| M5-LAG-01 | Owner 的 Cast 先于 AuthorityEmit；最终 Owner Handler 仍只有一次 | 通过；4 组合法 Damage 的预测领先服务器 `115/121/120/115ms`，两端各 4 次 Handler |
| M5-LAG-02 | Impact/Critical 只在服务器确认后出现 | 通过；客户端只在服务器 ExecCalc/Impact 后收到结果 Cue，没有预测最终命中或 Critical |
| M5-LAG-03 | Immunity 立即产生事件，确认后不重复生命周期，最终正常清理 | 日志通过；双端 `OnActive/WhileActive/Removed=1/1/1`，Owner 先于 Host 约 143ms；肉眼闪烁/颜色待非 Headless 录屏 |
| M5-LAG-04 | 不同激活使用不同 PredictionKey，延迟回包不清理新 Cue | 核心通过；合法 Damage 使用 `2/4/7/9` 且各自 CatchUp，事件计数无额外清理；旧回包与新持续 Cue 重叠场景及肉眼残留仍待测 |

弱网证据：`Saved/GASBaseline/20260813_132026/`。Host 和 Client 日志都出现 `PktLag set to 150`，因此是每方向约 150ms、RTT 约 300ms。第二次计划输入发生在客户端仍持有 1 秒 Damage Cooldown 时，没有创建激活 PredictionKey、TargetData 或 Cue；这属于本地门禁，不是网络丢包或服务器拒绝。其余 4 次合法激活全部形成唯一 PredictionKey 配对，CatchUp 在预测后 `292～348ms` 到达。

## 7. 问题复盘：M5-001 第三人称目标在合法距离内却无法命中

### 1. 现象

测试夹具把敌方目标放在 Avatar 前方 300cm，Damage 的服务器允许距离为 600cm，但 Client 的 TargetData 一直为空；Ability 被取消，没有 Damage Cast、ExecCalc 或 Vulnerability。稳定复现，影响玩法正确性和 M5 网络证据。

### 2. 复现条件

1. 使用第三人称 Character，SpringArm 让相机位于 Avatar 后方约 350cm。
2. 在 Avatar 前方 300cm 生成敌方 TargetDummy。
3. 将控制器朝向目标并激活 Damage。
4. 预期：Sweep 选中目标并提交 TargetData。
5. 实际：最初 `Hits=0`；追踪终点停在目标前约 50cm。

### 3. 为什么难

客户端目标选择从 Camera ViewPoint 开始，服务器距离校验从 Avatar 开始；两条链对 `TargetRange` 的空间原点不同。角色到目标合法不等于相机到目标在同一数值范围内。

### 4. 初始假设

| 假设 | 为什么怀疑 | 如何验证 | 结果 |
|---|---|---|---|
| Controller Rotation 同帧未进入 CameraManager | 自动夹具刚设置朝向就激活 | 测试模式改用 ControlRotation，并记录 Start/End | 修正了方向，但仍 `Hits=0`，排除为最终根因 |
| Target 碰撞或 Visibility 通道错误 | Sweep 没返回目标 | 输出 Hit 数、目标位置和追踪终点 | 目标可在更长 Sweep 中命中，排除 |
| 相机偏移吞掉有效射程 | Camera 在 Avatar 后方，目标靠近 Range 尾端 | 比较 Camera、Avatar、Target 和 TraceEnd | 确认 |

### 5. 定位工具

使用结构化日志 `GAS_TARGET_TRACE Phase=Sweep/Selected/NoHostileTarget` 记录起点、终点、命中数和最终候选；双进程自动序列保证每次都走真实 Ability、Sweep、TargetData RPC 与服务器复验。

### 6. 根因

第一版用 `CameraLocation + CameraForward * TargetRange`。`TargetRange` 的玩法语义是 Avatar 到目标的最大距离，但第三人称相机位于 Avatar 后方，实际查询长度被 SpringArm 偏移缩短。

### 7. 候选方案与取舍

| 方案 | 能解决什么 | 新增成本 | 采用/放弃 |
|---|---|---|---|
| 把测试目标放得更近 | 让测试暂时通过 | 掩盖真实第三人称边界 Bug | 放弃 |
| 完全从 Avatar 中心追踪 | 距离定义一致 | 准星与相机所见可能不一致 | 放弃 |
| Camera Sweep 增加 Camera→Avatar 偏移，服务器仍按 Avatar 校验 | 保留准星语义且覆盖完整玩法射程 | 客户端查询略长，但不能扩大权威命中范围 | 采用 |

### 8. 最终解决方案

```text
Client Camera ViewPoint
-> QueryLength = TargetRange + Distance(Camera, Avatar)
-> Sweep + 敌我/存活/ASC 过滤
-> TargetData + PredictionKey
-> Server 仍按 Distance(Avatar, Target) <= TargetRange + 容差复验
```

### 9. 验证结果

| 验证层级 | 结果 |
|---|---|
| Editor/Game 编译 | 通过 |
| 配置自动化 | 1/1 通过 |
| 0ms 双进程 | 5/5 TargetData 选择成功 |
| 每方向 150ms | 4 次合法激活全部选择成功；1 次输入被本地 Cooldown 正常门禁 |
| Fatal/Ensure | 两次证据运行均为 0 |
| 可视录屏 | 待补 |

### 10. 最终效果

- 正确性：第三人称相机偏移不再缩短角色的可用技能射程。
- 架构：客户端负责候选查询，服务器权威范围没有扩大。
- 维护：切换不同 SpringArm 长度时不需要手工改 TargetRange。

### 11. 遗留问题

仍需在遮挡、极端相机偏移、近墙和不同 FOV 下补边界用例；有限服务器回溯尚未实现。

### 12. 可复用经验

客户端查询空间和服务器规则空间可以不同，但必须明确各自原点；扩大客户端候选集合不能扩大服务器权威接受集合。

## 8. 当前遗留问题

- 当前表现为无外部素材的 PointLight 反馈；Niagara、音效和 Montage 仍需正式美术资产。
- 瞬时 Cast Cue 无法回滚，只能在失败时避免权威副作用。
- 真正 `ClientActivateAbilityFailed`、预测 GE/Cost/Cooldown 拒绝回滚尚未验证。
- 丢包、Dedicated Server、晚加入和 Network Insights 尚未进入本阶段证据。
- `Critical`、`HitType` 与 `ImpactImpulse` 都进入 Cue Handler；当前用 HitType/方向结构化日志和命中方向灯位移证明消费，正式物理冲量仍未启用。
- Character 的共享状态灯按 `Death > Immunity > Vulnerability` 维护独立活动标记，避免一个 `Removed` 直接清掉另一个状态；正式多状态表现仍应升级为按 CueTag/实例分别持有组件。

## 9. M5 退出结论

M5 的代码实现、静态自动化、0ms Client 预测接受路径、每方向 150ms 弱网接受路径、服务器确认 Impact/Critical、持续 Cue 到期/Reset 清理和死亡/复活基础链均已通过。阶段状态记为“**接受路径技术闭环完成**”；原路线中的真正拒绝清理门禁明确拆入 M6，正式美术与人工视觉证据仍待补。

不得从本阶段推导出以下结论：真正 `ClientActivateAbilityFailed` 已回滚、瞬时 Cue 可以撤销、Cost/Cooldown 拒绝已验证、丢包/晚加入/Dedicated 已通过。这些保留给 M6 及后续阶段。

> 后续状态（2026-08-13）：M6 已用 Immunity 无 TargetData 探针完成真正 `ClientActivateAbilityFailed` 对预测 Cost、Cooldown、持续 GE/Cue 的回滚验证；M5 本报告仍保留为当时的接受路径历史快照。详见 [M6 报告](GAS_M6_Prediction_Reject_Rollback_Test_Report.md)。
