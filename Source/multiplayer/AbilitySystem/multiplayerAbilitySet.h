// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/DataAsset.h"
#include "GameplayAbilitySpec.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "multiplayerAbilitySet.generated.h"

class UAbilitySystemComponent;
class UGameplayAbility;
class UGameplayEffect;

USTRUCT(BlueprintType)
struct FmultiplayerAbilitySetAbility
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "Ability")
	TSubclassOf<UGameplayAbility> Ability;

	UPROPERTY(EditDefaultsOnly, Category = "Ability", meta = (ClampMin = "1"))
	int32 AbilityLevel = 1;

	UPROPERTY(EditDefaultsOnly, Category = "Ability", meta = (Categories = "InputTag"))
	FGameplayTag InputTag;
};

USTRUCT(BlueprintType)
struct FmultiplayerAbilitySetEffect
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, Category = "Effect")
	TSubclassOf<UGameplayEffect> Effect;

	UPROPERTY(EditDefaultsOnly, Category = "Effect", meta = (ClampMin = "0"))
	float EffectLevel = 1.0f;
};

USTRUCT()
struct FmultiplayerAbilitySetGrantedHandles
{
	GENERATED_BODY()

	void TakeFromAbilitySystem(UAbilitySystemComponent* AbilitySystemComponent);

	TArray<FGameplayAbilitySpecHandle> AbilitySpecHandles;
	TArray<FActiveGameplayEffectHandle> GameplayEffectHandles;
};

UCLASS(BlueprintType, Const)
class MULTIPLAYER_API UmultiplayerAbilitySet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	void GiveToAbilitySystem(
		UAbilitySystemComponent* AbilitySystemComponent,
		FmultiplayerAbilitySetGrantedHandles* OutGrantedHandles,
		UObject* SourceObject = nullptr) const;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "GAS|Abilities")
	TArray<FmultiplayerAbilitySetAbility> GrantedAbilities;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Effects")
	TArray<FmultiplayerAbilitySetEffect> GrantedEffects;
};
