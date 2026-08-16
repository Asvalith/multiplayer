// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/Interface.h"
#include "multiplayerAbilityPresentationInterface.generated.h"

class UAnimMontage;

/**
 * Presentation-only lifecycle emitted by a GameplayAbility.
 *
 * None of these phases grants gameplay authority. Blueprints may use them for
 * animation, camera, sound, or debug UI, while Cost, Cooldown, targeting, and
 * GameplayEffect application remain owned by GAS and the server.
 */
UENUM(BlueprintType)
enum class EmultiplayerAbilityPresentationPhase : uint8
{
	PredictedStarted,
	AuthorityStarted,
	Reconciled,
	Rejected,
	Completed,
	Interrupted,
	Cancelled
};

/** Asset references configured later by an Ability Blueprint or native CDO. */
USTRUCT(BlueprintType)
struct MULTIPLAYER_API FmultiplayerAbilityMontageConfig
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation")
	TObjectPtr<UAnimMontage> Montage;

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation", meta = (ClampMin = "0.01"))
	float PlayRate = 1.0f;

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation")
	FName StartSection = NAME_None;

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation", meta = (ClampMin = "0.0", Units = "s"))
	float RejectBlendOutSeconds = 0.1f;

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation")
	bool bStopOnRejected = true;

	UPROPERTY(EditAnywhere, Category = "GAS|Presentation")
	bool bStopOnCancelled = true;
};

USTRUCT(BlueprintType)
struct MULTIPLAYER_API FmultiplayerAbilityPresentationEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "GAS|Presentation")
	FGameplayTag AbilityTag;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|Presentation")
	EmultiplayerAbilityPresentationPhase Phase =
		EmultiplayerAbilityPresentationPhase::PredictedStarted;

	/** Diagnostic transaction id only; it is not a lockstep frame number. */
	UPROPERTY(BlueprintReadOnly, Category = "GAS|Presentation")
	int32 PredictionKey = 0;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|Presentation")
	bool bLocallyControlled = false;
};

UINTERFACE(BlueprintType)
class MULTIPLAYER_API UmultiplayerAbilityPresentationInterface : public UInterface
{
	GENERATED_BODY()
};

class MULTIPLAYER_API ImultiplayerAbilityPresentationInterface
{
	GENERATED_BODY()

public:
	/** Cosmetic notification only. It must never apply damage, healing, or GE state. */
	UFUNCTION(
		BlueprintNativeEvent,
		BlueprintCosmetic,
		Category = "GAS|Presentation",
		meta = (DisplayName = "On Ability Presentation Phase"))
	void HandleAbilityPresentation(const FmultiplayerAbilityPresentationEvent& Event);
};
