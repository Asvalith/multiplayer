// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Blueprint/UserWidget.h"
#include "GameplayTagContainer.h"
#include "multiplayerGASHUDWidget.generated.h"

class AmultiplayerGASPlayerState;
class UProgressBar;
class UTextBlock;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerHUDAttributeEvent,
	float,
	CurrentValue,
	float,
	MaxValue);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerHUDTagStateEvent,
	FGameplayTag,
	StateTag,
	bool,
	bIsActive);

/**
 * Blueprint-facing GAS HUD model. It owns all delegate subscriptions so a
 * repeated ASC initialization or Pawn replacement cannot duplicate UI events.
 */
UCLASS(BlueprintType, Blueprintable)
class MULTIPLAYER_API UmultiplayerGASHUDWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "GAS|HUD")
	void InitializeWithPlayerState(AmultiplayerGASPlayerState* NewPlayerState);

	UFUNCTION(BlueprintCallable, Category = "GAS|HUD")
	void ClearPlayerStateBinding();

	UFUNCTION(BlueprintPure, Category = "GAS|HUD")
	AmultiplayerGASPlayerState* GetBoundPlayerState() const { return BoundPlayerState; }

	UPROPERTY(BlueprintAssignable, Category = "GAS|HUD")
	FmultiplayerHUDAttributeEvent OnHealthDisplayChanged;

	UPROPERTY(BlueprintAssignable, Category = "GAS|HUD")
	FmultiplayerHUDAttributeEvent OnEnergyDisplayChanged;

	UPROPERTY(BlueprintAssignable, Category = "GAS|HUD")
	FmultiplayerHUDTagStateEvent OnTagStateDisplayChanged;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|HUD")
	float CurrentHealth = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|HUD")
	float MaxHealth = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|HUD")
	float CurrentEnergy = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "GAS|HUD")
	float MaxEnergy = 0.0f;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;

	UFUNCTION(BlueprintImplementableEvent, Category = "GAS|HUD", meta = (DisplayName = "GAS HUD Initialized"))
	void BP_OnGASHUDInitialized();

private:
	UFUNCTION()
	void HandleHealthChanged(float OldValue, float NewValue);

	UFUNCTION()
	void HandleEnergyChanged(float OldValue, float NewValue);

	void HandleObservedTagChanged(const FGameplayTag Tag, int32 NewCount);
	void BroadcastCurrentValues();
	void RegisterObservedTag(const FGameplayTag& Tag);
	void BuildNativeFallbackLayout();
	void UpdateAttributePresentation();
	void UpdateTagPresentation();

	UPROPERTY(Transient)
	TObjectPtr<AmultiplayerGASPlayerState> BoundPlayerState;

	TMap<FGameplayTag, FDelegateHandle> ObservedTagHandles;
	TSet<FGameplayTag> ActiveObservedTags;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> HealthText;

	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> HealthBar;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> EnergyText;

	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> EnergyBar;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> StateText;
};
