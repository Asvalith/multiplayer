// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "TimerManager.h"
#include "multiplayerGASDeveloperHarnessComponent.generated.h"

class AmultiplayerGASTargetDummy;
class UInputComponent;
class UmultiplayerAbilitySystemComponent;

/**
 * Non-shipping fixture for the M5/M6 two-process GAS experiments.
 *
 * Keeping the fixture on a replicated default actor component preserves the
 * owning-client RPC route without making the gameplay character own test
 * targets, test mutations, timers, or automation state.
 */
UCLASS(ClassGroup = (Developer))
class MULTIPLAYER_API UmultiplayerGASDeveloperHarnessComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerGASDeveloperHarnessComponent();

	/** Supplies the persistent PlayerState ASC whenever its avatar is initialized. */
	void OnAbilitySystemReady(UmultiplayerAbilitySystemComponent* InAbilitySystemComponent);

	/** Binds 7/8/9 only when the process explicitly uses -GASDeveloperControls. */
	void BindDeveloperInput(UInputComponent* PlayerInputComponent);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	void RequestBaselineEnemyTarget();
	void RequestBaselineEnemyDamage();
	void RequestArmNextImmunityPredictionRejection();
	void TriggerAbilityInput(const FGameplayTag& InputTag);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestBaselineEnemyTarget();

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestBaselineEnemyDamage();

	void TryStartGASAutomation();
	void RunNextGASM5AutomationStep();
	void RunNextGASM6AutomationStep();
	void RunNextGASM6IntentAutomationStep();
	void ScheduleNextGASM5AutomationStep(float DelaySeconds);
	bool AimGASM5AutomationAtTarget();
	void LogGASM6Snapshot(const TCHAR* Phase) const;
	void FailGASM6Automation(const TCHAR* Reason);
	bool IsGASM6AutomationTimedOut() const;

	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	/** Server-only target reference used by the baseline damage helper. */
	UPROPERTY(Transient)
	TObjectPtr<AmultiplayerGASTargetDummy> BaselineEnemyTarget;

	FTimerHandle GASAutomationTimer;
	int32 GASM5AutomationStep = 0;
	int32 GASM6AutomationStep = 0;
	int32 GASM6IntentAutomationStep = 0;
	uint32 GASM6AutomationTrialId = 6000;
	uint32 GASM6IntentResultSerialBefore = 0;
	float GASM6AutomationDeadlineSeconds = 0.0f;
};
