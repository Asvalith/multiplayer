// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameMode.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerCoopPlayerController.h"
#include "multiplayerKeySocket.h"
#include "multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerControllerClass = AmultiplayerCoopPlayerController::StaticClass();

	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != nullptr)
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
		FmultiplayerCoopObjectiveState InitialState;
		InitialState.RequiredKeys = ResolveRequiredKeys();
		CoopState->ApplyAuthoritativeState(InitialState);
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Coop objective configured: RequiredKeys=%d"),
			InitialState.RequiredKeys);
	}
}

bool AmultiplayerGameMode::RegisterActivatedKey()
{
	if (!HasAuthority())
	{
		return false;
	}

	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr
		|| CoopState->GetObjectiveState().bGameWon
		|| CoopState->IsObjectiveComplete())
	{
		return false;
	}

	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	++NewState.ActivatedKeys;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

bool AmultiplayerGameMode::TryCompleteCoopGame(
	int32 CurrentPlayers,
	int32 RequiredPlayers)
{
	if (!HasAuthority() || CurrentPlayers < FMath::Max(1, RequiredPlayers))
	{
		return false;
	}

	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr
		|| CoopState->GetObjectiveState().bGameWon
		|| !CoopState->IsObjectiveComplete())
	{
		return false;
	}

	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	NewState.bGameWon = true;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

void AmultiplayerGameMode::RestartCoopGame()
{
	if (!HasAuthority())
	{
		return;
	}
	if (bRestartTravelRequested)
	{
		UE_LOG(LogMultiplayer, Verbose, TEXT("COOP_RESTART Phase=Ignored Reason=AlreadyRequested"));
		return;
	}

	const AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || !CoopState->GetObjectiveState().bGameWon)
	{
		UE_LOG(LogMultiplayer, Warning, TEXT("COOP_RESTART Phase=Ignored Reason=GameNotWon"));
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
		UE_LOG(LogMultiplayer, Error, TEXT("COOP_RESTART Phase=TravelFailed Url=%s"), *TravelUrl);
		return;
	}

	UE_LOG(LogMultiplayer, Display, TEXT("COOP_RESTART Phase=TravelRequested Url=%s"), *TravelUrl);
}

int32 AmultiplayerGameMode::ResolveRequiredKeys() const
{
	int32 PlacedSocketCount = 0;
	for (TActorIterator<AmultiplayerKeySocket> SocketIt(GetWorld()); SocketIt; ++SocketIt)
	{
		++PlacedSocketCount;
	}

	return PlacedSocketCount > 0
		? PlacedSocketCount
		: FMath::Max(1, RequiredKeys);
}
