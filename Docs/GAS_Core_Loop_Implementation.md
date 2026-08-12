# Co-op GAS 核心闭环实施说明

> 分支：`coop-GAS`
>
> UE 版本：5.5
> 更新日期：2026-08-12

完整架构讲解、调用链、技术选型、问题复盘和面试场景题见：
[《Co-op GAS 架构与面试讲解手册》](GAS_Architecture_Interview_Guide.md)。本文件只维护实施状态和运行步骤。

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
- Health、MaxHealth、Energy、MaxEnergy 使用 RepNotify。
- IncomingDamage、IncomingHealing 是服务器结算用 Meta Attribute。
- 伤害、治疗和免疫均使用 `LocalPredicted` 激活。
- Cost 与 Cooldown 通过 GameplayEffect 预测并由服务器校正。
- 伤害目标通过自定义 AbilityTask 和 PredictionKey 上传服务器。
- 服务器重新验证目标类型、存活状态、距离和视线。
- 最终伤害和治疗只由服务器应用。
- 免疫使用持续 GameplayEffect、`State.Immune` 和 `UImmunityGameplayEffectComponent`。

## 2. 临时测试按键

当前不依赖 InputAction 资产，直接使用：

| 按键 | 能力 | 数值 |
|---|---|---|
| `4` | 攻击最近的另一名玩家 | 伤害 25、能量 10、冷却 1 秒、范围 600 |
| `5` | 自我治疗 | 治疗 30、能量 20、冷却 3 秒 |
| `6` | 状态免疫 | 持续 5 秒、能量 30、冷却 8 秒 |

后续创建 Enhanced Input 资产后，只替换输入绑定，不修改能力网络逻辑。

## 3. 伤害网络链路

```text
Owning Client 按下 4
-> ASC 根据 InputTag 找到 AbilitySpec
-> LocalPredicted 激活并预测 Cost/Cooldown
-> AbilityTask 在本地选择目标
-> FScopedPredictionWindow
-> ServerSetReplicatedTargetData
-> Server 根据 SpecHandle + ActivationPredictionKey 接收数据
-> 验证目标类型、距离、视线、ASC 和 Health
-> 服务器创建 Damage GameplayEffectSpec
-> 服务器写入 Data.Damage SetByCaller
-> IncomingDamage
-> PostGameplayEffectExecute
-> 扣除 Health
-> Health 复制到两个客户端
```

客户端不能提交伤害数值。上传的数据只有目标，伤害量来自服务器能力类默认值。

## 4. 双客户端手工验证

编辑器 PIE 设置：

1. Number of Players：`2`。
2. Net Mode：`Play As Listen Server`。
3. New Editor Window，两个窗口大小一致。
4. 确认当前 GameMode 没有在蓝图里覆盖 PlayerState Class；正确类型是 `multiplayerGASPlayerState`。
5. 两名玩家移动到 600 单位以内。

测试矩阵：

| 场景 | 预期结果 |
|---|---|
| Client 按一次 `4` | Server 对另一名玩家扣除 25 Health |
| 连续快速按 `4` | 1 秒冷却期间不能重复激活 |
| 目标超过 650 单位 | 服务器拒绝结算伤害 |
| 目标隔着阻挡 Visibility 的墙 | 服务器拒绝结算伤害 |
| 玩家按 `5` | Health 最多恢复到 MaxHealth |
| Energy 不足 | `CommitAbility` 失败，不产生技能结果 |
| 玩家 A 按 `6`，玩家 B 立即按 `4` | 负面伤害 GE 被免疫组件阻止 |
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

第一版尚未包含：

- 正式技能图标、Niagara、音效和 GameplayCue。
- GAS 属性 HUD；目前已暴露 PlayerState getter 和属性变化委托。
- 死亡、复活和 Energy 恢复。
- 队友选择 UI；治疗第一版是自我治疗。
- 正式 Enhanced Input、AbilitySet 与 Blueprint GE 数据资产接线。
- `ExecutionCalculation`、自定义 `GameplayEffectContext` 和 Buff/Debuff 堆叠规则。
- 预测拒绝/回滚可视化实验以及 GameplayCue 去重证据。
- 双客户端功能自动化、Dedicated Server、晚加入和打包验收。
- Network Insights 的带宽基线报告。

完整未完成项、优先级和完成口径见
[《Co-op GAS 架构与面试讲解手册》第 12.2 节](GAS_Architecture_Interview_Guide.md#122-完整未完成项矩阵)。

这些内容不阻塞对 ASC 所有权、预测激活、TargetData、服务器验证、Cost/Cooldown 和属性复制基础链路的第一轮验证；但在双窗口与弱网测试真正执行前，不能声称回滚体验或多人行为已经验收通过。

## 6. 参考边界

架构阅读来源：

- `DruidMech/GameplayAbilitySystem_Aura`
- `CNGoSeI/GASAura`
- Unreal Engine 5.5 本地 GameplayAbilities 插件源码

由于两个参考仓库未提供明确开源许可证，本项目没有复制其实现文件、资源或项目专用命名。
