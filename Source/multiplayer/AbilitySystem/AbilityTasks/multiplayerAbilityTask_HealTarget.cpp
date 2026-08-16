// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_HealTarget.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameFramework/PlayerController.h"
#include "GameplayPrediction.h"
#include "multiplayer.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "Team/multiplayerTeamLibrary.h"
#include "TimerManager.h"

namespace
{
	constexpr float HealTargetDataTimeoutSeconds = 5.0f;
}

const TCHAR* GetMultiplayerHealTargetResultName(
	EmultiplayerHealTargetResult Result)
{
	switch (Result)
	{
	case EmultiplayerHealTargetResult::Accepted:
		return TEXT("Accepted");
	case EmultiplayerHealTargetResult::InvalidSchema:
		return TEXT("InvalidSchema");
	case EmultiplayerHealTargetResult::NoTarget:
		return TEXT("NoTarget");
	case EmultiplayerHealTargetResult::SourceDead:
		return TEXT("SourceDead");
	case EmultiplayerHealTargetResult::SelfTargetDisabled:
		return TEXT("SelfTargetDisabled");
	case EmultiplayerHealTargetResult::UnknownTeam:
		return TEXT("UnknownTeam");
	case EmultiplayerHealTargetResult::NotFriendly:
		return TEXT("NotFriendly");
	case EmultiplayerHealTargetResult::MissingAbilitySystem:
		return TEXT("MissingAbilitySystem");
	case EmultiplayerHealTargetResult::TargetDead:
		return TEXT("TargetDead");
	case EmultiplayerHealTargetResult::OutOfRange:
		return TEXT("OutOfRange");
	case EmultiplayerHealTargetResult::Obstructed:
		return TEXT("Obstructed");
	case EmultiplayerHealTargetResult::TargetDataTimeout:
		return TEXT("TargetDataTimeout");
	case EmultiplayerHealTargetResult::AuthorityRejected:
		return TEXT("AuthorityRejected");
	default:
		return TEXT("Unknown");
	}
}

EmultiplayerHealTargetResult ValidateMultiplayerHealTargetDataSchema(
	const FGameplayAbilityTargetDataHandle& TargetData,
	AActor*& OutCandidate)
{
	OutCandidate = nullptr;
	if (TargetData.Num() != 1 || TargetData.Get(0) == nullptr)
	{
		return EmultiplayerHealTargetResult::InvalidSchema;
	}

	const FGameplayAbilityTargetData* Data = TargetData.Get(0);
	if (Data->GetScriptStruct()
		!= FGameplayAbilityTargetData_ActorArray::StaticStruct())
	{
		return EmultiplayerHealTargetResult::InvalidSchema;
	}

	const TArray<TWeakObjectPtr<AActor>> Actors = Data->GetActors();
	if (Actors.Num() != 1)
	{
		return EmultiplayerHealTargetResult::InvalidSchema;
	}

	OutCandidate = Actors[0].Get();
	return IsValid(OutCandidate)
		? EmultiplayerHealTargetResult::Accepted
		: EmultiplayerHealTargetResult::NoTarget;
}

UmultiplayerAbilityTask_HealTarget*
UmultiplayerAbilityTask_HealTarget::CreateHealTargetTask(
	UGameplayAbility* OwningAbility,
	float MaxRange,
	float SweepRadius,
	bool bAllowSelf,
	bool bFallbackToSelf,
	bool bRequireLineOfSight)
{
	UmultiplayerAbilityTask_HealTarget* Task =
		NewAbilityTask<UmultiplayerAbilityTask_HealTarget>(OwningAbility);
	Task->TargetRange = FMath::Max(MaxRange, 0.0f);
	Task->TargetSweepRadius = FMath::Max(SweepRadius, 0.0f);
	Task->bAllowSelfTarget = bAllowSelf;
	Task->bUseSelfFallback = bAllowSelf && bFallbackToSelf;
	Task->bCheckLineOfSight = bRequireLineOfSight;
	return Task;
}

void UmultiplayerAbilityTask_HealTarget::Activate()
{
	if (Ability == nullptr || !AbilitySystemComponent.IsValid())
	{
		EndTask();
		return;
	}

	const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo();
	if (ActorInfo == nullptr)
	{
		EndTask();
		return;
	}

	if (ActorInfo->IsLocallyControlled())
	{
		SendLocalTargetData();
		return;
	}

	const FGameplayAbilitySpecHandle SpecHandle = GetAbilitySpecHandle();
	const FPredictionKey ActivationPredictionKey = GetActivationPredictionKey();
	AbilitySystemComponent->AbilityTargetDataSetDelegate(
		SpecHandle,
		ActivationPredictionKey).AddUObject(
			this,
			&UmultiplayerAbilityTask_HealTarget::OnTargetDataReplicated);

	if (!AbilitySystemComponent->CallReplicatedTargetDataDelegatesIfSet(
		SpecHandle,
		ActivationPredictionKey))
	{
		SetWaitingOnRemotePlayerData();
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				RemoteTargetDataTimeoutHandle,
				this,
				&UmultiplayerAbilityTask_HealTarget::HandleRemoteTargetDataTimeout,
				HealTargetDataTimeoutSeconds,
				false);
		}
	}
}

void UmultiplayerAbilityTask_HealTarget::OnDestroy(bool bInOwnerFinished)
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->AbilityTargetDataSetDelegate(
			GetAbilitySpecHandle(),
			GetActivationPredictionKey()).RemoveAll(this);
		AbilitySystemComponent->AbilityReplicatedEventDelegate(
			EAbilityGenericReplicatedEvent::GameCustom1,
			GetAbilitySpecHandle(),
			GetActivationPredictionKey()).RemoveAll(this);
		AbilitySystemComponent->AbilityReplicatedEventDelegate(
			EAbilityGenericReplicatedEvent::GameCustom2,
			GetAbilitySpecHandle(),
			GetActivationPredictionKey()).RemoveAll(this);
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RemoteTargetDataTimeoutHandle);
		World->GetTimerManager().ClearTimer(AuthorityResultTimeoutHandle);
	}

	Super::OnDestroy(bInOwnerFinished);
}

void UmultiplayerAbilityTask_HealTarget::SendLocalTargetData()
{
	FScopedPredictionWindow PredictionWindow(AbilitySystemComponent.Get());

	AActor* Candidate = FindLocalCandidate();
	if (!IsValid(Candidate))
	{
		BroadcastResult(
			FGameplayAbilityTargetDataHandle(),
			EmultiplayerHealTargetResult::NoTarget);
		if (IsValid(this))
		{
			EndTask();
		}
		return;
	}

	const FGameplayAbilityTargetDataHandle TargetData =
		MakeActorTargetData(Candidate);
	const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo();
	if (ActorInfo != nullptr && ActorInfo->IsNetAuthority())
	{
		ProcessAuthorityTarget(TargetData, FGameplayTag(), false);
		return;
	}

	CallOrAddReplicatedDelegate(
		EAbilityGenericReplicatedEvent::GameCustom1,
		FSimpleMulticastDelegate::FDelegate::CreateUObject(
			this,
			&UmultiplayerAbilityTask_HealTarget::HandleAuthorityAccepted));
	CallOrAddReplicatedDelegate(
		EAbilityGenericReplicatedEvent::GameCustom2,
		FSimpleMulticastDelegate::FDelegate::CreateUObject(
			this,
			&UmultiplayerAbilityTask_HealTarget::HandleAuthorityRejected));

	AbilitySystemComponent->CallServerSetReplicatedTargetData(
		GetAbilitySpecHandle(),
		GetActivationPredictionKey(),
		TargetData,
		FGameplayTag(),
		AbilitySystemComponent->ScopedPredictionKey);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			AuthorityResultTimeoutHandle,
			this,
			&UmultiplayerAbilityTask_HealTarget::HandleAuthorityResultTimeout,
			HealTargetDataTimeoutSeconds,
			false);
	}

	BroadcastResult(TargetData, EmultiplayerHealTargetResult::Accepted);
}

void UmultiplayerAbilityTask_HealTarget::SendAuthorityResultToOwner(
	bool bAccepted)
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	if (!AbilitySystemComponent.IsValid()
		|| ActorInfo == nullptr
		|| !ActorInfo->IsNetAuthority()
		|| ActorInfo->IsLocallyControlled())
	{
		return;
	}

	AbilitySystemComponent->ClientSetReplicatedEvent(
		bAccepted
			? EAbilityGenericReplicatedEvent::GameCustom1
			: EAbilityGenericReplicatedEvent::GameCustom2,
		GetAbilitySpecHandle(),
		GetActivationPredictionKey());
}

void UmultiplayerAbilityTask_HealTarget::OnTargetDataReplicated(
	const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ActivationTag)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RemoteTargetDataTimeoutHandle);
	}
	ProcessAuthorityTarget(TargetData, ActivationTag, true);
}

void UmultiplayerAbilityTask_HealTarget::HandleRemoteTargetDataTimeout()
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->ConsumeClientReplicatedTargetData(
			GetAbilitySpecHandle(),
			GetActivationPredictionKey());
	}

	LogAuthorityResult(nullptr, EmultiplayerHealTargetResult::TargetDataTimeout);
	BroadcastResult(
		FGameplayAbilityTargetDataHandle(),
		EmultiplayerHealTargetResult::TargetDataTimeout);
	if (IsValid(this))
	{
		EndTask();
	}
}

void UmultiplayerAbilityTask_HealTarget::HandleAuthorityAccepted()
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->ConsumeGenericReplicatedEvent(
			EAbilityGenericReplicatedEvent::GameCustom1,
			GetAbilitySpecHandle(),
			GetActivationPredictionKey());
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AuthorityResultTimeoutHandle);
	}
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		AuthorityResult.Broadcast(true);
	}
	if (IsValid(this))
	{
		EndTask();
	}
}

void UmultiplayerAbilityTask_HealTarget::HandleAuthorityRejected()
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->ConsumeGenericReplicatedEvent(
			EAbilityGenericReplicatedEvent::GameCustom2,
			GetAbilitySpecHandle(),
			GetActivationPredictionKey());
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(AuthorityResultTimeoutHandle);
	}
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		AuthorityResult.Broadcast(false);
	}
	if (IsValid(this))
	{
		EndTask();
	}
}

void UmultiplayerAbilityTask_HealTarget::HandleAuthorityResultTimeout()
{
	UE_LOG(
		LogMultiplayerGAS,
		Warning,
		TEXT("GAS_HEAL_TARGET Phase=OwningClientTimeout Spec=%s PredictionKey=%s"),
		*GetAbilitySpecHandle().ToString(),
		*GetActivationPredictionKey().ToString());
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		AuthorityResult.Broadcast(false);
	}
	if (IsValid(this))
	{
		EndTask();
	}
}

void UmultiplayerAbilityTask_HealTarget::ProcessAuthorityTarget(
	const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ActivationTag,
	bool bConsumeReplicatedData)
{
	if (bConsumeReplicatedData)
	{
		AbilitySystemComponent->ConsumeClientReplicatedTargetData(
			GetAbilitySpecHandle(),
			GetActivationPredictionKey());
	}

	AActor* Candidate = nullptr;
	EmultiplayerHealTargetResult Result =
		ValidateMultiplayerHealTargetDataSchema(TargetData, Candidate);
	if (ActivationTag.IsValid())
	{
		Result = EmultiplayerHealTargetResult::InvalidSchema;
	}
	if (Result == EmultiplayerHealTargetResult::Accepted)
	{
		Result = ValidateAuthorityCandidate(Candidate);
	}

	const FGameplayAbilityTargetDataHandle ResolvedTargetData =
		Result == EmultiplayerHealTargetResult::Accepted
			? MakeActorTargetData(Candidate)
			: FGameplayAbilityTargetDataHandle();
	LogAuthorityResult(Candidate, Result);
	BroadcastResult(ResolvedTargetData, Result);
	if (IsValid(this))
	{
		EndTask();
	}
}

EmultiplayerHealTargetResult
UmultiplayerAbilityTask_HealTarget::ValidateAuthorityCandidate(
	AActor* Candidate) const
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	AActor* SourceActor = ActorInfo != nullptr
		? ActorInfo->AvatarActor.Get()
		: nullptr;
	if (!IsValid(SourceActor) || !AbilitySystemComponent.IsValid())
	{
		return EmultiplayerHealTargetResult::MissingAbilitySystem;
	}

	if (AbilitySystemComponent->HasMatchingGameplayTag(
			MultiplayerGameplayTags::State_Dead)
		|| AbilitySystemComponent->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return EmultiplayerHealTargetResult::SourceDead;
	}
	if (!IsValid(Candidate) || Candidate->IsActorBeingDestroyed())
	{
		return EmultiplayerHealTargetResult::NoTarget;
	}
	if (Candidate == SourceActor && !bAllowSelfTarget)
	{
		return EmultiplayerHealTargetResult::SelfTargetDisabled;
	}

	const int32 SourceTeam = UmultiplayerTeamLibrary::ResolveTeamId(SourceActor);
	const int32 TargetTeam = UmultiplayerTeamLibrary::ResolveTeamId(Candidate);
	if (SourceTeam == MultiplayerTeams::NoTeam
		|| TargetTeam == MultiplayerTeams::NoTeam)
	{
		return EmultiplayerHealTargetResult::UnknownTeam;
	}
	if (SourceTeam != TargetTeam)
	{
		return EmultiplayerHealTargetResult::NotFriendly;
	}

	UAbilitySystemComponent* TargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate);
	if (!IsValid(TargetASC))
	{
		return EmultiplayerHealTargetResult::MissingAbilitySystem;
	}
	if (TargetASC->HasMatchingGameplayTag(MultiplayerGameplayTags::State_Dead)
		|| TargetASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return EmultiplayerHealTargetResult::TargetDead;
	}

	if (Candidate != SourceActor
		&& FVector::DistSquared(
			SourceActor->GetActorLocation(),
			Candidate->GetActorLocation()) > FMath::Square(TargetRange))
	{
		return EmultiplayerHealTargetResult::OutOfRange;
	}
	if (bCheckLineOfSight
		&& Candidate != SourceActor
		&& !HasLineOfSight(SourceActor, Candidate))
	{
		return EmultiplayerHealTargetResult::Obstructed;
	}

	return EmultiplayerHealTargetResult::Accepted;
}

AActor* UmultiplayerAbilityTask_HealTarget::FindLocalCandidate() const
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	AActor* SourceActor = ActorInfo != nullptr
		? ActorInfo->AvatarActor.Get()
		: nullptr;
	UWorld* World = SourceActor != nullptr ? SourceActor->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return nullptr;
	}

	FVector ViewLocation;
	FRotator ViewRotation;
	SourceActor->GetActorEyesViewPoint(ViewLocation, ViewRotation);
	if (APlayerController* PlayerController = ActorInfo->PlayerController.Get())
	{
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	}

	const float CameraOffset = FVector::Distance(
		ViewLocation,
		SourceActor->GetActorLocation());
	const FVector TraceEnd = ViewLocation
		+ ViewRotation.Vector() * (TargetRange + CameraOffset);
	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(GASHealTargetPreview),
		false,
		SourceActor);
	TArray<FHitResult> HitResults;
	World->SweepMultiByChannel(
		HitResults,
		ViewLocation,
		TraceEnd,
		FQuat::Identity,
		ECC_Visibility,
		FCollisionShape::MakeSphere(TargetSweepRadius),
		QueryParams);

	const int32 SourceTeam = UmultiplayerTeamLibrary::ResolveTeamId(SourceActor);
	for (const FHitResult& HitResult : HitResults)
	{
		AActor* Candidate = HitResult.GetActor();
		if (!IsValid(Candidate)
			|| Candidate == SourceActor
			|| Candidate->IsActorBeingDestroyed()
			|| SourceTeam == MultiplayerTeams::NoTeam
			|| UmultiplayerTeamLibrary::ResolveTeamId(Candidate) != SourceTeam)
		{
			continue;
		}

		UAbilitySystemComponent* CandidateASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate);
		if (!IsValid(CandidateASC)
			|| CandidateASC->HasMatchingGameplayTag(
				MultiplayerGameplayTags::State_Dead)
			|| CandidateASC->GetNumericAttribute(
				UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f
			|| FVector::DistSquared(
				SourceActor->GetActorLocation(),
				Candidate->GetActorLocation()) > FMath::Square(TargetRange)
			|| (bCheckLineOfSight
				&& !HasLineOfSight(SourceActor, Candidate)))
		{
			continue;
		}

		return Candidate;
	}

	return bUseSelfFallback ? SourceActor : nullptr;
}

bool UmultiplayerAbilityTask_HealTarget::HasLineOfSight(
	AActor* SourceActor,
	AActor* TargetActor) const
{
	UWorld* World = IsValid(SourceActor) ? SourceActor->GetWorld() : nullptr;
	if (World == nullptr || !IsValid(TargetActor))
	{
		return false;
	}

	FVector SourceViewLocation;
	FRotator SourceViewRotation;
	SourceActor->GetActorEyesViewPoint(SourceViewLocation, SourceViewRotation);
	FVector TargetViewLocation;
	FRotator TargetViewRotation;
	TargetActor->GetActorEyesViewPoint(TargetViewLocation, TargetViewRotation);

	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(GASHealTargetLineOfSight),
		false,
		SourceActor);
	FHitResult HitResult;
	const bool bBlocked = World->LineTraceSingleByChannel(
		HitResult,
		SourceViewLocation,
		TargetViewLocation,
		ECC_Visibility,
		QueryParams);
	return !bBlocked || HitResult.GetActor() == TargetActor;
}

FGameplayAbilityTargetDataHandle
UmultiplayerAbilityTask_HealTarget::MakeActorTargetData(
	AActor* TargetActor) const
{
	FGameplayAbilityTargetData_ActorArray* ActorData =
		new FGameplayAbilityTargetData_ActorArray();
	ActorData->TargetActorArray.Add(TargetActor);
	return FGameplayAbilityTargetDataHandle(ActorData);
}

void UmultiplayerAbilityTask_HealTarget::BroadcastResult(
	const FGameplayAbilityTargetDataHandle& TargetData,
	EmultiplayerHealTargetResult Result)
{
	if (ShouldBroadcastAbilityTaskDelegates())
	{
		TargetReady.Broadcast(TargetData, Result);
	}
}

void UmultiplayerAbilityTask_HealTarget::LogAuthorityResult(
	AActor* Candidate,
	EmultiplayerHealTargetResult Result) const
{
	const FString Message = FString::Printf(
		TEXT("GAS_HEAL_TARGET Phase=AuthorityResolved Result=%s Source=%s Target=%s Range=%.1f LOS=%s Spec=%s PredictionKey=%s"),
		GetMultiplayerHealTargetResultName(Result),
		*GetNameSafe(Ability != nullptr
			? Ability->GetCurrentActorInfo()->AvatarActor.Get()
			: nullptr),
		*GetNameSafe(Candidate),
		TargetRange,
		bCheckLineOfSight ? TEXT("Required") : TEXT("Disabled"),
		*GetAbilitySpecHandle().ToString(),
		*GetActivationPredictionKey().ToString());
	if (Result == EmultiplayerHealTargetResult::Accepted)
	{
		UE_LOG(LogMultiplayerGAS, Display, TEXT("%s"), *Message);
	}
	else
	{
		UE_LOG(LogMultiplayerGAS, Warning, TEXT("%s"), *Message);
	}
}
