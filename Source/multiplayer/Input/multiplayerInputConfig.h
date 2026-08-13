// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "multiplayerInputConfig.generated.h"

class UInputAction;

USTRUCT(BlueprintType)
struct FmultiplayerTaggedInputAction
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
	TObjectPtr<const UInputAction> InputAction;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input", meta = (Categories = "InputTag"))
	FGameplayTag InputTag;
};

/** Data-only mapping between Enhanced Input assets and GAS input tags. */
UCLASS(BlueprintType, Const)
class MULTIPLAYER_API UmultiplayerInputConfig : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	const TArray<FmultiplayerTaggedInputAction>& GetAbilityInputActions() const
	{
		return AbilityInputActions;
	}

	UFUNCTION(BlueprintPure, Category = "Input")
	const UInputAction* FindAbilityInputActionForTag(
		FGameplayTag InputTag,
		bool bLogNotFound = false) const;

private:
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input|Abilities", meta = (AllowPrivateAccess = "true"))
	TArray<FmultiplayerTaggedInputAction> AbilityInputActions;
};
