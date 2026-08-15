// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "GameFramework/Pawn.h"
#include "multiplayer.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"

UmultiplayerVictoryPresenterComponent::UmultiplayerVictoryPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerVictoryPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	RefreshBinding();
}

void UmultiplayerVictoryPresenterComponent::RefreshBinding()
{
	AmultiplayerCoopGameState* PreviousGameState = CoopGameState;
	ClearBinding();

	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn == nullptr || !OwnerPawn->IsLocallyControlled())
	{
		return;
	}

	UWorld* World = GetWorld();
	CoopGameState = World != nullptr
		? World->GetGameState<AmultiplayerCoopGameState>()
		: nullptr;
	if (CoopGameState == nullptr)
	{
		return;
	}

	// A controller/player-state refresh in the same match must not display the
	// victory screen twice. A genuinely new replicated GameState starts a new
	// local presentation lifetime (including seamless-travel style reuse).
	if (CoopGameState != PreviousGameState)
	{
		bVictoryNotified = false;
	}

	CoopGameState->OnGameWon.AddUniqueDynamic(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameWon);

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_VICTORY_UI Phase=Bound Actor=%s GameState=%s AlreadyWon=%s Owner=Blueprint"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(CoopGameState),
		CoopGameState->GetObjectiveState().bGameWon ? TEXT("true") : TEXT("false"));

	if (CoopGameState->GetObjectiveState().bGameWon)
	{
		HandleGameWon();
	}
}

void UmultiplayerVictoryPresenterComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	ClearBinding();
	bVictoryNotified = false;
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerVictoryPresenterComponent::ClearBinding()
{
	if (IsValid(CoopGameState))
	{
		CoopGameState->OnGameWon.RemoveDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleGameWon);
	}

	CoopGameState = nullptr;
}

void UmultiplayerVictoryPresenterComponent::HandleGameWon()
{
	if (bVictoryNotified)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Verbose,
			TEXT("COOP_VICTORY_UI Phase=BlueprintEventIgnored Actor=%s Reason=AlreadyNotified"),
			*GetNameSafe(GetOwner()));
		return;
	}

	AmultiplayerCharacter* Character = Cast<AmultiplayerCharacter>(GetOwner());
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		return;
	}

	bVictoryNotified = true;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_VICTORY_UI Phase=BlueprintEvent Actor=%s Role=%s"),
		*GetNameSafe(Character),
		*UEnum::GetValueAsString(Character->GetLocalRole()));
	Character->ReceiveCoopGameWon();
}
