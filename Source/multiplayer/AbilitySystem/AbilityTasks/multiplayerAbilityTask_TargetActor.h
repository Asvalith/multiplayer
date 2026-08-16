// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "AbilitySystem/multiplayerGameplayAbilityTargetData.h"
#include "multiplayerAbilityTask_TargetActor.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerTargetDataReadyEvent,
	const FGameplayAbilityTargetDataHandle&,
	TargetData);

/**
 * Collects local aim intent and sends it under the activation prediction key. Authority
 * resolves that intent with one server trace; the ability only rechecks lightweight
 * commit-time target invariants before applying the authoritative GameplayEffect.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerAbilityTask_TargetActor : public UAbilityTask
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = "GAS|AbilityTasks", meta = (HidePin = "OwningAbility", DefaultToSelf = "OwningAbility", BlueprintInternalUseOnly = "true"))
	static UmultiplayerAbilityTask_TargetActor* CreateTargetActorTask(
		UGameplayAbility* OwningAbility,
		float MaxRange);

	virtual void Activate() override;
	virtual void OnDestroy(bool bInOwnerFinished) override;
	uint32 GetResolvedShotId() const { return ResolvedShotId; }

	UPROPERTY(BlueprintAssignable)
	FmultiplayerTargetDataReadyEvent ValidData;

private:
	void SendLocalTargetData();
	void OnTargetDataReplicated(
		const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ActivationTag);
	void HandleRemoteTargetDataTimeout();
	void ProcessAuthorityIntent(
		const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ActivationTag,
		bool bConsumeReplicatedData);
	EmultiplayerDamageIntentResult ResolveAuthorityIntent(
		const FmultiplayerGameplayAbilityTargetData_DamageIntent& Intent,
		FHitResult& OutServerHit) const;
	void ReportAuthorityIntentResult(
		uint32 ShotId,
		EmultiplayerDamageIntentResult Result) const;
	bool FindCrosshairHostileHit(FHitResult& OutHitResult) const;

	float TargetRange = 0.0f;
	uint32 ResolvedShotId = 0;
	FTimerHandle RemoteTargetDataTimeoutHandle;
};
