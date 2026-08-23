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

	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : PlayerOverlapCounts)
	{
		if (Entry.Key.IsValid())
		{
			Entry.Key->OnDestroyed.RemoveDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		}
	}

	PlayerOverlapCounts.Reset();
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
		int32& OverlapCount = PlayerOverlapCounts.FindOrAdd(Character);
		++OverlapCount;
		if (OverlapCount == 1)
		{
			Character->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		}
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
		int32* OverlapCount = PlayerOverlapCounts.Find(Character);
		if (OverlapCount == nullptr)
		{
			return;
		}

		--(*OverlapCount);
		if (*OverlapCount <= 0)
		{
			PlayerOverlapCounts.Remove(Character);
			Character->OnDestroyed.RemoveDynamic(this, &AmultiplayerWinArea::HandlePlayerDestroyed);
		}
		EvaluateWinCondition();
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
		PlayerOverlapCounts.Remove(Character);
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
	for (auto Iterator = PlayerOverlapCounts.CreateIterator(); Iterator; ++Iterator)
	{
		if (!Iterator.Key().IsValid() || Iterator.Value() <= 0)
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
		PlayerOverlapCounts.Num(),
		RequiredPlayers,
		CoopGameState->IsObjectiveComplete() ? TEXT("true") : TEXT("false"));
	if (PlayerOverlapCounts.Num() >= RequiredPlayers
		&& CoopGameState->IsObjectiveComplete())
	{
		CoopGameState->TryCompleteGame();
	}
}
