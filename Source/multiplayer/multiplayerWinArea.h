// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerWinArea.generated.h"

class ACharacter;
class AmultiplayerCoopGameState;
class UBoxComponent;

/** Completes the match when the objective is ready and enough distinct players are inside. */
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
	void HandleBeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void HandleEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

	UFUNCTION()
	void HandlePlayerDestroyed(AActor* DestroyedActor);

private:
	void RemoveInvalidPlayers();
	void EvaluateWinCondition();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Win")
	TObjectPtr<UBoxComponent> WinTrigger;

	UPROPERTY(EditAnywhere, Category = "Coop|Win", meta = (ClampMin = "1"))
	int32 RequiredPlayers = 2;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	TSet<TWeakObjectPtr<ACharacter>> PlayersInside;
};
