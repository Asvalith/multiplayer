// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopPlayerController.h"

#include "multiplayerGameMode.h"
#include "multiplayerVictoryPresenterComponent.h"

AmultiplayerCoopPlayerController::AmultiplayerCoopPlayerController()
{
	VictoryPresenter = CreateDefaultSubobject<UmultiplayerVictoryPresenterComponent>(TEXT("VictoryPresenter"));
}

void AmultiplayerCoopPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();

	if (VictoryPresenter != nullptr)
	{
		VictoryPresenter->RefreshBinding();
	}
}

void AmultiplayerCoopPlayerController::RequestRestartCoopGame()
{
	if (IsLocalController())
	{
		ServerRestartCoopGame();
	}
}

bool AmultiplayerCoopPlayerController::ServerRestartCoopGame_Validate()
{
	return !IsActorBeingDestroyed();
}

void AmultiplayerCoopPlayerController::ServerRestartCoopGame_Implementation()
{
	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld() != nullptr ? GetWorld()->GetAuthGameMode<AmultiplayerGameMode>() : nullptr)
	{
		CoopGameMode->RestartCoopGame();
	}
}
