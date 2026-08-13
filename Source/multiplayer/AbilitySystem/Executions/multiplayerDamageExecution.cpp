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
	struct FDamageCaptureDefinitions
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(Health);
		DECLARE_ATTRIBUTE_CAPTUREDEF(MaxHealth);

		FDamageCaptureDefinitions()
		{
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, Health, Target, false);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerAttributeSet, MaxHealth, Target, false);
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
	RelevantAttributesToCapture.Add(DamageCaptures().HealthDef);
	RelevantAttributesToCapture.Add(DamageCaptures().MaxHealthDef);
}

void UmultiplayerDamageExecution::Execute_Implementation(
	const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	FAggregatorEvaluateParameters EvaluateParameters;
	EvaluateParameters.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	EvaluateParameters.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	float TargetHealth = 0.0f;
	float TargetMaxHealth = 1.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().HealthDef,
		EvaluateParameters,
		TargetHealth);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
		DamageCaptures().MaxHealthDef,
		EvaluateParameters,
		TargetMaxHealth);

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

	bool bCritical = false;
	const float FinalDamage = CalculateFinalDamage(
		BaseDamage,
		TargetHealth,
		TargetMaxHealth,
		VulnerabilityStacks,
		bCritical);

	FGameplayEffectContextHandle ContextHandle = Spec.GetContext();
	FGameplayEffectContext* BaseContext = ContextHandle.Get();
	if (FmultiplayerGameplayEffectContext* Context =
		BaseContext != nullptr
			&& BaseContext->GetScriptStruct()->IsChildOf(
				FmultiplayerGameplayEffectContext::StaticStruct())
				? static_cast<FmultiplayerGameplayEffectContext*>(BaseContext)
				: nullptr)
	{
		Context->SetCriticalHit(bCritical);
		Context->SetHitType(
			bCritical ? EmultiplayerHitType::Critical : EmultiplayerHitType::Direct);
		if (const FHitResult* HitResult = Context->GetHitResult())
		{
			Context->SetImpactImpulse(HitResult->TraceEnd - HitResult->TraceStart);
		}
	}

	if (FinalDamage > 0.0f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
			UmultiplayerAttributeSet::GetIncomingDamageAttribute(),
			EGameplayModOp::Additive,
			FinalDamage));
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_DAMAGE_EXEC Base=%.1f Health=%.1f/%.1f Vulnerability=%d Critical=%s Final=%.1f"),
		BaseDamage,
		TargetHealth,
		TargetMaxHealth,
		VulnerabilityStacks,
		bCritical ? TEXT("true") : TEXT("false"),
		FinalDamage);
}

float UmultiplayerDamageExecution::CalculateFinalDamage(
	float BaseDamage,
	float TargetHealth,
	float TargetMaxHealth,
	int32 VulnerabilityStacks,
	bool& bOutCritical)
{
	const float SafeMaxHealth = FMath::Max(TargetMaxHealth, 1.0f);
	bOutCritical = TargetHealth > 0.0f && TargetHealth <= SafeMaxHealth * 0.5f;
	const float CriticalMultiplier = bOutCritical ? 1.5f : 1.0f;
	const float VulnerabilityMultiplier =
		1.0f + 0.1f * static_cast<float>(FMath::Clamp(VulnerabilityStacks, 0, 3));
	return FMath::Max(BaseDamage, 0.0f) * CriticalMultiplier * VulnerabilityMultiplier;
}
