// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayEffectExecutionCalculation.h"
#include "multiplayerDamageExecution.generated.h"

/**
 * Pure damage-calculation result. Keeping the intermediate values here makes
 * the formula independently testable without constructing an ASC or a World.
 */
struct MULTIPLAYER_API FmultiplayerDamageCalculationResult
{
	float RawDamage = 0.0f;
	float ArmorMultiplier = 1.0f;
	float ResistanceMultiplier = 1.0f;
	float VulnerabilityMultiplier = 1.0f;
	float AppliedCriticalMultiplier = 1.0f;
	float MitigatedDamage = 0.0f;
	float FinalDamage = 0.0f;
	bool bCritical = false;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UmultiplayerDamageExecution();
	virtual void Execute_Implementation(
		const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

	/** Pure helpers used by both the execution and formula automation tests. */
	static float CalculateRawDamage(float BaseDamage, float AttackPower);
	static float CalculateArmorMultiplier(float Armor);
	static float CalculateResistanceMultiplier(float Resistance);
	static float CalculateVulnerabilityMultiplier(int32 VulnerabilityStacks);
	static bool ShouldCriticalHit(
		float TargetHealth,
		float TargetMaxHealth,
		float CriticalChance,
		float CriticalRoll);

	static FmultiplayerDamageCalculationResult CalculateDamage(
		float BaseDamage,
		float AttackPower,
		float TargetArmor,
		float TargetResistance,
		float TargetHealth,
		float TargetMaxHealth,
		int32 VulnerabilityStacks,
		float CriticalChance,
		float CriticalMultiplier,
		float CriticalRoll);

	/**
	 * Compatibility wrapper for the original deterministic formula tests.
	 * New code should call CalculateDamage so every captured attribute is explicit.
	 */
	static float CalculateFinalDamage(
		float BaseDamage,
		float TargetHealth,
		float TargetMaxHealth,
		int32 VulnerabilityStacks,
		bool& bOutCritical);
};
