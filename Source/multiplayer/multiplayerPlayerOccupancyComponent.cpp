// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerPlayerOccupancyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"

UmultiplayerPlayerOccupancyComponent::UmultiplayerPlayerOccupancyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerPlayerOccupancyComponent::BindTrigger(
	UPrimitiveComponent* InTrigger,
	bool bInRequirePlayerControlledCharacter)
{
	UnbindTrigger();
	BoundTrigger = InTrigger;
	bRequirePlayerControlledCharacter = bInRequirePlayerControlledCharacter;

	AActor* Owner = GetOwner();
	if (BoundTrigger == nullptr || Owner == nullptr)
	{
		return;
	}

	if (!Owner->HasAuthority())
	{
		BoundTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	BoundTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
	BoundTrigger->OnComponentEndOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);
}

void UmultiplayerPlayerOccupancyComponent::UnbindTrigger()
{
	if (BoundTrigger != nullptr)
	{
		BoundTrigger->OnComponentBeginOverlap.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
		BoundTrigger->OnComponentEndOverlap.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);
	}

	BoundTrigger = nullptr;
	ClearOccupants();
}

int32 UmultiplayerPlayerOccupancyComponent::GetPlayerCount() const
{
	int32 PlayerCount = 0;
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid() && Entry.Value > 0)
		{
			++PlayerCount;
		}
	}
	return PlayerCount;
}

void UmultiplayerPlayerOccupancyComponent::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid() && Entry.Value > 0)
		{
			OutCharacters.Add(Entry.Key.Get());
		}
	}
}

void UmultiplayerPlayerOccupancyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	UnbindTrigger();
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	AddOccupant(OtherActor);
}

void UmultiplayerPlayerOccupancyComponent::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	RemoveOccupant(OtherActor);
}

void UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed(
	AActor* DestroyedActor)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(DestroyedActor);
	if (Character == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	OverlapCounts.Remove(Character);
	Character->OnDestroyed.RemoveDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

ACharacter* UmultiplayerPlayerOccupancyComponent::GetValidOccupant(
	AActor* OtherActor) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return nullptr;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr
		|| (bRequirePlayerControlledCharacter && !Character->IsPlayerControlled()))
	{
		return nullptr;
	}

	return Character;
}

void UmultiplayerPlayerOccupancyComponent::AddOccupant(AActor* OtherActor)
{
	ACharacter* Character = GetValidOccupant(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	int32& OverlapCount = OverlapCounts.FindOrAdd(Character);
	++OverlapCount;
	if (OverlapCount == 1)
	{
		Character->OnDestroyed.AddUniqueDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

void UmultiplayerPlayerOccupancyComponent::RemoveOccupant(AActor* OtherActor)
{
	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	int32* OverlapCount = OverlapCounts.Find(Character);
	if (OverlapCount == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	--(*OverlapCount);
	if (*OverlapCount <= 0)
	{
		OverlapCounts.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

void UmultiplayerPlayerOccupancyComponent::BroadcastIfPlayerCountChanged(
	int32 PreviousPlayerCount)
{
	const int32 NewPlayerCount = GetPlayerCount();
	if (NewPlayerCount != PreviousPlayerCount)
	{
		OnOccupancyChanged.Broadcast(NewPlayerCount);
	}
}

void UmultiplayerPlayerOccupancyComponent::ClearOccupants()
{
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid())
		{
			Entry.Key->OnDestroyed.RemoveDynamic(
				this,
				&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
		}
	}
	OverlapCounts.Reset();
}
