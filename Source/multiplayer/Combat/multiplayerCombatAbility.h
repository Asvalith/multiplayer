// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "multiplayerCombatAbility.generated.h"

// 三个技能共用服务器执行和死亡限制；客户端只通过自己的 ASC 发起技能请求。
// 默认沿用服务器执行；防御在自己的构造中覆写为本地预测，最终结果仍由服务器决定。
UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerCombatAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerCombatAbility();
};

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
};

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

protected:
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};
