// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/Tasks/AbilityTask.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "multiplayerAbilityTask_TargetActor.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerTargetDataReadyEvent,
	const FGameplayAbilityTargetDataHandle&,
	TargetData);

/**
 * Collects a locally selected player target and sends it to the server under the
 * activation prediction key. The consuming ability must still validate it.
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

	UPROPERTY(BlueprintAssignable)
	FmultiplayerTargetDataReadyEvent ValidData;

private:
	void SendLocalTargetData();
	void OnTargetDataReplicated(
		const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ActivationTag);
	AActor* FindNearestPlayerTarget() const;

	float TargetRange = 0.0f;
};
