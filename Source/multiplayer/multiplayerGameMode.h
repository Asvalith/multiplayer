// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "multiplayerGameMode.generated.h"

UCLASS(minimalapi)
class AmultiplayerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AmultiplayerGameMode();

	/** Reloads the current match and travels every connected player together. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Coop|Match")
	void RestartCoopGame();

	/** Records one newly activated socket in the authoritative objective. */
	bool RegisterActivatedKey();

	/** Completes the match when both objective and occupancy rules are satisfied. */
	bool TryCompleteCoopGame(int32 CurrentPlayers, int32 RequiredPlayers);

protected:
	virtual void BeginPlay() override;

	/** Authoritative number of rack slots that must be activated before victory. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Coop|Objective", meta = (ClampMin = "1"))
	int32 RequiredKeys = 4;

private:
	int32 ResolveRequiredKeys() const;

	/** Makes simultaneous restart clicks from both victory screens idempotent. */
	bool bRestartTravelRequested = false;
};
