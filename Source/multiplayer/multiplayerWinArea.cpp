// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerWinArea.h"

#include "multiplayerCoopGameState.h"
#include "Components/BoxComponent.h"
#include "GameFramework/Character.h"

AmultiplayerWinArea::AmultiplayerWinArea()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	WinTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("WinTrigger"));
	SetRootComponent(WinTrigger);
	WinTrigger->SetBoxExtent(FVector(150.0f));
	WinTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WinTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	WinTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	WinTrigger->OnComponentBeginOverlap.AddDynamic(
		this,
		&AmultiplayerWinArea::HandleBeginOverlap);
	WinTrigger->OnComponentEndOverlap.AddDynamic(
		this,
		&AmultiplayerWinArea::HandleEndOverlap);
}

void AmultiplayerWinArea::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		return;
	}

	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}
}

void AmultiplayerWinArea::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}

	for (const TWeakObjectPtr<ACharacter>& Player : PlayersInside)
	{
		if (Player.IsValid())
		{
			Player->OnDestroyed.RemoveDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		}
	}

	PlayersInside.Reset();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerWinArea::HandleBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character != nullptr && Character->IsPlayerControlled())
	{
		PlayersInside.Add(Character);
		Character->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		EvaluateWinCondition();
	}
}

void AmultiplayerWinArea::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		PlayersInside.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
	}
}

void AmultiplayerWinArea::HandlePlayerDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(DestroyedActor))
	{
		PlayersInside.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		EvaluateWinCondition();
	}
}

void AmultiplayerWinArea::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	EvaluateWinCondition();
}

void AmultiplayerWinArea::RemoveInvalidPlayers()
{
	for (auto Iterator = PlayersInside.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator->IsValid())
		{
			Iterator.RemoveCurrent();
		}
	}
}

void AmultiplayerWinArea::EvaluateWinCondition()
{
	if (!HasAuthority() || CoopGameState == nullptr)
	{
		return;
	}

	RemoveInvalidPlayers();
	UE_LOG(
		LogTemp,
		Log,
		TEXT("WinArea[%s] Evaluate: Players=%d RequiredPlayers=%d KeysComplete=%s"),
		*GetName(),
		PlayersInside.Num(),
		RequiredPlayers,
		CoopGameState->IsObjectiveComplete() ? TEXT("true") : TEXT("false"));
	if (PlayersInside.Num() >= RequiredPlayers
		&& CoopGameState->IsObjectiveComplete())
	{
		CoopGameState->TryCompleteGame();
	}
}
