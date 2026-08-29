// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerWinArea.h"

#include "Components/BoxComponent.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerGameMode.h"
#include "multiplayerLog.h"
#include "multiplayerPlayerOccupancyComponent.h"

AmultiplayerWinArea::AmultiplayerWinArea()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;

	WinTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("WinTrigger"));
	SetRootComponent(WinTrigger);
	WinTrigger->SetBoxExtent(FVector(150.0f));
	WinTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WinTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	WinTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
}

void AmultiplayerWinArea::BeginPlay()
{
	Super::BeginPlay();

	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(WinTrigger);

	if (!HasAuthority())
	{
		return;
	}

	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}
	EvaluateWinCondition();
}

void AmultiplayerWinArea::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AmultiplayerWinArea::HandleOccupancyChanged(int32 PlayerCount)
{
	EvaluateWinCondition();
}

void AmultiplayerWinArea::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	EvaluateWinCondition();
}

void AmultiplayerWinArea::EvaluateWinCondition()
{
	if (!HasAuthority() || CoopGameState == nullptr)
	{
		return;
	}

	const int32 PlayerCount = PlayerOccupancy->GetPlayerCount();
	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("WinArea[%s] Players=%d Required=%d ObjectiveComplete=%s"),
		*GetName(),
		PlayerCount,
		RequiredPlayers,
		CoopGameState->IsObjectiveComplete() ? TEXT("true") : TEXT("false"));

	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
	{
		CoopGameMode->TryCompleteCoopGame(PlayerCount, RequiredPlayers);
	}
}
