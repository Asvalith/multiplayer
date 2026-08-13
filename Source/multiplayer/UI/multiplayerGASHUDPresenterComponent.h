// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "multiplayerGASHUDPresenterComponent.generated.h"

class UmultiplayerGASHUDWidget;

/** Local-only owner of the GAS HUD widget and its Pawn lifecycle. */
UCLASS(ClassGroup = (GAS), meta = (BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerGASHUDPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerGASHUDPresenterComponent();

	UFUNCTION(BlueprintCallable, Category = "GAS|HUD")
	void RefreshBinding();

	UFUNCTION(BlueprintPure, Category = "GAS|HUD")
	UmultiplayerGASHUDWidget* GetHUDWidget() const { return HUDWidget; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void ReleaseWidget();

	UPROPERTY(EditDefaultsOnly, Category = "GAS|HUD")
	TSubclassOf<UmultiplayerGASHUDWidget> HUDWidgetClass;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerGASHUDWidget> HUDWidget;
};
