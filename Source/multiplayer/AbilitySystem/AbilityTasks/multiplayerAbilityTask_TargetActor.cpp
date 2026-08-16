// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameplayPrediction.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "multiplayer.h"
#include "Team/multiplayerTeamLibrary.h"

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
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					RemoteTargetDataTimeoutHandle,
					this,
					&UmultiplayerAbilityTask_TargetActor::HandleRemoteTargetDataTimeout,
					5.0f,
					false);
			}
		}
	}
}

void UmultiplayerAbilityTask_TargetActor::OnDestroy(bool bInOwnerFinished)
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->AbilityTargetDataSetDelegate(
			GetAbilitySpecHandle(),
			GetActivationPredictionKey()).RemoveAll(this);
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RemoteTargetDataTimeoutHandle);
	}

	Super::OnDestroy(bInOwnerFinished);
}

void UmultiplayerAbilityTask_TargetActor::SendLocalTargetData()
{
	FScopedPredictionWindow PredictionWindow(AbilitySystemComponent.Get());

	FGameplayAbilityTargetDataHandle IntentHandle;
	FGameplayAbilityTargetDataHandle LocalTargetDataHandle;
	FHitResult TargetHit;
	const bool bHasLocalTarget = FindCrosshairHostileHit(TargetHit);
	const FGameplayAbilityActorInfo* ActorInfo = Ability->GetCurrentActorInfo();
	AActor* AvatarActor = ActorInfo != nullptr ? ActorInfo->AvatarActor.Get() : nullptr;
	UmultiplayerAbilitySystemComponent* ProjectASC =
		Cast<UmultiplayerAbilitySystemComponent>(AbilitySystemComponent.Get());
	if (AvatarActor == nullptr || ProjectASC == nullptr)
	{
		if (ShouldBroadcastAbilityTaskDelegates())
		{
			// Route setup failure through the ability's normal cancellation path so
			// it cannot remain active after this one-shot task ends.
			ValidData.Broadcast(FGameplayAbilityTargetDataHandle());
		}
		EndTask();
		return;
	}

	FVector EyeOrigin;
	FRotator EyeRotation;
	AvatarActor->GetActorEyesViewPoint(EyeOrigin, EyeRotation);
	if (APlayerController* PlayerController = ActorInfo->PlayerController.Get())
	{
		EyeRotation = PlayerController->GetControlRotation();
	}
	const FVector AimDirection = bHasLocalTarget
		? (TargetHit.ImpactPoint - EyeOrigin).GetSafeNormal()
		: EyeRotation.Vector().GetSafeNormal();
	const EmultiplayerDamageIntentTestMutation TestMutation =
		ProjectASC->ConsumeDamageIntentLabMutation();
	FmultiplayerGameplayAbilityTargetData_DamageIntent* Intent =
		new FmultiplayerGameplayAbilityTargetData_DamageIntent();
	Intent->ShotId = TestMutation
		== EmultiplayerDamageIntentTestMutation::DuplicateLastShotId
		? ProjectASC->GetLastAllocatedDamageShotId()
		: ProjectASC->AllocateLocalDamageShotId();
	Intent->Origin = EyeOrigin;
	Intent->Direction = AimDirection;
	if (const UWorld* World = AvatarActor->GetWorld())
	{
		const AGameStateBase* GameState = World->GetGameState();
		Intent->ClientFireServerTimeSeconds = GameState != nullptr
			? GameState->GetServerWorldTimeSeconds()
			: World->GetTimeSeconds();
	}
	switch (TestMutation)
	{
	case EmultiplayerDamageIntentTestMutation::ForgedOrigin:
		Intent->Origin = EyeOrigin + FVector(5000.0f, 0.0f, 0.0f);
		break;
	case EmultiplayerDamageIntentTestMutation::OppositeDirection:
		Intent->Direction = -AimDirection;
		break;
	case EmultiplayerDamageIntentTestMutation::TooOld:
		Intent->ClientFireServerTimeSeconds -= 10.0f;
		break;
	case EmultiplayerDamageIntentTestMutation::Future:
		Intent->ClientFireServerTimeSeconds += 10.0f;
		break;
	case EmultiplayerDamageIntentTestMutation::CleanMiss:
		Intent->Direction = FQuat(
			FVector::UpVector,
			FMath::DegreesToRadians(25.0f)).RotateVector(AimDirection).GetSafeNormal();
		break;
	default:
		break;
	}
	IntentHandle.Add(Intent);
	ResolvedShotId = Intent->ShotId;
	if (bHasLocalTarget)
	{
		LocalTargetDataHandle.Add(
			new FGameplayAbilityTargetData_SingleTargetHit(TargetHit));
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_INTENT Phase=LocalIntent ShotId=%u Mutation=%s Origin=%s Direction=%s LocalTarget=%s PredictionKey=%s"),
		Intent->ShotId,
		GetMultiplayerDamageIntentTestMutationName(TestMutation),
		*FVector(Intent->Origin).ToCompactString(),
		*FVector(Intent->Direction).ToCompactString(),
		*GetNameSafe(TargetHit.GetActor()),
		*AbilitySystemComponent->ScopedPredictionKey.ToString());

	if (ActorInfo != nullptr && ActorInfo->IsNetAuthority())
	{
		ProcessAuthorityIntent(IntentHandle, FGameplayTag(), false);
		return;
	}

	AbilitySystemComponent->CallServerSetReplicatedTargetData(
		GetAbilitySpecHandle(),
		GetActivationPredictionKey(),
		IntentHandle,
		FGameplayTag(),
		AbilitySystemComponent->ScopedPredictionKey);

	if (ShouldBroadcastAbilityTaskDelegates())
	{
		ValidData.Broadcast(LocalTargetDataHandle);
	}
	EndTask();
}

void UmultiplayerAbilityTask_TargetActor::OnTargetDataReplicated(
	const FGameplayAbilityTargetDataHandle& TargetData,
	FGameplayTag ActivationTag)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RemoteTargetDataTimeoutHandle);
	}
	ProcessAuthorityIntent(TargetData, ActivationTag, true);
}

void UmultiplayerAbilityTask_TargetActor::HandleRemoteTargetDataTimeout()
{
	if (AbilitySystemComponent.IsValid())
	{
		AbilitySystemComponent->ConsumeClientReplicatedTargetData(
			GetAbilitySpecHandle(),
			GetActivationPredictionKey());
		ReportAuthorityIntentResult(
			0,
			EmultiplayerDamageIntentResult::TargetDataTimeout);
	}

	if (ShouldBroadcastAbilityTaskDelegates())
	{
		ValidData.Broadcast(FGameplayAbilityTargetDataHandle());
	}
	EndTask();
}

void UmultiplayerAbilityTask_TargetActor::ProcessAuthorityIntent(
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

	uint32 ShotId = 0;
	EmultiplayerDamageIntentResult Result =
		EmultiplayerDamageIntentResult::InvalidSchema;
	FGameplayAbilityTargetDataHandle ResolvedTargetData;
	const FmultiplayerGameplayAbilityTargetData_DamageIntent* Intent = nullptr;
	Result = ValidateMultiplayerDamageIntentSchema(TargetData, Intent);
	if (ActivationTag.IsValid())
	{
		Result = EmultiplayerDamageIntentResult::InvalidSchema;
	}
	if (Result == EmultiplayerDamageIntentResult::Accepted && Intent != nullptr)
	{
		ShotId = Intent->ShotId;
		ResolvedShotId = ShotId;
		FHitResult ServerHit;
		Result = ResolveAuthorityIntent(*Intent, ServerHit);
		if (Result == EmultiplayerDamageIntentResult::Accepted)
		{
			ResolvedTargetData.Add(
				new FGameplayAbilityTargetData_SingleTargetHit(ServerHit));
		}
	}

	if (Result != EmultiplayerDamageIntentResult::Accepted)
	{
		ReportAuthorityIntentResult(ShotId, Result);
	}

	if (ShouldBroadcastAbilityTaskDelegates())
	{
		ValidData.Broadcast(ResolvedTargetData);
	}
	EndTask();
}

EmultiplayerDamageIntentResult
UmultiplayerAbilityTask_TargetActor::ResolveAuthorityIntent(
	const FmultiplayerGameplayAbilityTargetData_DamageIntent& Intent,
	FHitResult& OutServerHit) const
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	AActor* AvatarActor = ActorInfo != nullptr ? ActorInfo->AvatarActor.Get() : nullptr;
	UWorld* World = AvatarActor != nullptr ? AvatarActor->GetWorld() : nullptr;
	UmultiplayerAbilitySystemComponent* ProjectASC =
		Cast<UmultiplayerAbilitySystemComponent>(AbilitySystemComponent.Get());
	if (World == nullptr || AvatarActor == nullptr || ProjectASC == nullptr)
	{
		return EmultiplayerDamageIntentResult::InvalidSchema;
	}
	FVector ServerOrigin;
	FRotator ServerEyeRotation;
	AvatarActor->GetActorEyesViewPoint(ServerOrigin, ServerEyeRotation);
	const double ServerNow = World->GetTimeSeconds();

	// ShotId is the business idempotency boundary. Once the schema has been
	// accepted, consume the request before any semantic gate or scene query so
	// the same request cannot be corrected/replayed to sample the server again.
	const EmultiplayerDamageIntentResult GuardResult =
		ProjectASC->TryConsumeDamageIntent(Intent.ShotId, ServerNow);
	if (GuardResult != EmultiplayerDamageIntentResult::Accepted)
	{
		return GuardResult;
	}

	if (ProjectASC->HasMatchingGameplayTag(MultiplayerGameplayTags::State_Dead)
		|| ProjectASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return EmultiplayerDamageIntentResult::SourceDead;
	}

	FmultiplayerDamageIntentServerContext ValidationContext;
	ValidationContext.ServerEyeOrigin = ServerOrigin;
	ValidationContext.ServerAimDirection = ServerEyeRotation.Vector();
	ValidationContext.ServerNowSeconds = ServerNow;
	const EmultiplayerDamageIntentResult FieldResult =
		ValidateMultiplayerDamageIntentFields(Intent, ValidationContext);
	if (FieldResult != EmultiplayerDamageIntentResult::Accepted)
	{
		return FieldResult;
	}
	const FVector ClientOrigin = FVector(Intent.Origin);
	const FVector ClientDirection = FVector(Intent.Direction);
	const double RequestAge = ServerNow
		- static_cast<double>(Intent.ClientFireServerTimeSeconds);

	const FVector TraceEnd = ServerOrigin
		+ ClientDirection.GetSafeNormal() * TargetRange;
	FCollisionQueryParams QueryParams(
		SCENE_QUERY_STAT(GASDamageAuthorityTrace),
		false,
		AvatarActor);
	const bool bHit = World->SweepSingleByChannel(
		OutServerHit,
		ServerOrigin,
		TraceEnd,
		FQuat::Identity,
		ECC_Visibility,
		FCollisionShape::MakeSphere(35.0f),
		QueryParams);
	if (!bHit)
	{
		return EmultiplayerDamageIntentResult::Miss;
	}

	AActor* Candidate = OutServerHit.GetActor();
	UAbilitySystemComponent* CandidateASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate);
	if (Candidate == nullptr || CandidateASC == nullptr)
	{
		return EmultiplayerDamageIntentResult::Miss;
	}
	if (Candidate == AvatarActor
		|| Candidate->IsActorBeingDestroyed()
		|| !UmultiplayerTeamLibrary::AreHostile(AvatarActor, Candidate)
		|| CandidateASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return EmultiplayerDamageIntentResult::InvalidTarget;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_INTENT Phase=AuthorityTrace ShotId=%u Age=%.3f OriginDelta=%.1f AimDot=%.3f Target=%s Impact=%s"),
		Intent.ShotId,
		RequestAge,
		FVector::Distance(ClientOrigin, ServerOrigin),
		FVector::DotProduct(
			ClientDirection.GetSafeNormal(),
			ServerEyeRotation.Vector().GetSafeNormal()),
		*GetNameSafe(Candidate),
		*OutServerHit.ImpactPoint.ToCompactString());
	return EmultiplayerDamageIntentResult::Accepted;
}

void UmultiplayerAbilityTask_TargetActor::ReportAuthorityIntentResult(
	uint32 ShotId,
	EmultiplayerDamageIntentResult Result) const
{
	UE_LOG(
		LogMultiplayerGAS,
		Warning,
		TEXT("GAS_M6_INTENT Phase=AuthorityRejected ShotId=%u Reason=%s Spec=%s PredictionKey=%s Avatar=%s"),
		ShotId,
		GetMultiplayerDamageIntentResultName(Result),
		*GetAbilitySpecHandle().ToString(),
		*GetActivationPredictionKey().ToString(),
		*GetNameSafe(Ability != nullptr
			? Ability->GetCurrentActorInfo()->AvatarActor.Get()
			: nullptr));
	if (UmultiplayerAbilitySystemComponent* ProjectASC =
		Cast<UmultiplayerAbilitySystemComponent>(AbilitySystemComponent.Get()))
	{
		ProjectASC->ClientDamageIntentResult(ShotId, Result);
	}
}

bool UmultiplayerAbilityTask_TargetActor::FindCrosshairHostileHit(
	FHitResult& OutHitResult) const
{
	const FGameplayAbilityActorInfo* ActorInfo =
		Ability != nullptr ? Ability->GetCurrentActorInfo() : nullptr;
	AActor* AvatarActor = ActorInfo != nullptr ? ActorInfo->AvatarActor.Get() : nullptr;
	UWorld* World = AvatarActor != nullptr ? AvatarActor->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return false;
	}

	FVector ViewLocation = AvatarActor->GetActorLocation();
	FRotator ViewRotation = AvatarActor->GetActorRotation();
	if (APlayerController* PlayerController = ActorInfo->PlayerController.Get())
	{
		PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
		if (FParse::Param(FCommandLine::Get(), TEXT("GASM5Auto"))
			|| FParse::Param(FCommandLine::Get(), TEXT("GASM6Auto")))
		{
			// The automation fixture changes control rotation immediately before input.
			// CameraManager updates on the following frame, so use the new control rotation
			// while preserving the normal sweep, team filtering and replicated TargetData path.
			ViewRotation = PlayerController->GetControlRotation();
		}
	}

	// TargetRange is a gameplay distance measured from the avatar. In third-person,
	// tracing only TargetRange from the camera shortens the usable range by the
	// spring-arm offset. Extend the client query by that offset; the server still
	// validates Avatar -> Target against TargetRange, so authority is unchanged.
	const float CameraToAvatarDistance = FVector::Distance(
		ViewLocation,
		AvatarActor->GetActorLocation());
	const FVector TraceEnd = ViewLocation
		+ ViewRotation.Vector() * (TargetRange + CameraToAvatarDistance);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GASCrosshairTarget), false, AvatarActor);
	TArray<FHitResult> HitResults;
	World->SweepMultiByChannel(
		HitResults,
		ViewLocation,
		TraceEnd,
		FQuat::Identity,
		ECC_Visibility,
		FCollisionShape::MakeSphere(35.0f),
		QueryParams);

	const bool bLogAutomationTrace =
		FParse::Param(FCommandLine::Get(), TEXT("GASM5Auto"))
		|| FParse::Param(FCommandLine::Get(), TEXT("GASM6Auto"));
	if (bLogAutomationTrace)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_TARGET_TRACE Phase=Sweep Avatar=%s Start=%s End=%s Hits=%d"),
			*GetNameSafe(AvatarActor),
			*ViewLocation.ToCompactString(),
			*TraceEnd.ToCompactString(),
			HitResults.Num());
	}

	for (const FHitResult& HitResult : HitResults)
	{
		AActor* Candidate = HitResult.GetActor();
		if (Candidate == nullptr
			|| Candidate == AvatarActor
			|| Candidate->IsActorBeingDestroyed()
			|| !UmultiplayerTeamLibrary::AreHostile(AvatarActor, Candidate))
		{
			continue;
		}

		UAbilitySystemComponent* CandidateASC =
			UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Candidate);
		if (CandidateASC == nullptr
			|| CandidateASC->GetNumericAttribute(
				UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
		{
			continue;
		}

		OutHitResult = HitResult;
		if (bLogAutomationTrace)
		{
			UE_LOG(
				LogMultiplayerGAS,
				Display,
				TEXT("GAS_TARGET_TRACE Phase=Selected Target=%s Location=%s"),
				*GetNameSafe(Candidate),
				*HitResult.ImpactPoint.ToCompactString());
		}
		return true;
	}

	if (bLogAutomationTrace)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_TARGET_TRACE Phase=NoHostileTarget Hits=%d"),
			HitResults.Num());
	}

	return false;
}
