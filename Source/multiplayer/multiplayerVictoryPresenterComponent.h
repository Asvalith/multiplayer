// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;
class UUserWidget;

/** Local-only presentation layer that turns the replicated win state into a UMG screen. */
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

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	TSubclassOf<UUserWidget> VictoryWidgetClass;

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	bool bShowMouseCursor = true;

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	bool bSwitchToGameAndUIInput = true;

private:
	void ClearBinding();

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> VictoryWidget;
};
