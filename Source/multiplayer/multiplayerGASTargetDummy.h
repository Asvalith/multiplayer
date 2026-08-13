// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayCueInterface.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "TimerManager.h"
#include "multiplayerGASTargetDummy.generated.h"

class UStaticMeshComponent;
class UPointLightComponent;
class UmultiplayerAbilitySystemComponent;
class UmultiplayerAttributeSet;
struct FOnAttributeChangeData;

/**
 * Asset-free hostile GAS target used only by the M0 cooperative combat baseline.
 * It lets both players validate damage without introducing friendly fire.
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerGASTargetDummy : public AActor, public IAbilitySystemInterface, public ImultiplayerCoopTeamAgentInterface, public IGameplayCueInterface
{
	GENERATED_BODY()

public:
	AmultiplayerGASTargetDummy();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual int32 GetCoopTeamId_Implementation() const override { return CoopTeamId; }
	virtual void GameplayCueDefaultHandler(
		EGameplayCueEvent::Type EventType,
		const FGameplayCueParameters& Parameters) override;

	UFUNCTION(BlueprintPure, Category = "GAS|Baseline")
	float GetHealth() const;

	void ResetForBaseline(const FVector& NewLocation);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UFUNCTION()
	void OnRep_CoopTeamId();

	void ApplyTeamIdentity();
	void HandleHealthChanged(const FOnAttributeChangeData& ChangeData);
	void UpdateBaselineVisuals(float CurrentHealth);
	void ShowGameplayCueFlash(const FLinearColor& Color, float Intensity, float Duration);
	void ClearGameplayCueFlash();
	void SetVulnerabilityCueVisible(bool bVisible);
	void RefreshGameplayCueState();
	void PositionGameplayCueFlashFromImpact(const FVector& ImpactImpulse);

	UPROPERTY(VisibleAnywhere, Category = "GAS|Baseline")
	TObjectPtr<UStaticMeshComponent> TargetMesh;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Baseline")
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Baseline")
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueFlashLight;

	UPROPERTY(VisibleAnywhere, Category = "GAS|Cue")
	TObjectPtr<UPointLightComponent> GameplayCueStateLight;

	FTimerHandle GameplayCueFlashTimer;
	bool bVulnerabilityCueActive = false;
	bool bDeathCueActive = false;

	UPROPERTY(ReplicatedUsing = OnRep_CoopTeamId)
	int32 CoopTeamId = MultiplayerTeams::Enemies;
};
