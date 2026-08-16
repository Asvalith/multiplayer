// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "GameplayCueInterface.h"
#include "TimerManager.h"
#include "multiplayerGASCuePresenterComponent.generated.h"

class UPointLightComponent;
class UmultiplayerAbilitySystemComponent;

/** Selects one visual owner so native debug lights and formal Cue assets never double-play. */
UENUM(BlueprintType)
enum class EmultiplayerCuePresentationOwner : uint8
{
	NativeDebugFallback,
	GameplayCueAssets
};

/**
 * Local presentation adapter for the project's asset-free GameplayCue lights.
 *
 * Gameplay state remains owned by GAS. This component only translates Cue
 * events and replicated death state into the two existing point lights.
 */
UCLASS(ClassGroup = (GAS))
class MULTIPLAYER_API UmultiplayerGASCuePresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerGASCuePresenterComponent();

	/** Binds the existing flash and persistent-state lights owned by the Actor. */
	void BindLights(
		UPointLightComponent* InGameplayCueFlashLight,
		UPointLightComponent* InGameplayCueStateLight);

	/** Subscribes to prediction reconciliation without coupling Ability code to UI. */
	void BindAbilitySystem(UmultiplayerAbilitySystemComponent* InAbilitySystemComponent);

	/** Returns true when the Cue belongs to this presenter. */
	bool HandleGameplayCue(
		EGameplayCueEvent::Type EventType,
		const FGameplayCueParameters& Parameters);

	/** Reconciles persistent presentation with the replicated death state. */
	void ApplyDeathState(bool bNewDeadState);

	/** Idempotently removes the local prediction-lab pending presentation. */
	void ReconcilePredictionLabPendingPresentation(
		const TCHAR* Outcome,
		int16 PredictionKey);

	/** Clears timers, state flags, and both bound lights. */
	void ClearPresentation();

	bool IsPredictionPendingActive() const
	{
		return bGameplayCuePredictionPendingActive;
	}

	EmultiplayerCuePresentationOwner GetPresentationOwner() const
	{
		return PresentationOwner;
	}

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void ShowGameplayCueFlash(
		const FLinearColor& Color,
		float Intensity,
		float Duration);
	void ClearGameplayCueFlash();
	void SetGameplayCueState(const FLinearColor& Color, float Intensity);
	void ClearGameplayCueState();
	void HandlePersistentCueEvent(
		bool& bState,
		EGameplayCueEvent::Type EventType);
	void RefreshGameplayCueState();
	void PositionGameplayCueFlashFromImpact(const FVector& ImpactImpulse);
	void HandlePredictionLabReconciled(bool bRejected, int16 PredictionKey);
	void UnbindAbilitySystem();

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> GameplayCueFlashLight;

	UPROPERTY(Transient)
	TObjectPtr<UPointLightComponent> GameplayCueStateLight;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	/**
	 * Native lights are an asset-free debug fallback. Switch the formal Character
	 * Blueprint to GameplayCueAssets after Niagara/audio Cue assets are connected.
	 * Prediction.Pending remains native because it is a development-only probe.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "GAS|Cue", meta = (AllowPrivateAccess = "true"))
	EmultiplayerCuePresentationOwner PresentationOwner =
		EmultiplayerCuePresentationOwner::NativeDebugFallback;

	FTimerHandle GameplayCueFlashTimer;
	FDelegateHandle PredictionLabReconciledHandle;
	bool bGameplayCueImmunityActive = false;
	bool bGameplayCueVulnerabilityActive = false;
	bool bGameplayCueDeathActive = false;
	bool bGameplayCuePredictionPendingActive = false;
};
