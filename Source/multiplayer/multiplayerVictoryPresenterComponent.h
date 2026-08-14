// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;
class APlayerController;
class UButton;
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

	/** Read-only access for configuration validation without exposing mutation. */
	TSubclassOf<UUserWidget> GetVictoryWidgetClass() const { return VictoryWidgetClass; }
	FName GetRestartButtonName() const { return RestartButtonName; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleGameWon();

	UFUNCTION()
	void HandleRestartClicked();

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	TSubclassOf<UUserWidget> VictoryWidgetClass;

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	bool bShowMouseCursor = true;

	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	bool bSwitchToGameAndUIInput = true;

	/** Designer name of the restart button in the configured victory widget. */
	UPROPERTY(EditAnywhere, Category = "Coop|Victory")
	FName RestartButtonName = TEXT("\u91cd\u65b0\u5f00\u59cb");

private:
	void ClearBinding();

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> VictoryWidget;

	UPROPERTY(Transient)
	TObjectPtr<UButton> RestartButton;

	/** Controller whose presentation state was changed by this component. */
	TWeakObjectPtr<APlayerController> PresentationPlayerController;

	/** Only restore input state that this component actually changed. */
	bool bAppliedGameAndUIInput = false;
	bool bAppliedMouseCursorOverride = false;
	bool bPreviousMouseCursorState = false;
	bool bAppliedMouseCursorState = false;
	bool bRestartRequested = false;
};
