# GAS M4 ExecutionCalculation、EffectContext 与堆叠验收报告

> 分支：`coop-GAS`
>
> 日期：2026-08-13

## 1. 伤害公式

服务器 `UmultiplayerDamageExecution` 读取：

- `Data.Damage` SetByCaller 基础伤害。
- 目标当前 Health / MaxHealth。
- 目标 `State.Vulnerable` 活跃效果聚合层数。

公式：

```text
FinalDamage
= max(BaseDamage, 0)
* (Health <= 50% ? 1.5 : 1.0)
* (1.0 + clamp(VulnerabilityStacks, 0, 3) * 0.1)
```

暴击条件是服务器可重复计算的“目标处于半血或以下”，不是客户端上传布尔值，也不是无法复现的客户端随机数。

## 2. 自定义 GameplayEffectContext

`FmultiplayerGameplayEffectContext` 实现：

- `GetScriptStruct()`：保证 GAS 知道真实派生结构。
- `Duplicate()`：深拷贝 HitResult。
- `NetSerialize()`：复制 Critical、HitType 和量化 ImpactImpulse，并复用父类 Context 序列化。
- `UmultiplayerAbilitySystemGlobals::AllocGameplayEffectContext()`：所有项目 GE 默认分配自定义 Context。

真实消费者：

- ExecCalc 写入 Critical、HitType、ImpactImpulse。
- AttributeSet 读取并输出 `GAS_DAMAGE_CONTEXT`，与最终 IncomingDamage 关联。
- M5 GameplayCue 已消费这些字段：选择普通/暴击样式、记录 HitType，并用归一化 ImpactImpulse 方向偏移占位命中灯；尚未把它当作正式物理力。

## 3. Vulnerability 堆叠策略

| 规则 | 当前值 | 原因 |
|---|---:|---|
| 聚合主体 | AggregateByTarget | 两名合作玩家共同叠加同一目标状态 |
| 层数上限 | 3 | 控制增伤上限为 30% |
| 单层持续 | 8 秒 | 给合作攻击留出窗口 |
| 重复施加 | 成功叠层刷新持续时间 | 连续配合保持窗口 |
| 第 4 次施加 | 拒绝溢出 | 层数和增伤都不超过上限 |
| 到期 | ClearEntireStack | 行为简单、可解释 |
| 死亡清理 | 移除 Vulnerability | 不把旧生命周期 Debuff 带到复活后 |

Damage 先执行，再施加一层 Vulnerability，因此第一击不享受自己刚添加的层；下一击才消费现有层数。

## 4. 自动化矩阵

| 编号 | 检查 | 状态 |
|---|---|---|
| M4-FORM-01 | 满血、0 层：25 -> 25 | 自动化通过 |
| M4-FORM-02 | 满血、3 层：25 -> 32.5 | 自动化通过 |
| M4-FORM-03 | 半血、2 层：25 -> 45 | 自动化通过 |
| M4-STACK-01 | AggregateByTarget、上限 3、刷新、整组到期 | 自动化通过 |
| M4-CTX-01 | Globals 分配自定义 Context | 自动化通过；运行时类为 `multiplayerAbilitySystemGlobals` |
| M4-CTX-02 | Critical/HitType/Impulse NetSerialize 往返 | 自动化通过 |

运行命令：`Automation RunTests multiplayer.GAS.Configuration`。2026-08-13 运行结果为 1/1 成功，退出码 0。

### M4-CONFIG-001：自定义 EffectContext 类已编译但运行时未启用（历史问题，已修复）

- 现象：首轮测试中 `AllocGameplayEffectContext()` 仍返回引擎默认 Context。
- 排除项：类路径可反射、Editor/Game 均能链接，因此不是 UCLASS 命名或模块链接失败。
- 定位工具：自动化断言同时打印 DeveloperSettings 路径和运行时 Globals 类。
- 根因：当前 UE 5.5 启动链中，ini 内的 DeveloperSettings 声明没有在 Globals 首次获取前进入对应 CDO。
- 解决：主游戏模块启动时显式写入项目 Globals 类和 Cue 路径，再由 GameplayAbilities 延迟创建单例；ini 仍保留项目设置声明。
- 保护：自动化同时验证设置路径、运行时类和实际分配的 Context，避免只验证其中一层。
- 结果：日志为 `GlobalsPath=/Script/multiplayer.multiplayerAbilitySystemGlobals RuntimeClass=multiplayerAbilitySystemGlobals`，测试通过。

## 5. 双窗口运行矩阵

| 编号 | 操作 | 预期 | 状态 |
|---|---|---|---|
| M4-DMG-01 | 连续命中 TargetDummy | Host 输出 Base、层数、Critical、Final；Client Health 与服务器一致 | Run `20260813_021312` 通过；伤害为 25/27.5/45/48.8，Client Health 对应 75/47.5/2.5/0 |
| M4-STACK-02 | 3 次内连续命中 | Vulnerability 依次到 1/2/3 层 | Run `20260813_021312` 通过 |
| M4-STACK-03 | 第 4 次施加 | 不超过 3 层 | Run 中最高观察到 3 层，目标随后死亡；独立“存活目标溢出”仍待验证 |
| M4-STACK-04 | 等待 8 秒 | State.Vulnerable 移除 | 后续 M5 Run `20260813_131659` 回归通过；第二个生命周期约 7.85 秒后双端各 Removed 一次 |
| M4-IMM-01 | 玩家 Immune 时接受敌人 Damage GE | 负面 GE 被免疫，Health 不变 | M3 窗口同步验证 |

原始运行证据：`Saved/GASBaseline/20260813_021312/Host.log` 与 `Client.log`。该轮本身没有覆盖 8 秒自然到期；后续 M5 运行 `20260813_131659` 和弱网运行 `20260813_132026` 已补充生命周期清理证据，详见 [M5 报告](GAS_M5_GameplayCue_Prediction_Test_Report.md)。

## 6. 当前不足

- 暴击目前是确定性低血量规则，不包含装备属性、抗性或随机种子。
- M5 已让 GameplayCue Handler 消费 Critical、HitType 与 ImpactImpulse：选择暴击样式、记录命中类型并按方向偏移占位命中灯。它仍不是正式物理冲量；施加物理力需要独立做强度限制和服务器规则设计。
- 尚无可视化 Debuff 层数 Widget，基础 HUD 只显示是否 Vulnerable。
- 晚加入时堆叠状态仍需双端专门验收。
