// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerGameplayEffects.h"

#include "AbilitySystem/multiplayerAttributeSet.h"
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

	void ConfigureCooldown(UGameplayEffect& Effect, float Duration)
	{
		Effect.DurationPolicy = EGameplayEffectDurationType::HasDuration;
		Effect.DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Duration));
	}
}

UmultiplayerDamageEffect::UmultiplayerDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	AddSetByCallerModifier(
		*this,
		UmultiplayerAttributeSet::GetIncomingDamageAttribute(),
		MultiplayerGameplayTags::Data_Damage);
}

void UmultiplayerDamageEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddAssetTag(*this, MultiplayerGameplayTags::Effect_Negative_Damage);
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

	FGameplayTagContainer NegativeEffectTags;
	NegativeEffectTags.AddTag(MultiplayerGameplayTags::Effect_Negative);
	FindOrAddComponent<UImmunityGameplayEffectComponent>().ImmunityQueries.Add(
		FGameplayEffectQuery::MakeQuery_MatchAnyEffectTags(NegativeEffectTags));
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
