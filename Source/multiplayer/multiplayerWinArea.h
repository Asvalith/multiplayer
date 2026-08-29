// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerWinArea.generated.h"

class AmultiplayerCoopGameState;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;

/**
 * Server-only rule adapter that asks GameMode to complete the cooperative match
 * when the replicated objective is ready and enough distinct players are inside.
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerWinArea : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerWinArea();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

private:
	void EvaluateWinCondition();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Win")
	TObjectPtr<UBoxComponent> WinTrigger;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Win")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	UPROPERTY(EditAnywhere, Category = "Coop|Win", meta = (ClampMin = "1"))
	int32 RequiredPlayers = 2;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
