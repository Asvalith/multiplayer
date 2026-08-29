// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;

/** Bridges replicated victory state to the owning local PlayerController. */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerVictoryPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerVictoryPresenterComponent();

	/** Re-evaluates local ownership and the current replicated GameState. */
	void RefreshBinding();

protected:
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
