# UE5.5 GAS 与网络预测深度路线

> 更新日期：2026-08-10
>
> 项目：`E:\ueprojrct\multiplayer`
>
> 目标：从零实现可双人测试的 GAS 单局 Demo，并达到能够解释预测、服务器校验、复制和网络优化的面试深度。

## 1. 路线结论

本项目不复刻完整 Aura，也不把已经稳定的压力板、门、钥匙强行迁移到 GAS。执行策略是：

1. 冻结并验收当前 Co-op Puzzle Demo。
2. 在独立开发分支和独立战斗关卡中接入 GAS。
3. 从零实现伤害、治疗、状态免疫三个能力。
4. 把重点放在客户端预测、服务器拒绝、回滚、TargetData 校验和弱网一致性。
5. 使用 Network Insights 等工具保留优化前后数据。
6. 输出源码分析、测试报告、演示视频和可复现步骤。

预计投入：

| 投入方式 | 预计周期 |
|---|---:|
| 每周 15～20 小时 | 5～7 个月 |
| 每周 35～40 小时 | 10～14 周 |
| 总有效投入 | 350～500 小时 |

计划按 **20 周开发 + 4 周缓冲** 执行。缓冲用于 UE5.5 API 差异、反射/蓝图重建、动画资源、预测 Bug 和弱网回归。

## 2. 当前基线与边界

### 2.1 已有基础

当前 Co-op 项目已经覆盖：

- Listen Server / Client 双人运行框架。
- Session 创建、搜索、加入、销毁和 Travel。
- Authority、Ownership、Server/Client/Multicast RPC。
- 属性复制、RepNotify、GameMode / GameState 分工。
- 服务端权威压力板、门、平台、钥匙和胜利判定。
- 双人共享目标、胜利 UMG、重新开始服务端请求。
- Git LFS 资源管理和双窗口测试脚本。

这些能力可以直接复用到 GAS 网络测试，不需要重新开发房间系统。

### 2.2 GAS 当前状态

GAS 目前只完成方案设计，尚未接入：

- `multiplayer.Build.cs` 尚未依赖 `GameplayAbilities`、`GameplayTags`、`GameplayTasks`。
- `multiplayer.uproject` 尚未启用 GameplayAbilities 插件。
- 尚无 ASC、AttributeSet、GameplayAbility、GameplayEffect 和 GameplayCue 实现。
- 尚无 PredictionKey、TargetData 和 Replication Mode 实测结果。

准确表述是：

> 已具备 UE5 双人网络和服务器权威 Gameplay 基础；GAS、预测和网络优化属于下一阶段。

### 2.3 不在首版范围内

首版 GAS Demo 暂不实现：

- Aura 完整存档、任务、装备和大规模 RPG 数值系统。
- 完整 Lyra Experience / GameFeature 框架。
- MMO 级后端、跨服和无缝迁移。
- 完整反作弊产品。
- 为了展示 GAS 而重写成熟机关。

先完成可解释、可验证的最小能力闭环，再决定是否扩展 AI、存档或装备。

## 3. 最终项目结构

```text
主菜单
├─ Co-op Puzzle Demo
│  ├─ 压力板、门和移动平台
│  ├─ 四把钥匙和共享目标
│  └─ 双人胜利区
│
└─ GAS Co-op Arena
   ├─ 伤害技能
   ├─ 治疗技能
   ├─ 状态免疫技能
   ├─ 双人技能配合机关
   ├─ 预测拒绝实验
   └─ 弱网与压力测试
```

推荐源码目录：

```text
Source/multiplayer/
├─ AbilitySystem/
│  ├─ CoopAbilitySystemComponent
│  ├─ CoopAttributeSet
│  ├─ CoopGameplayAbility
│  ├─ CoopAbilitySet
│  ├─ CoopGameplayTags
│  └─ CoopAbilitySystemLibrary
├─ Abilities/
│  ├─ GA_Damage
│  ├─ GA_Heal
│  ├─ GA_Immunity
│  └─ Tasks/
├─ Effects/
│  ├─ ExecCalc_Damage
│  ├─ MMC_Cooldown
│  └─ CoopGameplayEffectContext
├─ Player/
│  ├─ CoopPlayerState
│  ├─ CoopCharacter
│  └─ CoopPlayerController
└─ UI/
   ├─ AttributeViewModel
   └─ AbilityDebugWidget
```

核心对象关系：

```text
PlayerState
├─ AbilitySystemComponent
└─ AttributeSet

Character
├─ 作为 ASC 的 AvatarActor
├─ 实现 IAbilitySystemInterface 或转发 ASC
└─ 将 Enhanced Input 转换为 InputTag

AbilitySet DataAsset
├─ 授予 GameplayAbility
├─ 应用初始 GameplayEffect
├─ 添加 AttributeSet
└─ 保存 GrantedHandles 供统一撤销
```

ASC 首选放在 PlayerState。这样死亡和换 Pawn 后能力状态可以保留，但必须正确处理 OwnerActor、AvatarActor、
网络所有权链和 PlayerState 更新频率。

## 4. GAS 基础阶段

### 4.1 模块和插件

启用：

```text
GameplayAbilities
GameplayTags
GameplayTasks
```

构建门禁：

- Game 与 Editor Development 目标编译通过。
- UHT 可以生成所有反射类型。
- 两个客户端启动时无重复 ASC 初始化日志。

### 4.2 ASC 初始化

实现：

```text
Server: Character::PossessedBy
Client: Character::OnRep_PlayerState
→ 获取 PlayerState ASC
→ InitAbilityActorInfo(PlayerState, Character)
→ 初始化输入与本地 UI 监听
```

必须能解释：

- OwnerActor 与 AvatarActor 的区别。
- GAS Ability Owner 与 Actor 网络 Ownership 的区别。
- 为什么 Client-to-Server Ability 激活依赖本地所有权链。
- 为什么 PlayerState ASC 需要在 Server 和 Client 分别初始化。
- 换 Pawn 后为什么只刷新 Avatar，不能重复授予全部技能。

### 4.3 AttributeSet

首版属性：

```text
Health
MaxHealth
Mana
MaxMana
Armor
IncomingDamage（Meta Attribute）
```

必须实现：

- `PreAttributeChange`。
- `PostGameplayEffectExecute`。
- Health / Mana Clamp。
- 属性 RepNotify。
- `GAMEPLAYATTRIBUTE_REPNOTIFY`。
- 默认属性 GameplayEffect。
- 死亡 GameplayTag。

验收：

- 服务器修改属性后 Host 与 Client UI 一致。
- Client 不能直接决定 Health 最终值。
- 晚加入能得到当前属性和状态标签。
- 重生后没有重复 AttributeSet 或重复 Delegate。

## 5. 三个技能的网络设计

### 5.1 伤害技能

推荐实现可预测施法、服务器伤害的射线或投射技能：

```text
拥有客户端预测激活
→ 本地播放 Montage / Cue
→ 本地采集 TargetData
→ 服务器验证距离、视线、目标、阵营和技能状态
→ ExecCalc_Damage
→ 修改 IncomingDamage
→ PostGameplayEffectExecute 扣除 Health
→ 属性复制到相关客户端
```

边界：

- 可以预测能力激活、输入响应和本地表现。
- 可以按设计预测自身 Cost / Cooldown。
- 最终伤害、暴击、击杀和掉落由服务器决定。
- ExecutionCalculation 只把服务器结果当作权威结果。
- 客户端提供的 Damage Magnitude 不能直接相信。

### 5.2 治疗技能

```text
客户端选择队友
→ 预测激活、Mana Cost 和 Cooldown
→ 发送 TargetData
→ 服务器验证队伍、距离、视线、存活状态和资源
→ 应用 Heal GameplayEffect
→ Health 属性复制
```

服务器拒绝用例：

- 目标超出距离。
- Mana 不足。
- 冷却未结束。
- 目标已经死亡。
- 目标不是队友。
- TargetData 已经过期或与当前世界状态不一致。

### 5.3 状态免疫技能

第一版实现自身免疫：

```text
GA_Immunity
→ Duration GameplayEffect
→ Granted Tag: State.Immune
→ 阻挡指定 Damage / Control Effects
→ Persistent GameplayCue
```

第二版扩展为队友免疫：

```text
玩家 A 为玩家 B 施加免疫
→ 服务器验证目标和距离
→ 玩家 B 获得 State.Immune
→ 玩家 B 穿过伤害区域或解除控制机关
→ 两人完成协作目标
```

队友免疫是 Demo 的原创玩法重点。它把 GAS 状态系统和现有 Co-op 关卡结合起来，但不重写压力板和门。

## 6. PredictionKey 深度路线

### 6.1 源码阅读顺序

以本地 UE5.5 引擎源码为最终依据，按顺序阅读：

```text
GameplayPrediction.h
GameplayAbility.cpp
AbilitySystemComponent.cpp
AbilitySystemComponent_Abilities.cpp
GameplayEffect.cpp
ActiveGameplayEffects.cpp
GameplayEffectTypes.cpp
AbilityTask.cpp
GameplayCueManager.cpp
```

每个文件必须输出笔记，并回答：

1. PredictionKey 在哪里创建和复制？
2. 为什么只有拥有客户端可以进行正常预测？
3. 服务器怎样确认或拒绝一次预测？
4. Reject 后哪些修改能够回滚，哪些不能自动回滚？
5. Catch-up Delegate 在什么情况下执行？
6. 为什么旧 PredictionKey 不能用于后续独立操作？
7. Scoped Prediction Window 解决什么问题？
8. Dependent Prediction Key 如何组织连续预测？
9. AbilityTask 如何继承并使用当前预测上下文？
10. 预测 GameplayCue 为什么可能重复或残留？

### 6.2 Prediction Lab

创建专门的 `GA_PredictionLab`：

```text
可切换 Local Predicted / Server Only
可切换 Mana Cost
可切换 Cooldown
可切换 GameplayEffect
可让服务器强制接受或拒绝
可配置服务器延迟响应
输出 PredictionKey、Role、ActivationMode 和失败标签
```

实验矩阵：

| 场景 | 检查内容 |
|---|---|
| 0ms | 基础调用链和日志 |
| 100ms | 本地响应与服务器确认 |
| 200ms | 预测和非预测体验差异 |
| 5% 丢包 | Reliable 确认延迟和最终一致性 |
| 10% 丢包 | 重试、堆积和技能失败表现 |
| 服务器拒绝 | Cost、Cooldown、Cue 和 UI 回滚 |
| 连续快速输入 | PredictionKey 是否错误复用 |
| 中途取消 | Client / Server 是否一致结束 |

### 6.3 可预测边界

| 内容 | 首版策略 |
|---|---|
| Ability 激活 | Local Predicted |
| 自身 Mana Cost | 预测并等待服务器校正 |
| Cooldown 开始 | 预测并等待服务器确认 |
| 本地 Montage | 预测 |
| 本地 GameplayCue | 可预测，但必须测试拒绝清理 |
| 对敌人的最终伤害 | 服务器决定 |
| ExecCalc | 服务器权威 |
| 掉落、奖励、胜负 | 服务器决定 |
| 随机暴击 | 默认服务器决定 |
| 对其他玩家的强制状态 | 服务器验证并应用 |

## 7. 网络优化实验

网络优化必须遵循：

```text
建立基线
→ 修改一个变量
→ 保存 Network Insights / 日志数据
→ 对比正确性、延迟和带宽
→ 决定是否保留
```

### 7.1 ASC Replication Mode

比较 `Full`、`Mixed`、`Minimal`：

- 玩家 ASC 重点验证 `Mixed`。
- AI ASC 重点验证 `Minimal`。
- 比较 Owner 与 Simulated Proxy 收到的 Ability、Effect、Tag、Attribute 和 Cue。
- 验证晚加入、死亡、换 Pawn 和断线后的状态。

记录：

- 每秒入站/出站字节。
- 每次技能激活的包和 RPC 数量。
- Active GameplayEffect 数量。
- Owner 和 Simulated Proxy 的复制差异。
- 晚加入恢复耗时。

### 7.2 输入复制

首版保持：

```cpp
bReplicateInputDirectly = false;
```

优先使用 InputTag、AbilityLocalInputPressed / Released 和 GAS Generic Replicated Events。对比普通 Reliable
Server RPC、GameplayEvent 和 InputTag 激活的调用与网络成本。

### 7.3 GameplayEffect 数量与堆叠

实验：

```text
20 个独立 Duration GE
vs
1 个可堆叠 GE，StackCount = 20
```

再比较高频 Periodic GE 与服务器汇总后低频应用 GE。记录 Active GE、FastArray 增量、带宽、服务器时间和 UI
回调次数。优化目标不是“少用 GE”，而是在状态语义、预测能力和复制成本之间取得平衡。

### 7.4 GameplayCue

GameplayCue 只负责音画表现，不承担最终 Gameplay 状态。测试：

- GE 自动触发 Cue。
- 本地预测 Cue。
- Server Cue。
- Persistent Cue。
- Cue Actor 创建/销毁和资源预加载。
- 预测被拒绝后的 Cue 清理。
- 晚加入是否恢复持续 Cue。

记录重复播放、残留对象、Spawn 压力、内存和网络事件数量。

### 7.5 TargetData 安全与流量

服务器重新验证：

```text
目标存在且仍存活
目标位于合法距离
目标没有被墙遮挡
目标阵营合法
能力仍处于有效状态
TargetData 属于本次 Ability / PredictionKey
```

优化：

- 不发送无关字段。
- 位置和方向使用可量化类型。
- 不每帧发送目标更新。
- 确认时只提交一次最终数据。
- 不接受客户端提供的最终伤害数值。

### 7.6 PlayerState 更新频率

ASC 放在 PlayerState 后，比较：

- 默认 NetUpdateFrequency。
- 提高 NetUpdateFrequency。
- Adaptive Net Update Frequency。
- Ability 激活时 ForceNetUpdate。

必须同时记录属性到达延迟和带宽；单纯提高频率不算优化。

### 7.7 压力场景

除两个真实玩家外，增加 10～20 个带 ASC 的测试 AI，制造：

- 大量 Attribute 变化。
- 多个 Duration / Periodic GE。
- GameplayCue 并发。
- Ability 激活和取消。
- 目标选择与伤害事件。

用于判断优化是否只在“两个人几乎没有负载”的情况下成立。

## 8. Lyra 架构吸收

只吸收与本项目直接相关的部分：

1. `ULyraAbilitySet`。
2. GrantedHandles 生命周期。
3. InputTag 激活。
4. Ability Tag Relationship Mapping。
5. Activation Policy。
6. PlayerState ASC。
7. Equipment 动态授予能力。
8. Gameplay Message / UI 解耦。

实现精简版：

```cpp
USTRUCT()
struct FCoopAbilitySetGrantedHandles
{
    TArray<FGameplayAbilitySpecHandle> AbilityHandles;
    TArray<FActiveGameplayEffectHandle> EffectHandles;
    TArray<TObjectPtr<UAttributeSet>> AttributeSets;

    void TakeFromAbilitySystem(UCoopAbilitySystemComponent* ASC);
};
```

不复制 Lyra 类名和完整 Experience / GameFeature 框架。必须能解释为什么保留或删减每一层。

参考：

- [Epic：GAS Overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/understanding-the-unreal-engine-gameplay-ability-system)
- [Epic：Abilities in Lyra](https://dev.epicgames.com/documentation/unreal-engine/abilities-in-lyra-in-unreal-engine)
- [tranek/GASDocumentation](https://github.com/tranek/GASDocumentation)
- [DruidMech/GameplayAbilitySystem_Aura](https://github.com/DruidMech/GameplayAbilitySystem_Aura)
- [CNGoSeI/GASAura](https://github.com/CNGoSeI/GASAura)

GASDocumentation 主要对应 UE5.3；本项目是 UE5.5，版本冲突时以本地引擎源码和实际编译结果为准。

## 9. 测试工具与指标

工具：

```text
stat net
stat abilitysystem
showdebug abilitysystem
AbilitySystem.Debug.NextTarget
Unreal Insights
Network Insights
Net PktLag
Net PktLoss
Net PktOrder
Net PktDup
```

每轮保存：

| 指标 | 基线 | 优化后 |
|---|---:|---:|
| 两玩家待机带宽 | 待测 | 待测 |
| 一次伤害技能发送字节 | 待测 | 待测 |
| 一次治疗技能发送字节 | 待测 | 待测 |
| 一次免疫技能发送字节 | 待测 | 待测 |
| RPC/s | 待测 | 待测 |
| Active GE 数量 | 待测 | 待测 |
| GameplayCue Actor 数量 | 待测 | 待测 |
| 200ms 下本地响应时间 | 待测 | 待测 |
| 服务器拒绝后的回滚时间 | 待测 | 待测 |
| 5% 丢包下最终错误率 | 待测 | 待测 |
| 10～20 AI 下服务器帧时间 | 待测 | 待测 |

不能只记录平均值。至少保存测试持续时间、样本次数、最大值、P95、网络参数、机器配置和对应提交号。

## 10. 20+4 周执行计划

| 周数 | 工作 | 阶段产物 |
|---|---|---|
| 1～2 | 当前 Co-op V1 收口 | 双端日志、视频、稳定标签 |
| 3～4 | GAS 插件、ASC、PlayerState、AttributeSet | 两端 ASC 初始化与属性复制 |
| 5～6 | GE、Attribute RepNotify、Damage ExecCalc | 基础属性与伤害测试 |
| 7～8 | 伤害和治疗能力 | Cost、Cooldown、TargetData |
| 9～10 | 状态免疫和双人配合 | State.Immune 与协作关卡 |
| 11～12 | PredictionKey 与拒绝回滚实验 | Prediction Lab 与时序日志 |
| 13～14 | TargetData 校验和作弊测试 | 服务器验证矩阵 |
| 15～16 | Replication Mode、GE、Cue 优化 | Network Insights 对比 |
| 17 | Lyra AbilitySet 与 InputTag | 精简 AbilitySet 架构 |
| 18 | 晚加入、死亡、重生、断线和弱网 | 回归报告 |
| 19 | 源码分析和技术文章 | 至少 8～12 篇笔记 |
| 20 | README、视频、简历和面试题 | 可展示版本 |
| 21～24 | 缓冲和高风险问题 | 修复与最终验收 |

## 11. 阶段门禁

### Gate A：基础 GAS

- Host / Client ASC 初始化正确。
- 属性、Tag、GE 复制正确。
- 重生不重复授予。
- 晚加入状态一致。
- UI 没有重复 Delegate。

### Gate B：三个技能

- 伤害、治疗、免疫全部可用。
- 两端表现一致。
- Client 不能伪造最终结果。
- Cost / Cooldown 正确。
- Ability 正常 End / Cancel，没有残留 Task。

### Gate C：预测

- 200ms 下有即时本地反馈。
- Server Reject 后能够回滚。
- 没有重复 Cue、重复扣费或残留 Cooldown。
- PredictionKey 日志和调用链可以解释。

### Gate D：网络优化

- 有优化前后数据。
- 能解释带宽和延迟变化原因。
- 能解释 Replication Mode 的取舍。
- 5% / 10% 丢包下最终状态一致。
- 10～20 AI 压力下没有明显 GE/Cue 生命周期泄漏。

### Gate E：简历发布

- 项目和类型不使用教程原名。
- 有原创双人免疫协作玩法。
- 有 GAS 源码分析文档。
- 有双端弱网演示视频。
- 有可复现测试步骤和测试参数。
- 有一项以上带数据的网络优化。
- 能脱离代码讲清一次预测 Ability 的完整调用链。

## 12. 每周文档输出

每周至少完成一篇：

1. ASC 初始化与 Owner / Avatar。
2. GameplayEffectSpec 生命周期。
3. Attribute Aggregator。
4. ActiveGameplayEffects 与 FastArray。
5. PredictionKey 创建、确认与拒绝。
6. Prediction Reject 和回滚边界。
7. AbilityTask 生命周期。
8. TargetData 复制与校验。
9. GameplayCue 复制策略。
10. Replication Mode 对比。
11. Lyra AbilitySet。
12. 弱网测试和优化数据。

每篇使用统一格式：

```text
问题
→ 关键类型和源码入口
→ 完整调用链
→ 最小复现实验
→ Host / Client 日志
→ 弱网结果
→ 设计取舍
→ 当前仍未知的问题
```

## 13. 最终简历表述目标

完成 Gate E 后可以表述为：

> 在 UE5.5 中从零搭建服务器权威、支持客户端预测的双人 Gameplay Ability System，实现伤害、治疗与
> 状态免疫能力，完成 TargetData 二次校验、PredictionKey 拒绝回滚、AbilitySet 数据驱动授予以及
> GameplayEffect / GameplayCue / Replication Mode 网络优化，并通过 100/200ms 延迟、5%/10% 丢包与
> 多 ASC 压力场景的 Network Insights 数据验证结果。

在 Gate E 之前，只描述已经通过测试和有日志证据的部分，不把计划、阅读和未验证蓝图写成已完成成果。
