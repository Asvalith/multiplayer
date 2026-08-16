// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/Guardian/multiplayerGuardianAIController.h"

#include "AI/Guardian/multiplayerGuardianCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Navigation/PathFollowingComponent.h"
#include "TimerManager.h"
#include "multiplayer.h"

AmultiplayerGuardianAIController::AmultiplayerGuardianAIController()
{
	PrimaryActorTick.bCanEverTick = false;
	bAttachToPawn = true;
}

void AmultiplayerGuardianAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	GuardianCharacter = Cast<AmultiplayerGuardianCharacter>(InPawn);
	if (!HasAuthority() || !GuardianCharacter.IsValid())
	{
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("GUARDIAN_AI_POSSESS_REJECT Controller=%s Pawn=%s Authority=%s"),
			*GetName(),
			*GetNameSafe(InPawn),
			HasAuthority() ? TEXT("true") : TEXT("false"));
		return;
	}

	GuardianCharacter->SetSpawnAnchor(InPawn->GetActorLocation());
	StartDecisionLoop();
}

void AmultiplayerGuardianAIController::OnUnPossess()
{
	StopDecisionLoop();
	GuardianCharacter.Reset();
	Super::OnUnPossess();
}

void AmultiplayerGuardianAIController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	StopDecisionLoop();
	GuardianCharacter.Reset();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerGuardianAIController::StartDecisionLoop()
{
	StopDecisionLoop();

	AmultiplayerGuardianCharacter* Guardian = GuardianCharacter.Get();
	UWorld* World = GetWorld();
	if (Guardian == nullptr || World == nullptr)
	{
		return;
	}

	const float Interval = FMath::Max(Guardian->GetAIDecisionInterval(), 0.05f);
	World->GetTimerManager().SetTimer(
		DecisionTimer,
		this,
		&AmultiplayerGuardianAIController::EvaluateGuardian,
		Interval,
		true,
		0.05f);
}

void AmultiplayerGuardianAIController::StopDecisionLoop()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DecisionTimer);
	}
	StopMovement();
}

void AmultiplayerGuardianAIController::EvaluateGuardian()
{
	AmultiplayerGuardianCharacter* Guardian = GuardianCharacter.Get();
	if (!HasAuthority() || Guardian == nullptr || Guardian->IsGuardianDead())
	{
		StopDecisionLoop();
		return;
	}

	Guardian->PruneInvalidChannelers();

	const EmultiplayerGuardianAIState State = Guardian->GetGuardianState();
	if (State == EmultiplayerGuardianAIState::Windup
		|| State == EmultiplayerGuardianAIState::Attack
		|| State == EmultiplayerGuardianAIState::Cooldown)
	{
		return;
	}

	const FVector GuardianLocation = Guardian->GetActorLocation();
	const float AnchorDistance = FVector::Dist(
		GuardianLocation,
		Guardian->GetSpawnAnchor());
	if (AnchorDistance > Guardian->GetLeashRadius())
	{
		Guardian->SetAuthoritativeTarget(nullptr);
		Guardian->SetAuthoritativeState(EmultiplayerGuardianAIState::Leash);
		MoveToLocation(
			Guardian->GetSpawnAnchor(),
			75.0f,
			true,
			true,
			false,
			true);
		return;
	}

	AActor* Target = Guardian->GetCurrentTarget();
	if (!Guardian->IsTargetCandidate(Target, Guardian->GetLoseTargetRadius()))
	{
		Guardian->SetAuthoritativeTarget(nullptr);
		Target = FindBestTarget(*Guardian);
		Guardian->SetAuthoritativeTarget(Target);
	}

	if (Target == nullptr)
	{
		if (AnchorDistance > 100.0f)
		{
			Guardian->SetAuthoritativeState(EmultiplayerGuardianAIState::Leash);
			MoveToLocation(
				Guardian->GetSpawnAnchor(),
				75.0f,
				true,
				true,
				false,
				true);
		}
		else
		{
			StopMovement();
			Guardian->SetAuthoritativeState(EmultiplayerGuardianAIState::Acquire);
		}
		return;
	}

	const float TargetDistance = FVector::Dist(GuardianLocation, Target->GetActorLocation());
	if (TargetDistance <= Guardian->GetAttackRange()
		&& Guardian->HasClearLineTo(Target))
	{
		StopMovement();
		Guardian->BeginAuthoritativeAttack();
		return;
	}

	Guardian->SetAuthoritativeState(EmultiplayerGuardianAIState::Chase);
	MoveToActor(
		Target,
		FMath::Max(Guardian->GetAttackRange() * 0.8f, 50.0f),
		true,
		true,
		true,
		nullptr,
		true);
}

AActor* AmultiplayerGuardianAIController::FindBestTarget(
	AmultiplayerGuardianCharacter& Guardian) const
{
	// Channeling players are a deliberate priority.  This makes the co-op
	// objective strategically visible without trusting a client target request.
	if (AActor* Channeler = FindBestCandidateFrom(
		Guardian,
		Guardian.GetActiveChannelers(),
		Guardian.GetLoseTargetRadius()))
	{
		return Channeler;
	}

	TArray<TObjectPtr<AActor>> PawnCandidates;
	if (UWorld* World = Guardian.GetWorld())
	{
		for (TActorIterator<APawn> It(World); It; ++It)
		{
			APawn* Candidate = *It;
			if (Candidate != &Guardian)
			{
				PawnCandidates.Add(Candidate);
			}
		}
	}

	return FindBestCandidateFrom(
		Guardian,
		PawnCandidates,
		Guardian.GetAcquisitionRadius());
}

AActor* AmultiplayerGuardianAIController::FindBestCandidateFrom(
	AmultiplayerGuardianCharacter& Guardian,
	const TArray<TObjectPtr<AActor>>& Candidates,
	float MaxDistance) const
{
	AActor* BestTarget = nullptr;
	float BestDistanceSquared = FMath::Square(FMath::Max(MaxDistance, 0.0f));
	for (AActor* Candidate : Candidates)
	{
		if (!Guardian.IsTargetCandidate(Candidate, MaxDistance))
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared(
			Guardian.GetActorLocation(),
			Candidate->GetActorLocation());
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			BestTarget = Candidate;
		}
	}
	return BestTarget;
}
