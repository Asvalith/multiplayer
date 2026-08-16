// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "multiplayerGuardianGameplayAbility.generated.h"

/**
 * Persistent server-owned shield used by the Guardian.  Keeping immunity in a
 * GameplayEffect makes negative-effect rejection part of the GAS pipeline
 * instead of a presentation or animation decision.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGuardianShieldEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerGuardianShieldEffect();
	virtual void PostInitProperties() override;
};

/**
 * Minimal server-only attack implementation.  A Blueprint subclass may add
 * montage/cue presentation later, but target validation and GE application
 * remain authoritative and do not depend on an AnimNotify.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGuardianMeleeAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerGuardianMeleeAbility();

protected:
	virtual bool CanActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayTagContainer* SourceTags = nullptr,
		const FGameplayTagContainer* TargetTags = nullptr,
		FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;

	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};
