// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemGlobals.h"
#include "multiplayerAbilitySystemGlobals.generated.h"

UCLASS()
class MULTIPLAYER_API UmultiplayerAbilitySystemGlobals : public UAbilitySystemGlobals
{
	GENERATED_BODY()

public:
	virtual FGameplayEffectContext* AllocGameplayEffectContext() const override;
};
