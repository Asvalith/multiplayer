// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "multiplayerCoopGameState.generated.h"

USTRUCT(BlueprintType)
struct FmultiplayerCoopObjectiveState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Coop|Objective")
	int32 ActivatedKeys = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Coop|Objective")
	int32 RequiredKeys = 2;

	UPROPERTY(BlueprintReadOnly, Category = "Coop|Objective")
	bool bGameWon = false;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerObjectiveProgressEvent,
	int32,
	ActivatedKeys,
	int32,
	RequiredKeys);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerGameWonEvent);

/** Replicated shared state for the cooperative objective. */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Coop|Objective")
	const FmultiplayerCoopObjectiveState& GetObjectiveState() const
	{
		return ObjectiveState;
	}

	UFUNCTION(BlueprintPure, Category = "Coop|Objective")
	bool IsObjectiveComplete() const
	{
		return ObjectiveState.ActivatedKeys >= ObjectiveState.RequiredKeys;
	}

	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Coop|Objective")
	void ConfigureRequiredKeys(int32 RequiredKeys);

	bool RegisterActivatedKey();
	bool TryCompleteGame();

	UPROPERTY(BlueprintAssignable, Category = "Coop|Objective")
	FmultiplayerObjectiveProgressEvent OnObjectiveProgressChanged;

	UPROPERTY(BlueprintAssignable, Category = "Coop|Objective")
	FmultiplayerGameWonEvent OnGameWon;

protected:
	UFUNCTION()
	void OnRep_ObjectiveState();

private:
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState)
	FmultiplayerCoopObjectiveState ObjectiveState;
};
