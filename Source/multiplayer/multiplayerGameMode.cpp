// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameMode.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/SoftObjectPath.h"

AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();

	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}

void AmultiplayerGameMode::HostLANGame()
{
	UWorld* World = GetWorld();
	if (World == nullptr || !HasAuthority())
	{
		return;
	}

	const FString TravelMapPath = FSoftObjectPath(LANMapPath).GetLongPackageName();
	const FString TravelUrl = (TravelMapPath.IsEmpty() ? LANMapPath : TravelMapPath) + TEXT("?listen");

	World->ServerTravel(TravelUrl);
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

void AmultiplayerGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	LogConnectionSnapshot(
		TEXT("PostLogin"),
		NewPlayer,
		NewPlayer != nullptr ? NewPlayer->GetPawn() : nullptr);
}

void AmultiplayerGameMode::Logout(AController* Exiting)
{
	LogConnectionSnapshot(
		TEXT("Logout"),
		Exiting,
		Exiting != nullptr ? Exiting->GetPawn() : nullptr);

	Super::Logout(Exiting);
}

void AmultiplayerGameMode::LogConnectionSnapshot(
	const TCHAR* Phase,
	const AController* Controller,
	const APawn* Pawn) const
{
	const APlayerState* PlayerState =
		Controller != nullptr ? Controller->GetPlayerState<APlayerState>() : nullptr;
	const AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	const FmultiplayerCoopObjectiveState* Objective =
		CoopState != nullptr ? &CoopState->GetObjectiveState() : nullptr;
	const int32 ConnectedPlayers =
		CoopState != nullptr ? CoopState->PlayerArray.Num() : 0;

	UE_LOG(
		LogTemp,
		Display,
		TEXT("COOP_CONNECTION Phase=%s Player=%s PlayerId=%d Pawn=%s PlayerStates=%d Keys=%d/%d GameWon=%s"),
		Phase,
		PlayerState != nullptr ? *PlayerState->GetPlayerName() : TEXT("None"),
		PlayerState != nullptr ? PlayerState->GetPlayerId() : INDEX_NONE,
		Pawn != nullptr ? *Pawn->GetName() : TEXT("None"),
		ConnectedPlayers,
		Objective != nullptr ? Objective->ActivatedKeys : 0,
		Objective != nullptr ? Objective->RequiredKeys : RequiredKeys,
		Objective != nullptr && Objective->bGameWon ? TEXT("true") : TEXT("false"));
}

void AmultiplayerGameMode::JoinLANGame()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	APlayerController* PlayerController = World->GetFirstPlayerController();
	if (PlayerController != nullptr)
	{
		PlayerController->ClientTravel(LANServerAddress, TRAVEL_Absolute);
	}
}

void AmultiplayerGameMode::RestartCoopGame()
{
	if (!HasAuthority())
	{
		return;
	}

	const AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || !CoopState->GetObjectiveState().bGameWon)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const FString CurrentMap = UWorld::RemovePIEPrefix(
		World->GetOutermost()->GetName());
	World->ServerTravel(CurrentMap + TEXT("?listen"));
}
