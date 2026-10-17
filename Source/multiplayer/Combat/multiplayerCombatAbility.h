// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Combat/multiplayerCombatConfig.h"
#include "multiplayerCombatAbility.generated.h"

// UE5.5禁止客户端任意移除权威GE；只开放“结束本次本地施法”的窄入口，不开放资源/CD移除。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatAbilitySystem : public UAbilitySystemComponent
{
	GENERATED_BODY()
public:
	void EndCasting(FActiveGameplayEffectHandle ServerHandle, const FPredictionKey& ActivationKey);
};

// 三个技能共用 GAS 本地预测、连接接纳与死亡限制；最终玩法结果仍由服务器决定。
UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerCombatAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerCombatAbility();
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
protected:
	// 所有技能的资源与冷却从同一份规则读取；效果继续交给 GAS 预测与确认。
	virtual bool CheckCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
	virtual void ApplyCost(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	virtual void ApplyCooldown(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) const override;
	virtual void CommitExecute(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo) override;
	const FmultiplayerCombatSkillRule& GetRule() const;
	EMultiplayerCombatSkill Skill = EMultiplayerCombatSkill::Attack;
	// InstancedPerActor 会复用实例；同步取消后再激活时，旧调用栈不能继续推动新一轮技能。
	uint32 ActivationRevision = 0;
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

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerAttackCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 普通攻击冷却半秒。
	UmultiplayerAttackCooldownEffect();
};

// 普通攻击：提交冷却后生成服务器投射物，伤害在命中时结算。
UCLASS()
class MULTIPLAYER_API UmultiplayerAttackAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	UmultiplayerAttackAbility();

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	UFUNCTION() void ReleaseProjectile();
	UFUNCTION() void FinishAttack();
	bool bProjectileReleased = false;
};

// 防御：消耗、冷却、限时效果和技能。
UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 防御技能消耗二十点能量。
	UmultiplayerDefendCostEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 防御技能冷却八秒。
	UmultiplayerDefendCooldownEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerDefendingEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 三秒内授予防御标签，减伤由属性结算层统一执行。
	UmultiplayerDefendingEffect();
};

// 防御：由限时 GameplayEffect 授予减伤状态，重复施放不会叠加。
// 客户端先预测消耗、冷却和自身防御；服务器拒绝时由同一个预测键撤销这些效果。
UCLASS()
class MULTIPLAYER_API UmultiplayerDefendAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	UmultiplayerDefendAbility();

protected:
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
	// 一次恢复三十点血量，属性集负责限制上限与禁止复活。
	UmultiplayerHealEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 治疗技能消耗二十五点能量。
	UmultiplayerHealCostEffect();
};

UCLASS(Blueprintable)
class MULTIPLAYER_API UmultiplayerHealCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	// 治疗技能冷却六秒。
	UmultiplayerHealCooldownEffect();
};

// 治疗：选择附近同阵营最低血量比例的存活角色；没有合法目标时不扣资源。
UCLASS()
class MULTIPLAYER_API UmultiplayerHealAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()

public:
	UmultiplayerHealAbility();
	virtual bool CanActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo, const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr, FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};

// 范围技能沿用防御/治疗的资源通道，不另写客户端扣费或冷却计时。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public: UmultiplayerAreaCostEffect();
};
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public: UmultiplayerAreaCooldownEffect();
};
UCLASS(NotBlueprintable)
class MULTIPLAYER_API UmultiplayerCastingEffect : public UGameplayEffect
{
	GENERATED_BODY()
public: UmultiplayerCastingEffect();
};
// 同一目标只保留一份眩晕，再次施加刷新时长，不能叠出多份移动锁。
UCLASS()
class MULTIPLAYER_API UmultiplayerStunEffect : public UGameplayEffect
{
	GENERATED_BODY()
public: UmultiplayerStunEffect();
};

// 起手扣费并启动冷却；延时由 AbilityTask 管理，取消/死亡统一 EndAbility 销毁任务。
UCLASS()
class MULTIPLAYER_API UmultiplayerAreaAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	UmultiplayerAreaAbility();
protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, bool bReplicateEndAbility, bool bWasCancelled) override;
private:
	UFUNCTION() void ReleaseArea();
	UFUNCTION() void FinishRecovery();
	FActiveGameplayEffectHandle CastingEffect;
	FVector TargetLocation = FVector::ZeroVector;
	bool bReleased = false;
};
