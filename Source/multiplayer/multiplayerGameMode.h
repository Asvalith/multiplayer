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

protected:
	virtual void BeginPlay() override;

	/** Authoritative number of rack slots that must be activated before victory. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Coop|Objective", meta = (ClampMin = "1"))
	int32 RequiredKeys = 4;

private:
	/** Makes simultaneous restart clicks from both victory screens idempotent. */
	bool bRestartTravelRequested = false;
};
