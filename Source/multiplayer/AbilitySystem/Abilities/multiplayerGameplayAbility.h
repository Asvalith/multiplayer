// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/GameplayAbility.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "multiplayerGameplayAbility.generated.h"

UCLASS(Abstract)
class MULTIPLAYER_API UmultiplayerGameplayAbility : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UmultiplayerGameplayAbility();

protected:
	void ExecutePredictedCue(const FGameplayTag& CueTag, FName AbilityName);
	void TrackPrediction(FName AbilityName);
	bool IsPredictionLabEnabled() const;
	void ApplyPredictionLabPendingEffect();
	void LogPredictionState(const TCHAR* Phase, FName AbilityName, int16 PredictionKey) const;

private:
	void HandlePredictionRejected(
		FName AbilityName,
		FGameplayAbilitySpecHandle SpecHandle,
		FPredictionKey ActivationPredictionKey,
		FPredictionKey ActionPredictionKey);
	void HandlePredictionCaughtUp(
		FName AbilityName,
		FGameplayAbilitySpecHandle SpecHandle,
		int16 PredictionKey);
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

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Damage", meta = (ClampMin = "0"))
	float DamageAmount = 25.0f;

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Damage", meta = (ClampMin = "0"))
	float TargetRange = 600.0f;

private:
	UPROPERTY(Transient)
	TObjectPtr<class UmultiplayerAbilityTask_TargetActor> ActiveTargetTask;

	UFUNCTION()
	void HandleTargetData(const FGameplayAbilityTargetDataHandle& TargetData);

	bool IsServerTargetValid(AActor* TargetActor) const;
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
