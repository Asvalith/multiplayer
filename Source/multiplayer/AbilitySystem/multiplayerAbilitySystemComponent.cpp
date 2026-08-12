// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAbilitySystemComponent.h"

#include "Abilities/GameplayAbility.h"

namespace
{
	FPredictionKey GetActivePredictionKey(const FGameplayAbilitySpec& AbilitySpec)
	{
		if (const UGameplayAbility* AbilityInstance = AbilitySpec.GetPrimaryInstance())
		{
			return AbilityInstance->GetCurrentActivationInfo().GetActivationPredictionKey();
		}

		return FPredictionKey();
	}
}

void UmultiplayerAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	FScopedAbilityListLock AbilityListLock(*this);
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			continue;
		}

		AbilitySpecInputPressed(AbilitySpec);
		if (AbilitySpec.IsActive())
		{
			InvokeReplicatedEvent(
				EAbilityGenericReplicatedEvent::InputPressed,
				AbilitySpec.Handle,
				GetActivePredictionKey(AbilitySpec));
		}
		else
		{
			TryActivateAbility(AbilitySpec.Handle);
		}
	}
}

void UmultiplayerAbilitySystemComponent::AbilityInputTagReleased(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	FScopedAbilityListLock AbilityListLock(*this);
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag) || !AbilitySpec.IsActive())
		{
			continue;
		}

		AbilitySpecInputReleased(AbilitySpec);
		InvokeReplicatedEvent(
			EAbilityGenericReplicatedEvent::InputReleased,
			AbilitySpec.Handle,
			GetActivePredictionKey(AbilitySpec));
	}
}
