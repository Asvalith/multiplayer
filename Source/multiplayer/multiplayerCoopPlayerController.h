// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopPlayerController.generated.h"

class UmultiplayerVictoryPresenterComponent;

/** Owns local co-op UI flow and the owning-client match commands. */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AmultiplayerCoopPlayerController();

	/** Blueprint presentation hook fired once when the replicated victory state arrives locally. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Victory", meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

	/** Called by the local victory widget; the server remains authoritative over match travel. */
	UFUNCTION(BlueprintCallable, Category = "Coop|Victory", meta = (DisplayName = "Request Restart Coop Game"))
	void RequestRestartCoopGame();

protected:
	virtual void BeginPlayingState() override;

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRestartCoopGame();

private:
	/** Local-only bridge from replicated GameState to the PlayerController Blueprint. */
	UPROPERTY(VisibleAnywhere, Category = "Coop|Victory")
	TObjectPtr<UmultiplayerVictoryPresenterComponent> VictoryPresenter;
};
