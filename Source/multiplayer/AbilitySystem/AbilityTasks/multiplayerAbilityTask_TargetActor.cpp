// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h"

#include "AbilitySystemComponent.h"
#include "EngineUtils.h"
#include "GameplayPrediction.h"
#include "multiplayerCharacter.h"

UmultiplayerAbilityTask_TargetActor* UmultiplayerAbilityTask_TargetActor::CreateTargetActorTask(
	UGameplayAbility* OwningAbility,
	float MaxRange)
{
	UmultiplayerAbilityTask_TargetActor* Task =
		NewAbilityTask<UmultiplayerAbilityTask_TargetActor>(OwningAbility);
	Task->TargetRange = FMath::Max(MaxRange, 0.0f);
	return Task;
}

void UmultiplayerAbilityTask_TargetActor::Activate()
{
	if (Ability == nullptr || AbilitySystemComponent == nullptr)
	{
		EndTask();
		return;
	}

	if (Ability->GetCurrentActorInfo()->IsLocallyControlled())
	{
		SendLocalTargetData();
	}
	else
	{
		const FGameplayAbilitySpecHandle SpecHandle = GetAbilitySpecHandle();
		const FPredictionKey ActivationPredictionKey = GetActivationPredictionKey();
		AbilitySystemComponent->AbilityTargetDataSetDelegate(
			SpecHandle,
			ActivationPredictionKey).AddUObject(
				this,
				&UmultiplayerAbilityTask_TargetActor::OnTargetDataReplicated);

		const bool bDataWasAlreadyReceived =
			AbilitySystemComponent->CallReplicatedTargetDataDelegatesIfSet(
				SpecHandle,
				ActivationPredictionKey);
		if (!bDataWasAlreadyReceived)
		{
			SetWaitingOnRemotePlayerData();
		}
	}
}

void UmultiplayerAbilityTask_TargetActor::SendLocalTargetData()
{
	FScopedPredictionWindow PredictionWindow(AbilitySystemComponent.Get());

	FGameplayAbilityTargetDataHandle TargetDataHandle;
	if (AActor* TargetActor = FindNearestPlayerTarget())
	{
		FGameplayAbilityTargetData_ActorArray* TargetData =
			new FGameplayAbilityTargetData_ActorArray();
		TargetData->TargetActorArray.Add(TargetActor);
		TargetDataHandle.Add(TargetData);
	}

	AbilitySystemComponent->ServerSetReplicatedTargetData(
		GetAbilitySpecHandle(),
		GetActivationPredictionKey(),
		TargetDataHandle,
		FGameplayTag(),
		AbilitySystemComponent->ScopedPredictionKey);

	if (ShouldBroadcastAbilityTaskDelegates())
	{
		ValidData.Broadcast(TargetDataHandle);
	}
}

void UmultiplayerAbilityTask_TargetActor::OnTargetDataReplicated(
	const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ActivationTag)
{
	AbilitySystemComponent->ConsumeClientReplicatedTargetData(
		GetAbilitySpecHandle(),
		GetActivationPredictionKey());

	if (ShouldBroadcastAbilityTaskDelegates())
	{
		ValidData.Broadcast(TargetData);
	}
}

AActor* UmultiplayerAbilityTask_TargetActor::FindNearestPlayerTarget() const
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	AActor* AvatarActor = ActorInfo != nullptr ? ActorInfo->AvatarActor.Get() : nullptr;
	UWorld* World = AvatarActor != nullptr ? AvatarActor->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return nullptr;
	}

	AActor* NearestTarget = nullptr;
	float NearestDistanceSquared = FMath::Square(TargetRange);
	for (TActorIterator<AmultiplayerCharacter> It(World); It; ++It)
	{
		AmultiplayerCharacter* Candidate = *It;
		if (Candidate == nullptr || Candidate == AvatarActor || Candidate->IsActorBeingDestroyed())
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared(
			AvatarActor->GetActorLocation(),
			Candidate->GetActorLocation());
		if (DistanceSquared <= NearestDistanceSquared)
		{
			NearestDistanceSquared = DistanceSquared;
			NearestTarget = Candidate;
		}
	}

	return NearestTarget;
}
