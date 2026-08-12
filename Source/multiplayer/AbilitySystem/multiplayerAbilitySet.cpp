// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAbilitySet.h"

#include "AbilitySystemComponent.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffect.h"

void FmultiplayerAbilitySetGrantedHandles::TakeFromAbilitySystem(UAbilitySystemComponent* AbilitySystemComponent)
{
	if (AbilitySystemComponent == nullptr || !AbilitySystemComponent->IsOwnerActorAuthoritative())
	{
		return;
	}

	for (const FGameplayAbilitySpecHandle& Handle : AbilitySpecHandles)
	{
		if (Handle.IsValid())
		{
			AbilitySystemComponent->ClearAbility(Handle);
		}
	}

	for (const FActiveGameplayEffectHandle& Handle : GameplayEffectHandles)
	{
		if (Handle.IsValid())
		{
			AbilitySystemComponent->RemoveActiveGameplayEffect(Handle);
		}
	}

	AbilitySpecHandles.Reset();
	GameplayEffectHandles.Reset();
}

void UmultiplayerAbilitySet::GiveToAbilitySystem(
	UAbilitySystemComponent* AbilitySystemComponent,
	FmultiplayerAbilitySetGrantedHandles* OutGrantedHandles,
	UObject* SourceObject) const
{
	if (AbilitySystemComponent == nullptr || !AbilitySystemComponent->IsOwnerActorAuthoritative())
	{
		return;
	}

	for (const FmultiplayerAbilitySetAbility& AbilityEntry : GrantedAbilities)
	{
		if (!AbilityEntry.Ability)
		{
			continue;
		}

		FGameplayAbilitySpec AbilitySpec(
			AbilityEntry.Ability,
			AbilityEntry.AbilityLevel,
			INDEX_NONE,
			SourceObject);
		if (AbilityEntry.InputTag.IsValid())
		{
			AbilitySpec.GetDynamicSpecSourceTags().AddTag(AbilityEntry.InputTag);
		}

		const FGameplayAbilitySpecHandle Handle = AbilitySystemComponent->GiveAbility(AbilitySpec);
		if (OutGrantedHandles != nullptr)
		{
			OutGrantedHandles->AbilitySpecHandles.Add(Handle);
		}
	}

	for (const FmultiplayerAbilitySetEffect& EffectEntry : GrantedEffects)
	{
		if (!EffectEntry.Effect)
		{
			continue;
		}

		FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
		Context.AddSourceObject(SourceObject);
		const FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(
			EffectEntry.Effect,
			EffectEntry.EffectLevel,
			Context);
		if (!Spec.IsValid())
		{
			continue;
		}

		const FActiveGameplayEffectHandle Handle =
			AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
		if (OutGrantedHandles != nullptr)
		{
			OutGrantedHandles->GameplayEffectHandles.Add(Handle);
		}
	}
}
