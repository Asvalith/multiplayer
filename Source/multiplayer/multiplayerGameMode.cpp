// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameMode.h"
#include "multiplayer.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"
#include "Player/multiplayerGASPlayerState.h"
#include "Engine/World.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerStateClass = AmultiplayerGASPlayerState::StaticClass();

	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}

void AmultiplayerGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>())
	{
		CoopState->ConfigureRequiredKeys(RequiredKeys);
		UE_LOG(
			LogTemp,
			Log,
			TEXT("Coop objective configured: RequiredKeys=%d RequiredPlayersInWinArea=2"),
			RequiredKeys);
	}
}

void AmultiplayerGameMode::RestartCoopGame()
{
	if (!HasAuthority())
	{
		return;
	}
	if (bRestartTravelRequested)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Verbose,
			TEXT("COOP_RESTART Phase=Ignored Reason=AlreadyRequested"));
		return;
	}

	const AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || !CoopState->GetObjectiveState().bGameWon)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("COOP_RESTART Phase=Ignored Reason=GameNotWon"));
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const FString CurrentMap = UWorld::RemovePIEPrefix(
		World->GetOutermost()->GetName());
	const FString TravelUrl = CurrentMap + TEXT("?listen");
	bRestartTravelRequested = true;
	if (!World->ServerTravel(TravelUrl))
	{
		bRestartTravelRequested = false;
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("COOP_RESTART Phase=TravelFailed Url=%s"),
			*TravelUrl);
		return;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_RESTART Phase=TravelRequested Url=%s"),
		*TravelUrl);
}
