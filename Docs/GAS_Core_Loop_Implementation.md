# Co-op GAS 核心闭环实施说明

> 分支：`coop-GAS`
>
> UE 版本：5.5
> 更新日期：2026-08-15

完整架构讲解、调用链、技术选型、问题复盘和面试场景题见：
[《Co-op GAS 架构与面试讲解手册》](GAS_Architecture_Interview_Guide.md)。本文件只维护实施状态和运行步骤。

当前阶段顺序、延迟补偿/作弊防护边界和下一批任务见：
[《Co-op GAS 作品集技术路线与执行清单》](GAS_Portfolio_Technical_Route.md)。

## 1. 当前实现

本阶段没有迁移 Aura 的源码或 Content 二进制资源。项目仅参考公开架构思想，针对当前 UE5.5 双人合作项目独立实现：

- 启用 `GameplayAbilities` 插件以及 `GameplayAbilities`、`GameplayTags`、`GameplayTasks` 模块。
- `AmultiplayerGASPlayerState` 持有 ASC 和 AttributeSet。
- 玩家 ASC 使用 `Mixed` GameplayEffect 复制模式。
- Character 是 AvatarActor，PlayerState 是 OwnerActor。
- 服务器在 `PossessedBy`、客户端在 `OnRep_PlayerState` 初始化 ActorInfo。
- 原生 GameplayTag 驱动技能输入与 AbilitySpec。
- AbilitySet DataAsset 支持批量授予能力、应用初始效果和保存撤销句柄。
- 默认 C++ 能力确保不创建资产也可以测试。
- Health、MaxHealth、Energy、MaxEnergy，以及 AttackPower、Armor、CriticalChance、CriticalMultiplier、Resistance 使用 RepNotify；战斗属性 Clamp 分别在 AttributeSet 中收口。
- IncomingDamage、IncomingHealing 是服务器结算用 Meta Attribute。
- 伤害、治疗和免疫均使用 `LocalPredicted` 激活。
- Cost 与 Cooldown 通过 GameplayEffect 预测并由服务器校正。
- 伤害 AbilityTask 将本地 HitResult 只用于预览；通过 PredictionKey 向服务器只上传 DamageIntent（ShotId、量化 Origin/方向和估算 ServerTime）。
- PlayerState ASC 跨 Pawn 分配 ShotId；服务器执行 Schema/source、重放/旧序号、50ms 最小间隔、时间/Origin/方向校验，再从权威 EyeOrigin 在当前世界 Sweep 重建 HitResult。
- 最终伤害和治疗只由服务器应用。
- 免疫使用持续 GameplayEffect、`State.Immune` 和 `UImmunityGameplayEffectComponent`。
- Enhanced Input 通过 InputTag 驱动唯一的正式能力入口；测试目标操作由显式 `-GASDeveloperControls` 夹具提供。
- 基础 HUD 绑定 PlayerState ASC，显示属性、冷却和状态，并在 Avatar 更换时安全重绑。
- 服务器用 `GameplayEffectExecutionCalculation` 计算 AttackPower、Armor、Resistance、Critical 和 Vulnerability；进攻属性按 Source Snapshot 捕获，生命与防御按 Target Live 捕获，随机暴击 Roll 只在服务器生成。
- 自定义 `GameplayEffectContext` 复制 Critical、HitType 和 ImpactImpulse，供确认 Cue 消费。
- Vulnerability 最多三层，按 Target 聚合、应用时刷新持续时间，并抑制叠层 Cue 重触发。
- 死亡、复活、Ability/Task/临时 GE 清理和 ASC Avatar 重绑已形成服务器幂等链。
- 原生 GameplayCue 已覆盖 Damage/Heal Cast、权威结果、Immunity/Vulnerability 生命周期和 Death 通知。

## 2. 正式输入与开发夹具

正式入口只使用 Enhanced Input：

| 按键 | 能力 | 数值 |
|---|---|---|
| 鼠标左键 | 攻击准星内的 `Team.Enemy` GAS 目标 | 伤害 25、能量 10、冷却 1 秒、范围 600；玩家均为 `Team.Player`，不能互伤 |
| `Q` | 自我治疗 | 治疗 30、能量 20、冷却 3 秒 |
| `E` | 状态免疫 | 持续 5 秒、能量 30、冷却 8 秒 |
| `7`（仅 `-GASDeveloperControls`） | 生成/重置敌对训练目标 | Developer Harness 的技术验收入口，不属于正式玩法 |

自动化和正式 InputAction 最终都进入同一 InputTag/ASC 调用链，不存在两套 Gameplay 权威逻辑。

## 3. 伤害网络链路

```text
Owning Client 按下鼠标左键
-> ASC 根据 InputTag 找到 AbilitySpec
-> LocalPredicted 激活并预测 Cost/Cooldown
-> AbilityTask 在本地生成只用于预览的 HitResult
-> 生成 DamageIntent: ShotId / Origin / Direction / estimated ServerTime
-> FScopedPredictionWindow
-> CallServerSetReplicatedTargetData
-> Server 根据 SpecHandle + ActivationPredictionKey 接收数据
-> 验证 Schema/source/ShotId/频率/时间/Origin/方向
-> 服务器当前世界 Sphere Sweep 重建 SingleTargetHit
-> 验证 Team.Enemy / 非 Team.Player / 存活
-> 服务器创建 Damage GameplayEffectSpec
-> 服务器写入 Data.Damage SetByCaller
-> IncomingDamage
-> PostGameplayEffectExecute
-> 扣除 Health
-> Health 复制到两个客户端
```

客户端不能提交目标 Actor、HitResult 或伤害数值。服务器根据受限意图重建命中，伤害量来自服务器能力类默认值。语义结果 RPC 只用于日志/UI，不手工退还资源；Cost/Cooldown 交给 PredictionKey 对账。

权威数值链：

```text
Raw = max(BaseDamage + AttackPower, 0)
-> Armor: 100 / (100 + Armor)
-> Resistance: 1 - clamp(Resistance, 0, 0.8)
-> Vulnerability: 1 + 0.1 * clamp(Stacks, 0, 3)
-> Critical: low-health OR server roll < CriticalChance
-> IncomingDamage -> PostGameplayEffectExecute -> Health
```

默认 AttackPower/Armor/CriticalChance/Resistance 均为 0，CriticalMultiplier 为 1.5，因此旧 M5/M6 基线数值保持兼容。完整捕获策略、公式矩阵和阶段 3 胜利 UI 证据见[阶段 3～4 证据报告](Evidence/Phase3_4_Victory_UI_Combat_Attributes_Report.md)。

## 4. 双客户端手工验证

编辑器 PIE 设置：

1. Number of Players：`2`。
2. Net Mode：`Play As Listen Server`。
3. New Editor Window，两个窗口大小一致。
4. 确认当前 GameMode 没有在蓝图里覆盖 PlayerState Class；正确类型是 `multiplayerGASPlayerState`。
5. 需要人工生成训练目标时，以 `-GASDeveloperControls` 启动，再按 `7`；正式运行不暴露该入口。

测试矩阵：

| 场景 | 预期结果 |
|---|---|
| Client 对敌对目标按一次鼠标左键 | Server 对 `Team.Enemy` 目标扣除 25 Health |
| 连续快速点击鼠标左键 | 1 秒冷却期间不能重复激活 |
| 目标超过 650 单位 | 服务器拒绝结算伤害 |
| 目标隔着阻挡 Visibility 的墙 | 服务器拒绝结算伤害 |
| 玩家按 `Q` | Health 最多恢复到 MaxHealth |
| Energy 不足 | `CommitAbility` 失败，不产生技能结果 |
| 免疫目标受到敌对负面 GE | 负面伤害 GE 被免疫组件阻止；玩家互伤不是正式验收路径 |
| 免疫 5 秒结束后再受击 | 正常扣除 Health |
| Host 使用三个技能 | 与 Client 使用时遵循相同服务器结算规则 |

控制台调试：

```text
showdebug abilitysystem
Net PktLag=150
Net PktLoss=5
```

建议分别测试 0ms、150ms、5% 丢包以及 150ms + 5% 丢包。

## 5. 当前边界

本文件最初记录的第一版缺口中，Enhanced Input、AbilitySet/HUD、死亡复活、ExecCalc、自定义 EffectContext、Vulnerability 堆叠和 GameplayCue 技术闭环已经在 M1～M5 完成。当前仍未包含：

- 正式技能图标、Niagara、音效和 Montage；现有 Cue 表现是零素材 PointLight 技术占位。
- 队友选择 UI；治疗当前仍是自我治疗。
- Energy 周期恢复以及更完整的 Blueprint GE 数值资产化。
- DamageIntent 的 token bucket/异常 strike、专用 Trace Channel 和历史回溯；当前只有 50ms 最小间隔与当前世界权威 Sweep。
- Host 反向输入、多轮丢包/更高延迟、非 Headless 视觉和持续 Cue 状态下死亡等补充矩阵。
- 功能级双客户端自动化、Dedicated Server、晚加入和打包验收。
- Network Insights 的带宽基线与同条件优化前后报告。

完整未完成项、优先级和完成口径见
[《Co-op GAS 架构与面试讲解手册》第 12.2 节](GAS_Architecture_Interview_Guide.md#122-完整未完成项矩阵)。

M5/M6/M6Intent 在扩展公式后完成回归：M5 `20260815_002532` 日志清点正常，M6 `20260815_002809` 为 95/95 PASS；此前 M6Intent 0ms `20260815_002959` 与双方各 `PktLag=150ms`（配置约 300ms RTT）`20260815_003155` 均为 52/52 PASS，追加 finite clamp/restart gate 后的最终二进制 0ms `20260815_004559` 再次为 52/52 PASS。服务器 TargetData 等待现有 5 秒超时收口，AbilityTask 会在数据到达、超时和 Task 结束路径清理委托/Timer，Damage Ability 也会在 `CommitAbility` 前验证权威目标、目标 ASC 和 DamageSpec。`TargetDataTimeout`、`SourceDead`、`InvalidTarget`、`CommitFailed` 仍缺专项双进程端到端分支；当前 Headless 证据也不等于正式视觉、loss 矩阵、服务器历史回溯或服务器+2 Clients 自动化已通过。详见 [M6 DamageIntent 安全验证报告](Evidence/GAS_M6_Damage_Intent_Security_Test_Report.md)与[阶段 3～4 证据报告](Evidence/Phase3_4_Victory_UI_Combat_Attributes_Report.md)。

## 6. 参考边界

架构阅读来源：

- `DruidMech/GameplayAbilitySystem_Aura`
- `CNGoSeI/GASAura`
- Unreal Engine 5.5 本地 GameplayAbilities 插件源码

由于两个参考仓库未提供明确开源许可证，本项目没有复制其实现文件、资源或项目专用命名。
