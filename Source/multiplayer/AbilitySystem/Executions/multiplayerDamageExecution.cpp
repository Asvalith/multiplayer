// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/Executions/multiplayerDamageExecution.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayEffect.h"
#include "multiplayer.h"

namespace
{
	float SanitizeNonNegativeDamage(float Value)
	{
		if (FMath::IsFinite(Value))
		{
			return FMath::Max(Value, 0.0f);
		}

		return Value > 0.0f ? MAX_flt : 0.0f;
	}

	struct FDamageCaptureDefinitions
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(AttackPower);
		DECLARE_ATTRIBUTE_CAPTUREDEF(CriticalChance);
		DECLARE_ATTRIBUTE_CAPTUREDEF(CriticalMultiplier);
		DECLARE_ATTRIBUTE_CAPTUREDEF(Health);
		DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);
		DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);
		DECLARE_ATTRIBUTE_CAPTUREDEF(Resistance);

		FDamageCaptureDefinitions()
		{
			// Offensive values are frozen when the outgoing GameplayEffectSpec is created.
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, AttackPower, Source, true);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, CriticalChance, Source, true);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, CriticalMultiplier, Source, true);

			// Defensive and life-state values are evaluated when the execution resolves.
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, Health, Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, MaxHealth, Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, Armor, Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, Resistance, Target, false);
		}
	};

	const FDamageCaptureDefinitions& DamageCaptures()
	{
		static FDamageCaptureDefinitions Definitions;
		return Definitions;
	}
}

UmultiplayerDamageExecution::UmultiplayerDamageExecution()
{
	RelevantAttributesToCapture.Add(DamageCaptures().AttackPowerDef);
	RelevantAttributesToCapture.Add(DamageCaptures().CriticalChanceDef);
	RelevantAttributesToCapture.Add(DamageCaptures().CriticalMultiplierDef);
	RelevantAttributesToCapture.Add(DamageCaptures().HealthDef);
	RelevantAttributesToCapture.Add(DamageCaptures().MaxHealthDef);
	RelevantAttributesToCapture.Add(DamageCaptures().ArmorDef);
	RelevantAttributesToCapture.Add(DamageCaptures().ResistanceDef);
}

void UmultiplayerDamageExecution::Execute_Implementation(
	const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	FAggregatorEvaluateParameters EvaluateParameters;
	EvaluateParameters.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvaluateParameters.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	float AttackPower = 0.0f;
	float CriticalChance = 0.0f;
	float CriticalMultiplier = 1.5f;
	float TargetHealth = 0.0f;
	float TargetMaxHealth = 1.0f;
	float TargetArmor = 0.0f;
	float TargetResistance = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().AttackPowerDef,
		EvaluateParameters,
		AttackPower);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().CriticalChanceDef,
		EvaluateParameters,
		CriticalChance);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().CriticalMultiplierDef,
		EvaluateParameters,
		CriticalMultiplier);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().HealthDef,
		EvaluateParameters,
		TargetHealth);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().MaxHealthDef,
		EvaluateParameters,
		TargetMaxHealth);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().ArmorDef,
		EvaluateParameters,
		TargetArmor);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().ResistanceDef,
		EvaluateParameters,
		TargetResistance);

	const float BaseDamage = Spec.GetSetByCallerMagnitude(
		MultiplayerGameplayTags::Data_Damage,
		false,
		0.0f);
	int32 VulnerabilityStacks = 0;
	if (const UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent())
	{
		FGameplayTagContainer VulnerabilityTags;
		VulnerabilityTags.AddTag(MultiplayerGameplayTags::State_Vulnerable);
		VulnerabilityStacks = TargetASC->GetAggregatedStackCount(
			FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(VulnerabilityTags));
	}

	// ExecCalcs for this project are applied by the server-owned damage chain;
	// the roll is therefore authoritative and never supplied by TargetData.
	const float CriticalRoll = FMath::FRand();
	const FmultiplayerDamageCalculationResult DamageResult = CalculateDamage(
		BaseDamage,
		AttackPower,
		TargetArmor,
		TargetResistance,
		TargetHealth,
		TargetMaxHealth,
		VulnerabilityStacks,
		CriticalChance,
		CriticalMultiplier,
		CriticalRoll);

	FGameplayEffectContextHandle ContextHandle = Spec.GetContext();
	FGameplayEffectContext* BaseContext = ContextHandle.Get();
	if (FmultiplayerGameplayEffectContext* Context =
		BaseContext != nullptr
			&& BaseContext->GetScriptStruct()->IsChildOf(
				FmultiplayerGameplayEffectContext::StaticStruct())
				? static_cast<FmultiplayerGameplayEffectContext*>(BaseContext)
				: nullptr)
	{
		Context->SetCriticalHit(DamageResult.bCritical);
		Context->SetHitType(
			DamageResult.bCritical
				? EmultiplayerHitType::Critical
				: EmultiplayerHitType::Direct);
		if (const FHitResult* HitResult = Context->GetHitResult())
		{
			Context->SetImpactImpulse(HitResult->TraceEnd - HitResult->TraceStart);
		}
	}

	if (DamageResult.FinalDamage > 0.0f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UmultiplayerAttributeSet::GetIncomingDamageAttribute(),
			EGameplayModOp::Additive,
			DamageResult.FinalDamage));
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_DAMAGE_EXEC Base=%.1f Health=%.1f/%.1f Vulnerability=%d Critical=%s Final=%.1f AttackPower=%.1f Armor=%.1f CriticalChance=%.3f CriticalMultiplier=%.3f Resistance=%.3f CriticalRoll=%.3f Raw=%.1f ArmorMultiplier=%.3f ResistanceMultiplier=%.3f VulnerabilityMultiplier=%.3f AppliedCriticalMultiplier=%.3f Mitigated=%.1f"),
		BaseDamage,
		TargetHealth,
		TargetMaxHealth,
		VulnerabilityStacks,
		DamageResult.bCritical ? TEXT("true") : TEXT("false"),
		DamageResult.FinalDamage,
		AttackPower,
		TargetArmor,
		CriticalChance,
		CriticalMultiplier,
		TargetResistance,
		CriticalRoll,
		DamageResult.RawDamage,
		DamageResult.ArmorMultiplier,
		DamageResult.ResistanceMultiplier,
		DamageResult.VulnerabilityMultiplier,
		DamageResult.AppliedCriticalMultiplier,
		DamageResult.MitigatedDamage);
}

float UmultiplayerDamageExecution::CalculateRawDamage(float BaseDamage, float AttackPower)
{
	const float SafeBaseDamage = FMath::IsFinite(BaseDamage) ? BaseDamage : 0.0f;
	const float SafeAttackPower = FMath::IsFinite(AttackPower) ? AttackPower : 0.0f;
	return SanitizeNonNegativeDamage(SafeBaseDamage + SafeAttackPower);
}

float UmultiplayerDamageExecution::CalculateArmorMultiplier(float Armor)
{
	if (!FMath::IsFinite(Armor))
	{
		return Armor > 0.0f ? 0.0f : 1.0f;
	}

	return 100.0f / (100.0f + FMath::Max(Armor, 0.0f));
}

float UmultiplayerDamageExecution::CalculateResistanceMultiplier(float Resistance)
{
	if (!FMath::IsFinite(Resistance))
	{
		return Resistance > 0.0f ? 0.2f : 1.0f;
	}

	return 1.0f - FMath::Clamp(Resistance, 0.0f, 0.8f);
}

float UmultiplayerDamageExecution::CalculateVulnerabilityMultiplier(int32 VulnerabilityStacks)
{
	return 1.0f
		+ 0.1f * static_cast<float>(FMath::Clamp(VulnerabilityStacks, 0, 3));
}

bool UmultiplayerDamageExecution::ShouldCriticalHit(
	float TargetHealth,
	float TargetMaxHealth,
	float CriticalChance,
	float CriticalRoll)
{
	if (!FMath::IsFinite(TargetHealth) || TargetHealth <= 0.0f)
	{
		return false;
	}

	const float SafeMaxHealth =
		FMath::IsFinite(TargetMaxHealth) ? FMath::Max(TargetMaxHealth, 1.0f) : 1.0f;
	const bool bLowHealthCritical =
		TargetHealth <= SafeMaxHealth * 0.5f;
	const float ClampedChance = FMath::IsFinite(CriticalChance)
		? FMath::Clamp(CriticalChance, 0.0f, 1.0f)
		: 0.0f;
	const float ClampedRoll = FMath::IsFinite(CriticalRoll)
		? FMath::Clamp(CriticalRoll, 0.0f, 1.0f)
		: 1.0f;
	const bool bChanceCritical =
		ClampedChance >= 1.0f
		|| (ClampedChance > 0.0f && ClampedRoll < ClampedChance);
	return bLowHealthCritical || bChanceCritical;
}

FmultiplayerDamageCalculationResult UmultiplayerDamageExecution::CalculateDamage(
	float BaseDamage,
	float AttackPower,
	float TargetArmor,
	float TargetResistance,
	float TargetHealth,
	float TargetMaxHealth,
	int32 VulnerabilityStacks,
	float CriticalChance,
	float CriticalMultiplier,
	float CriticalRoll)
{
	FmultiplayerDamageCalculationResult Result;
	Result.RawDamage = CalculateRawDamage(BaseDamage, AttackPower);
	Result.ArmorMultiplier = CalculateArmorMultiplier(TargetArmor);
	Result.ResistanceMultiplier = CalculateResistanceMultiplier(TargetResistance);
	Result.VulnerabilityMultiplier =
		CalculateVulnerabilityMultiplier(VulnerabilityStacks);
	Result.bCritical = ShouldCriticalHit(
		TargetHealth,
		TargetMaxHealth,
		CriticalChance,
		CriticalRoll);
	const float SafeCriticalMultiplier = FMath::IsFinite(CriticalMultiplier)
		? FMath::Max(CriticalMultiplier, 1.0f)
		: 1.0f;
	Result.AppliedCriticalMultiplier = Result.bCritical ? SafeCriticalMultiplier : 1.0f;
	Result.MitigatedDamage = SanitizeNonNegativeDamage(
		Result.RawDamage * Result.ArmorMultiplier * Result.ResistanceMultiplier);
	Result.FinalDamage = SanitizeNonNegativeDamage(
		Result.MitigatedDamage
			* Result.VulnerabilityMultiplier
			* Result.AppliedCriticalMultiplier);
	return Result;
}

float UmultiplayerDamageExecution::CalculateFinalDamage(
	float BaseDamage,
	float TargetHealth,
	float TargetMaxHealth,
	int32 VulnerabilityStacks,
	bool& bOutCritical)
{
	const FmultiplayerDamageCalculationResult Result = CalculateDamage(
		BaseDamage,
		0.0f,
		0.0f,
		0.0f,
		TargetHealth,
		TargetMaxHealth,
		VulnerabilityStacks,
		0.0f,
		1.5f,
		1.0f);
	bOutCritical = Result.bCritical;
	return Result.FinalDamage;
}
