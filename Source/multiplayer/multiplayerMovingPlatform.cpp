// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerMovingPlatform.h"

#include "multiplayerTransporterComponent.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);

	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	SetRootComponent(PlatformMesh);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	Transporter = CreateDefaultSubobject<UmultiplayerTransporterComponent>(TEXT("Transporter"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlatformMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	ActivationVolume->OnComponentBeginOverlap.AddDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleBeginOverlap);
	ActivationVolume->OnComponentEndOverlap.AddDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleEndOverlap);

	OnRep_PlayerCount();
}

void AmultiplayerMovingPlatform::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationVolume->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleBeginOverlap);
	ActivationVolume->OnComponentEndOverlap.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleEndOverlap);

	for (const TWeakObjectPtr<ACharacter>& Occupant : Occupants)
	{
		if (Occupant.IsValid())
		{
			Occupant->OnDestroyed.RemoveDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		}
	}

	Occupants.Reset();
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerMovingPlatform::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerMovingPlatform, ReplicatedPlayerCount);
}

void AmultiplayerMovingPlatform::HandleBeginOverlap(
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
		Occupants.Add(Character);
		Character->OnDestroyed.AddUniqueDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		RefreshOccupancy();
	}
}

void AmultiplayerMovingPlatform::HandleEndOverlap(
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
		Occupants.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		RefreshOccupancy();
	}
}

void AmultiplayerMovingPlatform::HandleOccupantDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(DestroyedActor))
	{
		Occupants.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		RefreshOccupancy();
	}
}

void AmultiplayerMovingPlatform::RefreshOccupancy()
{
	RemoveInvalidOccupants();

	const int32 NewPlayerCount = Occupants.Num();
	if (ReplicatedPlayerCount != NewPlayerCount)
	{
		ReplicatedPlayerCount = NewPlayerCount;
		OnRep_PlayerCount();
		ForceNetUpdate();
	}

	Transporter->SetTransportActive(ReplicatedPlayerCount >= RequiredPlayers);
}

void AmultiplayerMovingPlatform::RemoveInvalidOccupants()
{
	for (auto It = Occupants.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void AmultiplayerMovingPlatform::OnRep_PlayerCount()
{
	OnPlatformOccupancyChanged.Broadcast(ReplicatedPlayerCount, RequiredPlayers);
	ReceivePlatformOccupancyChanged(ReplicatedPlayerCount, RequiredPlayers);
}
