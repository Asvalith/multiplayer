// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;

/** Bridges the replicated win state to the owning Character Blueprint. */
UCLASS(ClassGroup = (Coop), meta = (BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerVictoryPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerVictoryPresenterComponent();

	/** Re-evaluates local ownership after possession or controller changes. */
	void RefreshBinding();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleGameWon();

private:
	void ClearBinding();

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	/** Prevents duplicate Blueprint presentation when the replicated state repeats. */
	bool bVictoryNotified = false;
};
