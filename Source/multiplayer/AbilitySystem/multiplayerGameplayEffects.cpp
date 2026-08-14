// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerGameplayEffects.h"

#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/Executions/multiplayerDamageExecution.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

namespace
{
	void AddAssetTag(UGameplayEffect& Effect, const FGameplayTag& Tag)
	{
		FInheritedTagContainer Tags;
		Tags.AddTag(Tag);
		Effect.FindOrAddComponent<UAssetTagsGameplayEffectComponent>()
			.SetAndApplyAssetTagChanges(Tags);
	}

	void AddTargetTag(UGameplayEffect& Effect, const FGameplayTag& Tag)
	{
		FInheritedTagContainer Tags;
		Tags.AddTag(Tag);
		Effect.FindOrAddComponent<UTargetTagsGameplayEffectComponent>()
			.SetAndApplyTargetTagChanges(Tags);
	}

	void AddGameplayCueTag(UGameplayEffect& Effect, const FGameplayTag& Tag)
	{
		for (const FGameplayEffectCue& ExistingCue : Effect.GameplayCues)
		{
			if (ExistingCue.GameplayCueTags.HasTagExact(Tag))
			{
				return;
			}
		}

		Effect.GameplayCues.Emplace(Tag, 0.0f, 0.0f);
	}

	void AddSetByCallerModifier(
		UGameplayEffect& Effect,
		const FGameplayAttribute& Attribute,
		const FGameplayTag& DataTag)
	{
		FSetByCallerFloat SetByCaller;
		SetByCaller.DataTag = DataTag;

		FGameplayModifierInfo Modifier;
		Modifier.Attribute = Attribute;
		Modifier.ModifierOp = EGameplayModOp::Additive;
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(SetByCaller);
		Effect.Modifiers.Add(Modifier);
	}

	void AddConstantModifier(
		UGameplayEffect& Effect,
		const FGameplayAttribute& Attribute,
		float Magnitude)
	{
		FGameplayModifierInfo Modifier;
		Modifier.Attribute = Attribute;
		Modifier.ModifierOp = EGameplayModOp::Additive;
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Magnitude));
		Effect.Modifiers.Add(Modifier);
	}

	void AddOverrideModifier(
		UGameplayEffect& Effect,
		const FGameplayAttribute& Attribute,
		float Magnitude)
	{
		FGameplayModifierInfo Modifier;
		Modifier.Attribute = Attribute;
		Modifier.ModifierOp = EGameplayModOp::Override;
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Magnitude));
		Effect.Modifiers.Add(Modifier);
	}

	void ConfigureCooldown(UGameplayEffect& Effect, float Duration)
	{
		Effect.DurationPolicy = EGameplayEffectDurationType::HasDuration;
		Effect.DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
	}
}

UmultiplayerDamageEffect::UmultiplayerDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayEffectExecutionDefinition ExecutionDefinition;
	ExecutionDefinition.CalculationClass = UmultiplayerDamageExecution::StaticClass();
	Executions.Add(ExecutionDefinition);
}

UmultiplayerVulnerabilityEffect::UmultiplayerVulnerabilityEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(8.0f));
	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	StackLimitCount = 3;
	StackDurationRefreshPolicy =
		EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	StackPeriodResetPolicy =
		EGameplayEffectStackingPeriodPolicy::NeverReset;
	StackExpirationPolicy =
		EGameplayEffectStackingExpirationPolicy::ClearEntireStack;
	bDenyOverflowApplication = true;
	bSuppressStackingCues = true;
}

void UmultiplayerVulnerabilityEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Negative_Vulnerability);
	AddTargetTag(*this, MultiplayerGameplayTags::State_Vulnerable);
	AddGameplayCueTag(*this, MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability);
}

void UmultiplayerDamageEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Negative_Damage);
	AddGameplayCueTag(*this, MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact);
}

UmultiplayerHealingEffect::UmultiplayerHealingEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddSetByCallerModifier(
		*this,
		UmultiplayerAttributeSet::GetIncomingHealingAttribute(),
		MultiplayerGameplayTags::Data_Heal);
}

void UmultiplayerHealingEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Positive_Heal);
	AddGameplayCueTag(*this, MultiplayerGameplayTags::GameplayCue_Coop_Heal_Result);
}

UmultiplayerImmunityEffect::UmultiplayerImmunityEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(5.0f));
}

void UmultiplayerImmunityEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Positive_Immunity);
	AddTargetTag(*this, MultiplayerGameplayTags::State_Immune);
	AddGameplayCueTag(*this, MultiplayerGameplayTags::GameplayCue_Coop_State_Immunity);

	FGameplayTagContainer NegativeEffectTags;
	NegativeEffectTags.AddTag(MultiplayerGameplayTags::Effect_Negative);
	FindOrAddComponent<UImmunityGameplayEffectComponent>().ImmunityQueries.Add(
		FGameplayEffectQuery::MakeQuery_MatchAnyEffectTags(NegativeEffectTags));
}

UmultiplayerPredictionPendingEffect::UmultiplayerPredictionPendingEffect()
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// Long enough that packet-loss tests cannot confuse natural expiration with
	// PredictionKey rejection cleanup. This effect only runs under -GASM6Lab.
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(30.0f));
}

void UmultiplayerPredictionPendingEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(
		*this,
		MultiplayerGameplayTags::Effect_Debug_PredictionPending);
	AddGameplayCueTag(
		*this,
		MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending);
}

UmultiplayerDamageCostEffect::UmultiplayerDamageCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddConstantModifier(*this, UmultiplayerAttributeSet::GetEnergyAttribute(), -10.0f);
}

UmultiplayerHealCostEffect::UmultiplayerHealCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddConstantModifier(*this, UmultiplayerAttributeSet::GetEnergyAttribute(), -20.0f);
}

UmultiplayerImmunityCostEffect::UmultiplayerImmunityCostEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddConstantModifier(*this, UmultiplayerAttributeSet::GetEnergyAttribute(), -30.0f);
}

void UmultiplayerImmunityCostEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Cost_Immunity);
}

UmultiplayerDamageCooldownEffect::UmultiplayerDamageCooldownEffect()
{
	ConfigureCooldown(*this, 1.0f);
}

void UmultiplayerDamageCooldownEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddTargetTag(*this, MultiplayerGameplayTags::Cooldown_Ability_Damage);
}

UmultiplayerHealCooldownEffect::UmultiplayerHealCooldownEffect()
{
	ConfigureCooldown(*this, 3.0f);
}

void UmultiplayerHealCooldownEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddTargetTag(*this, MultiplayerGameplayTags::Cooldown_Ability_Heal);
}

UmultiplayerImmunityCooldownEffect::UmultiplayerImmunityCooldownEffect()
{
	ConfigureCooldown(*this, 8.0f);
}

void UmultiplayerImmunityCooldownEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddTargetTag(*this, MultiplayerGameplayTags::Cooldown_Ability_Immunity);
}

UmultiplayerInitStatsEffect::UmultiplayerInitStatsEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetMaxHealthAttribute(), 100.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetHealthAttribute(), 100.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetMaxEnergyAttribute(), 100.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetEnergyAttribute(), 100.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetAttackPowerAttribute(), 0.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetArmorAttribute(), 0.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetCriticalChanceAttribute(), 0.0f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetCriticalMultiplierAttribute(), 1.5f);
	AddOverrideModifier(*this, UmultiplayerAttributeSet::GetResistanceAttribute(), 0.0f);
}
