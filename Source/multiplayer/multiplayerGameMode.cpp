// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameMode.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"
#include "Engine/World.h"
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
