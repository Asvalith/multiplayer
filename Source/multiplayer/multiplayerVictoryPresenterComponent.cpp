// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "multiplayerCoopPlayerController.h"
#include "multiplayerCoopGameState.h"

UmultiplayerVictoryPresenterComponent::UmultiplayerVictoryPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerVictoryPresenterComponent::RefreshBinding()
{
	AmultiplayerCoopGameState* PreviousGameState = CoopGameState;
	ClearBinding();

	const AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwner());
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
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

	if (CoopGameState != PreviousGameState)
	{
		bVictoryNotified = false;
	}

	CoopGameState->OnGameWon.AddUniqueDynamic(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameWon);

	if (CoopGameState->GetObjectiveState().bGameWon)
	{
		HandleGameWon();
	}
}

void UmultiplayerVictoryPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
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
		return;
	}

	AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwner());
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
	{
		return;
	}

	bVictoryNotified = true;
	PlayerController->ReceiveCoopGameWon();
}
