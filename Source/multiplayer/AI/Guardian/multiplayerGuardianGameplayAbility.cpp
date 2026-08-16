// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/Guardian/multiplayerGuardianGameplayAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AI/Guardian/multiplayerGuardianCharacter.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "multiplayer.h"

namespace
{
	void AddGuardianShieldAssetTags(UGameplayEffect& Effect)
	{
		FInheritedTagContainer Tags;
		Tags.AddTag(MultiplayerGameplayTags::Effect_Positive_GuardianShield);
		Effect.FindOrAddComponent<UAssetTagsGameplayEffectComponent>()
			.SetAndApplyAssetTagChanges(Tags);
	}

	void AddGuardianShieldTargetTags(UGameplayEffect& Effect)
	{
		FInheritedTagContainer Tags;
		Tags.AddTag(MultiplayerGameplayTags::State_Guardian_Shielded);
		Tags.AddTag(MultiplayerGameplayTags::State_Immune);
		Effect.FindOrAddComponent<UTargetTagsGameplayEffectComponent>()
			.SetAndApplyTargetTagChanges(Tags);
	}

	void AddGuardianShieldGameplayCue(UGameplayEffect& Effect)
	{
		for (const FGameplayEffectCue& ExistingCue : Effect.GameplayCues)
		{
			if (ExistingCue.GameplayCueTags.HasTagExact(
				MultiplayerGameplayTags::GameplayCue_Coop_Guardian_State_Shield))
			{
				return;
			}
		}
		Effect.GameplayCues.Emplace(
			MultiplayerGameplayTags::GameplayCue_Coop_Guardian_State_Shield,
			0.0f,
			0.0f);
	}
}

UmultiplayerGuardianShieldEffect::UmultiplayerGuardianShieldEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
}

void UmultiplayerGuardianShieldEffect::PostInitProperties()
{
	Super::PostInitProperties();
	AddGuardianShieldAssetTags(*this);
	AddGuardianShieldTargetTags(*this);
	AddGuardianShieldGameplayCue(*this);

	FGameplayTagContainer NegativeEffectTags;
	NegativeEffectTags.AddTag(MultiplayerGameplayTags::Effect_Negative);
	FindOrAddComponent<UImmunityGameplayEffectComponent>().ImmunityQueries.Add(
		FGameplayEffectQuery::MakeQuery_MatchAnyEffectTags(NegativeEffectTags));
}

UmultiplayerGuardianMeleeAbility::UmultiplayerGuardianMeleeAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
	ActivationBlockedTags.AddTag(MultiplayerGameplayTags::State_Dead);

	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Guardian_Melee);
	SetAssetTags(AssetTags);
}

bool UmultiplayerGuardianMeleeAbility::CanActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayTagContainer* SourceTags,
	const FGameplayTagContainer* TargetTags,
	FGameplayTagContainer* OptionalRelevantTags) const
{
	if (!Super::CanActivateAbility(
		Handle,
		ActorInfo,
		SourceTags,
		TargetTags,
		OptionalRelevantTags))
	{
		return false;
	}

	const AmultiplayerGuardianCharacter* Guardian = ActorInfo != nullptr
		? Cast<AmultiplayerGuardianCharacter>(ActorInfo->AvatarActor.Get())
		: nullptr;
	return Guardian != nullptr
		&& Guardian->HasAuthority()
		&& Guardian->CanCommitMeleeAttack(Guardian->GetCurrentTarget());
}

void UmultiplayerGuardianMeleeAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	AmultiplayerGuardianCharacter* Guardian = ActorInfo != nullptr
		? Cast<AmultiplayerGuardianCharacter>(ActorInfo->AvatarActor.Get())
		: nullptr;
	AActor* TargetActor = Guardian != nullptr ? Guardian->GetCurrentTarget() : nullptr;
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	UAbilitySystemComponent* TargetASC = IsValid(TargetActor)
		? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor)
		: nullptr;

	if (Guardian == nullptr
		|| !Guardian->CanCommitMeleeAttack(TargetActor)
		|| SourceASC == nullptr
		|| TargetASC == nullptr
		|| !CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	const TSubclassOf<UGameplayEffect> DamageEffectClass =
		Guardian->GetMeleeDamageEffectClass();
	FGameplayEffectSpecHandle DamageSpec = MakeOutgoingGameplayEffectSpec(
		DamageEffectClass,
		GetAbilityLevel());
	if (!DamageSpec.IsValid())
	{
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("GUARDIAN_ATTACK_REJECT Guardian=%s Target=%s Reason=InvalidDamageEffect"),
			*GetNameSafe(Guardian),
			*GetNameSafe(TargetActor));
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	DamageSpec.Data->SetSetByCallerMagnitude(
		MultiplayerGameplayTags::Data_Damage,
		Guardian->GetMeleeDamage());
	SourceASC->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data.Get(), TargetASC);

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_ATTACK_COMMIT Guardian=%s Target=%s Damage=%.1f"),
		*GetNameSafe(Guardian),
		*GetNameSafe(TargetActor),
		Guardian->GetMeleeDamage());

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
