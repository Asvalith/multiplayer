// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystem/Abilities/multiplayerAbilityPresentationInterface.h"
#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_HealTarget.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "TimerManager.h"
#include "multiplayerGameplayAbility.generated.h"

class UAbilitySystemComponent;
enum class EmultiplayerDamageIntentResult : uint8;

UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerGameplayAbility();

protected:
	virtual void EndAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

	void ExecutePredictedCue(const FGameplayTag& CueTag, FName AbilityName);
	void TrackPrediction(FName AbilityName);
	bool IsPredictionLabEnabled() const;
	void ApplyPredictionLabPendingEffect();
	void LogPredictionState(const TCHAR* Phase, FName AbilityName, int16 PredictionKey) const;
	void BeginAbilityPresentation(const FGameplayTag& AbilityTag);
	void CompleteAbilityPresentation();
	void RejectAbilityPresentation(const FGameplayTag& AbilityTag, int16 PredictionKey);
	int16 GetActivePresentationPredictionKey() const { return ActivePresentationPredictionKey; }

	/** Optional asset references filled by an Ability Blueprint after C++ freeze. */
	UPROPERTY(EditDefaultsOnly, Category = "GAS|Presentation")
	FmultiplayerAbilityMontageConfig PresentationMontage;

private:
	void DispatchAbilityPresentation(
		EmultiplayerAbilityPresentationPhase Phase,
		const FGameplayTag& AbilityTag,
		int16 PredictionKey) const;
	void StopPresentationMontage(float BlendOutSeconds);
	static FGameplayTag ResolveAbilityPresentationTag(FName AbilityName);

	void HandlePredictionRejected(
		FName AbilityName,
		FGameplayAbilitySpecHandle SpecHandle,
		FPredictionKey ActivationPredictionKey,
		FPredictionKey ActionPredictionKey);
	void HandlePredictionCaughtUp(
		FName AbilityName,
		FGameplayAbilitySpecHandle SpecHandle,
		int16 PredictionKey);

	FGameplayTag ActivePresentationTag;
	TSet<int16> RejectedPresentationKeys;
	int16 ActivePresentationPredictionKey = 0;
	bool bPresentationStarted = false;
	bool bPresentationCompleted = false;
	bool bPresentationRejected = false;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerDamageAbility : public UmultiplayerGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerDamageAbility();

protected:
	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
	virtual void EndAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility,
		bool bWasCancelled) override;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Damage", meta = (ClampMin = "0"))
	float DamageAmount = 25.0f;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Damage", meta = (ClampMin = "0"))
	float TargetRange = 600.0f;

private:
	UPROPERTY(Transient)
	TObjectPtr<class UmultiplayerAbilityTask_TargetActor> ActiveTargetTask;

	UFUNCTION()
	void HandleTargetData(const FGameplayAbilityTargetDataHandle& TargetData);
	void HandleDamageIntentResult(
		uint32 ShotId,
		EmultiplayerDamageIntentResult Result);
	void HandleDamageIntentResultTimeout();
	void ClearDamageIntentResultWait();

	bool IsResolvedTargetStillValid(
		AActor* TargetActor,
		UAbilitySystemComponent*& OutTargetASC) const;

	FDelegateHandle DamageIntentResultHandle;
	FTimerHandle DamageIntentResultTimeoutHandle;
	uint32 PendingDamageIntentShotId = 0;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerHealAbility : public UmultiplayerGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerHealAbility();

protected:
	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing", meta = (ClampMin = "0"))
	float HealingAmount = 30.0f;

	/** Maximum authority-validated avatar-to-ally distance. */
	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing|Targeting", meta = (ClampMin = "0", Units = "cm"))
	float HealTargetRange = 800.0f;

	/** Client-side crosshair acquisition radius; authority still validates the actor. */
	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing|Targeting", meta = (ClampMin = "0", Units = "cm"))
	float HealTargetSweepRadius = 45.0f;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing|Targeting")
	bool bAllowSelfTarget = true;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing|Targeting", meta = (EditCondition = "bAllowSelfTarget"))
	bool bFallbackToSelf = true;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Healing|Targeting")
	bool bRequireHealLineOfSight = true;

	/** Presentation-only hook. It never grants authority to alter the target or heal amount. */
	UFUNCTION(BlueprintImplementableEvent, Category = "GAS|Healing|Presentation", meta = (DisplayName = "On Heal Target Previewed"))
	void K2_OnHealTargetPreviewed(AActor* TargetActor);

private:
	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerAbilityTask_HealTarget> ActiveHealTargetTask;

	void HandleHealTargetData(
		const FGameplayAbilityTargetDataHandle& TargetData,
		EmultiplayerHealTargetResult Result);

	void HandleHealAuthorityResult(bool bAccepted);

	bool IsResolvedHealTargetStillValid(
		AActor* TargetActor,
		UAbilitySystemComponent*& OutTargetASC) const;

	TWeakObjectPtr<AActor> PendingHealTarget;
};

UCLASS()
class MULTIPLAYER_API UmultiplayerImmunityAbility : public UmultiplayerGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerImmunityAbility();

protected:
	virtual void ActivateAbility(
		const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;
};
