// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayEffect.h"
#include "multiplayerGameplayEffects.generated.h"

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerDamageEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerHealingEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerHealingEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerVulnerabilityEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerVulnerabilityEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerImmunityEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerImmunityEffect();
	virtual void PostInitProperties() override;
};

/**
 * Non-gameplay prediction lab marker. It visualizes a reversible predicted GE/Cue
 * and is only applied when the process explicitly runs with -GASM6Lab.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerPredictionPendingEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerPredictionPendingEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageCostEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerDamageCostEffect();
};

UCLASS()
class MULTIPLAYER_API UmultiplayerHealCostEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerHealCostEffect();
};

UCLASS()
class MULTIPLAYER_API UmultiplayerImmunityCostEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerImmunityCostEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerDamageCooldownEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerHealCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerHealCooldownEffect();
	virtual void PostInitProperties() override;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerImmunityCooldownEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerImmunityCooldownEffect();
	virtual void PostInitProperties() override;
};

/** Default attribute initialization entry for an editor-authored AbilitySet. */
UCLASS()
class MULTIPLAYER_API UmultiplayerInitStatsEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UmultiplayerInitStatsEffect();
};
