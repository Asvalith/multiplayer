// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAbilitySystemGlobals.h"

#include "AbilitySystem/multiplayerGameplayEffectContext.h"

FGameplayEffectContext* UmultiplayerAbilitySystemGlobals::AllocGameplayEffectContext() const
{
	return new FmultiplayerGameplayEffectContext();
}
