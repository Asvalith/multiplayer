// Copyright Epic Games, Inc. All Rights Reserved.

// 声明项目 ASC、技能公共约束、具体能力与默认效果；输入和 HUD 不在此处执行玩法。
// 实现分别在 AbilitySystem.cpp（GAS 集成）和 Ability.cpp（技能阶段及效果默认值）。

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Combat/multiplayerCombatConfig.h"
#include "multiplayerCombatAbility.generated.h"

class AmultiplayerCombatCharacter;

// UE5.5禁止客户端任意移除权威GE；只开放“结束本次本地施法”的窄入口，不开放资源/CD移除。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatAbilitySystem : public UAbilitySystemComponent
{
	GENERATED_BODY()
public:
	// 查询集中关系，不提交资源/目标；所属端预检和权威能力准入共用同一份规则。
	bool CheckAbilityTagRelationships(const FGameplayTagContainer& AbilityTags, FGameplayTagContainer* Reasons = nullptr) const;
	// 只扩展 GAS 原生阻断/取消参数；开始和结束的引用计数仍由 Super 成对管理。
	virtual void ApplyAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags, UGameplayAbility* RequestingAbility,
		bool bEnableBlockTags, const FGameplayTagContainer& BlockTags, bool bExecuteCancelTags,
		const FGameplayTagContainer& CancelTags) override;
	// 服务器按句柄、拥有者预测端按本次预测键解除施法锁，绝不移除其他施法或冷却。
	void EndCasting(FActiveGameplayEffectHandle ServerHandle, const FPredictionKey& ActivationKey);
	// 重连恢复只调整新效果的下一跳时刻，沿用 GE 自己的 TimerHandle 和清理链路。
	void SetPeriodicEffectDelay(FActiveGameplayEffectHandle Handle, float FirstDelay);
	// 服务器把正式拒绝原因发给技能拥有者，不用一次新的资格查询代替真实失败结果。
	virtual void NotifyAbilityFailed(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability,
		const FGameplayTagContainer& FailureReason) override;
	// UE5.5 的动画开始通知早于 ASC 认领；在这里统一处理同步取消/替换，能力不各自补停。
	virtual float PlayMontage(UGameplayAbility* AnimatingAbility, FGameplayAbilityActivationInfo ActivationInfo,
		UAnimMontage* Montage, float InPlayRate, FName StartSectionName = NAME_None, float StartTimeSeconds = 0.f) override;
protected:
	// 远程定向技能必须随激活提交瞄准；服务器 AI 的直接激活不经过此 RPC 入口。
	virtual void InternalServerTryActivateAbility(FGameplayAbilitySpecHandle Handle, bool InputPressed,
		const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData) override;
private:
	// 只标识动画播放请求，不替代能力激活版本或网络预测键。
	uint32 MontagePlayRevision = 0;
};

// 五个战斗技能共用 GAS 本地预测、连接接纳与死亡限制；最终玩法结果仍由服务器决定。
// 静态冲突关系在 CombatConfig，GAS 继续管理每次激活的真实生命周期。
UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerCombatAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	// 配置实例复用和本地预测；各技能构造函数再声明自身分类及效果。
	UmultiplayerCombatAbility();
	// 返回固定技能分类，供配置查询及失败反馈识别能力，不表示当前激活阶段。
	EMultiplayerCombatSkill GetCombatSkill() const { return Skill; }
	// 先查连接与角色业务资格，再交 GAS 检查标签、成本和冷却；查询不修改状态。
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 校验定向事件的数据形状及有限、归一化朝向；不接收客户端伤害或目标裁决。
	virtual bool ShouldAbilityRespondToEvent(const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayEventData* Payload) const override;
	// 原生按标签筛选/调试查询也必须看到中央关系，不能只在最终激活入口额外判断。
	virtual bool DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* Reasons = nullptr) const override;
protected:
	// 所有技能的资源与冷却从同一份规则读取；效果继续交给 GAS 预测与确认。
	virtual bool CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 保留 GAS 冷却查询，只补充项目可显示的失败原因。
	virtual bool CheckCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	// 独立成本提交入口读取当前有效规则，最终修改仍经预测 GE 执行。
	virtual void ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	// 独立冷却提交入口使用规则时长，不自行维护冷却计时器。
	virtual void ApplyCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	// CommitCheck 放行后，用同一份局部规则先扣费再挂冷却；回调返回时检查本轮是否仍有效。
	virtual void CommitExecute(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) override;
	// 读取角色结合成长后的有效数值；不是跨阶段固定的完整技能快照。
	FmultiplayerCombatSkillRule GetRule() const;
	EMultiplayerCombatSkill Skill = EMultiplayerCombatSkill::Attack;
	// InstancedPerActor 会复用实例；同步取消后再激活时，旧调用栈不能继续推动新一轮技能。
	uint32 ActivationRevision = 0;
private:
	// CommitExecute 用同一份局部规则提交成本和冷却，不把提交快照保存成跨激活成员。
	void ApplyRuleCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const;
	// 按调用者提供的规则挂冷却，保证一次 Commit 不会混用扣费前后的等级数值。
	void ApplyRuleCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FmultiplayerCombatSkillRule& Rule) const;
};

// 攻击：伤害效果、冷却和技能放在一起。
// 以下效果提供可运行的默认规则，蓝图派生类可调整数值和持续时间。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 伤害由服务器通过 Data.Damage 写入本次效果。
	UmultiplayerDamageEffect();
};

// 护盾是独立持续属性；获得护盾使用 GE，承伤顺序由同一属性集负责。
UCLASS()
class MULTIPLAYER_API UmultiplayerShieldEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 声明瞬时护盾增量；剩余护盾由属性集管理。
public: UmultiplayerShieldEffect();
};

// 同一来源刷新持续时间但不推迟下一跳，不同来源各自保留效果句柄。
UCLASS()
class MULTIPLAYER_API UmultiplayerBurningEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置服务端周期伤害及按来源刷新规则。
public: UmultiplayerBurningEffect();
};

// 禁锢与眩晕共用禁行权限，但禁锢不会取消攻击或治疗。
UCLASS()
class MULTIPLAYER_API UmultiplayerRootEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置禁锢和共享禁行标签；每份效果独立持有自己的标签引用。
public: UmultiplayerRootEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerAttackCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 普通攻击的默认冷却为半秒；实际提交时长由技能有效规则覆盖。
	UmultiplayerAttackCooldownEffect();
};

// 普通攻击：提交冷却后生成服务器投射物，伤害在命中时结算。
UCLASS()
class MULTIPLAYER_API UmultiplayerAttackAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明普通攻击分类、事件及默认成本／冷却效果。
	UmultiplayerAttackAbility();

protected:
	// 固定本轮瞄准，验证枪口后提交，并等待出手；只有服务器生成真实弹体。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	// 只结束匹配预测键的激活；取消时停止本能力拥有的动作，任务交 GAS 释放。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 起手回调先认领本次出手，再由服务器发弹；重复回调不会重复生成。
	UFUNCTION() void ReleaseProjectile();
	// 收招结束本地或权威能力；客户端正常结束不提前裁掉服务器起手。
	UFUNCTION() void FinishAttack();
	FRotator AimRotation = FRotator::ZeroRotator;
	bool bProjectileReleased = false;
};

// 小兵近战：行为树提供目标，GAS 管理起手、一次命中和收招；客户端只接收动作与结果。
UCLASS()
class MULTIPLAYER_API UmultiplayerEnemyMeleeAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	// 小兵近战仅在服务器运行，不复用玩家的远程投射物流程。
	UmultiplayerEnemyMeleeAbility();
	// 在公共准入外检查黑板目标、距离和遮挡，不在查询阶段消费资源。
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
protected:
	// 固定目标、提交并挂施法锁，等待本轮近战出手。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 先结束任务再解开自己的施法锁；结束中的保护避免行为树同步重启时污染下一轮。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 出手时重新验证服务器目标，只命中一次，挥空也进入收招。
	UFUNCTION() void ReleaseMelee();
	// 完成服务器收招，让行为树按技能结果继续决策。
	UFUNCTION() void FinishMelee();
	TWeakObjectPtr<AmultiplayerCombatCharacter> AttackTarget;
	FActiveGameplayEffectHandle CastingEffect;
	bool bReleased = false;
	bool bEndingMelee = false;
};

// 所有技能共用同一种能量成本；金额由本次提交的 Data.Cost 提供。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerEnergyCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerEnergyCostEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 配置防御冷却标签和默认八秒时长，提交时可按有效规则覆盖。
	UmultiplayerDefendCooldownEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendingEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 默认三秒内授予防御标签，正面完全格挡由属性结算层执行；实际时长由技能填入。
	UmultiplayerDefendingEffect();
};

// Q 的短弹反窗口独立于较长格挡效果；结束时只移除自己的标签引用。
UCLASS()
class MULTIPLAYER_API UmultiplayerParryEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置独立于较长防御状态的短弹反窗口。
public: UmultiplayerParryEffect();
};

// 防御：由限时 GameplayEffect 授予正面格挡状态，重复施放不会叠加。
// 客户端先预测消耗、冷却和自身防御；服务器拒绝时由同一个预测键撤销这些效果。
UCLASS()
class MULTIPLAYER_API UmultiplayerDefendAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明防御分类、定向事件；能量成本共用，冷却标签独立。
	UmultiplayerDefendAbility();

protected:
	// 提交后授予防御和短弹反，结束当次能力但保留限时 GE；可选护盾仅服务器添加。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};

// 治疗：治疗效果、消耗、冷却和技能。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 读取本次 Data.Heal 修改血量，属性集负责限制上限与禁止复活。
	UmultiplayerHealEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 配置治疗冷却标签和默认六秒时长，提交时可按有效规则覆盖。
	UmultiplayerHealCooldownEffect();
};

// 治疗：选择附近同阵营最低血量比例的存活角色；没有合法目标时不扣资源。
UCLASS()
class MULTIPLAYER_API UmultiplayerHealAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	// 声明治疗分类和默认效果；治疗直接激活，不接受客户端指定治疗对象。
	UmultiplayerHealAbility();
	// 公共准入通过后查找合法治疗对象，使预览和正式激活共用同一条件。
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	// 服务器先确认可治疗对象再提交，扣费通知后重新选目标，统一通过 GE 治疗。
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};

// 范围技能的默认冷却标签；实际时长在提交时由有效规则覆盖。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置默认范围冷却时长和标签。
public: UmultiplayerAreaCooldownEffect();
};
// 施法锁是跟随能力生命周期的无限 GE，不靠独立倒计时提前解锁。
UCLASS(NotBlueprintable)
class MULTIPLAYER_API UmultiplayerCastingEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置施法标签及表现，具体能力保存句柄并在结束时解除。
public: UmultiplayerCastingEffect();
};
// 同一目标只保留一份眩晕，再次施加刷新时长，不能叠出多份移动锁。
UCLASS()
class MULTIPLAYER_API UmultiplayerStunEffect : public UGameplayEffect
{
	GENERATED_BODY()
// 配置同目标刷新时长的眩晕及共享禁行标签。
public: UmultiplayerStunEffect();
};

// 起手扣费并启动冷却；延时由 AbilityTask 管理，取消/死亡统一 EndAbility 销毁任务。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	// 声明范围技能分类和可由拥有者主动取消的生命周期规则。
	UmultiplayerAreaAbility();
	// 范围结算归属于该技能；服务器在出手时验证目标，角色不再代管技能专属执行。
	static void ResolveTargets(AmultiplayerCombatCharacter& Character, const FVector& Center);
protected:
	// 验证并锁定落点，提交后挂施法锁与起手任务；取消不退还已提交成本／冷却。
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	// 提前摘下本次效果句柄，再结束任务并精确解锁，避免同步回调清掉新一轮状态。
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	// 起手完成先标记已出手；服务器按锁定落点结算一次，再进入收招。
	UFUNCTION() void ReleaseArea();
	// 正常收招仅由服务器裁决远端结束，客户端只结束自身表现。
	UFUNCTION() void FinishRecovery();
	FActiveGameplayEffectHandle CastingEffect;
	FVector TargetLocation = FVector::ZeroVector;
	bool bReleased = false;
};
