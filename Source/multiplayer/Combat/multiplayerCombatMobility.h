// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Combat/multiplayerCombatAbility.h"
#include "multiplayerCombatMobility.generated.h"

// 冲刺仍使用同一份能量属性与预测账本，不直接改客户端数值。
UCLASS()
class MULTIPLAYER_API UmultiplayerDashCostEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerDashCostEffect();
};

UCLASS()
class MULTIPLAYER_API UmultiplayerDashCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()
public:
	UmultiplayerDashCooldownEffect();
};

// GAS 预测激活与消耗，原生 RootMotionSource 交给 CMC 处理碰撞、移动包与服务器纠正。
UCLASS()
class MULTIPLAYER_API UmultiplayerDashAbility : public UmultiplayerCombatAbility
{
	GENERATED_BODY()
public:
	UmultiplayerDashAbility();
protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
private:
	UFUNCTION() void FinishDash();
};
