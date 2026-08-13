// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayEffectExecutionCalculation.h"
#include "multiplayerDamageExecution.generated.h"

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UmultiplayerDamageExecution();
	virtual void Execute_Implementation(
		const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

	static float CalculateFinalDamage(
		float BaseDamage,
		float TargetHealth,
		float TargetMaxHealth,
		int32 VulnerabilityStacks,
		bool& bOutCritical);
};
