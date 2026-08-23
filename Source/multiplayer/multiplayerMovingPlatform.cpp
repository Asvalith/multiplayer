// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerMovingPlatform.h"

#include "multiplayerTransporterComponent.h"
#include "multiplayerPressurePlate.h"
#include "Components/BoxComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
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

	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);

	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->bEditableWhenInherited = true;
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	Transporter = CreateDefaultSubobject<UmultiplayerTransporterComponent>(TEXT("Transporter"));

	StartPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("StartPoint"));
	StartPoint->SetupAttachment(PlatformRoot);
	StartPoint->SetMobility(EComponentMobility::Movable);
	StartPoint->bEditableWhenInherited = true;
	CastChecked<UArrowComponent>(StartPoint)->ArrowColor = FColor::Red;

	TargetPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("TargetPoint"));
	TargetPoint->SetupAttachment(PlatformRoot);
	TargetPoint->SetMobility(EComponentMobility::Movable);
	TargetPoint->bEditableWhenInherited = true;
	TargetPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 500.0f));
	CastChecked<UArrowComponent>(TargetPoint)->ArrowColor = FColor::Green;

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

	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	ActivationVolume->OnComponentBeginOverlap.AddDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleBeginOverlap);
	ActivationVolume->OnComponentEndOverlap.AddDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleEndOverlap);

	if (ActivationPlate != nullptr)
	{
		ActivationPlate->OnPlateActiveChanged.AddUniqueDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
	}

	OnRep_PlayerCount();
	RefreshActivation();
}

void AmultiplayerMovingPlatform::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationVolume->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleBeginOverlap);
	ActivationVolume->OnComponentEndOverlap.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleEndOverlap);

	if (ActivationPlate != nullptr)
	{
		ActivationPlate->OnPlateActiveChanged.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
	}

	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OccupantOverlapCounts)
	{
		if (Entry.Key.IsValid())
		{
			Entry.Key->OnDestroyed.RemoveDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		}
	}

	OccupantOverlapCounts.Reset();
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
		int32& OverlapCount = OccupantOverlapCounts.FindOrAdd(Character);
		++OverlapCount;
		if (OverlapCount == 1)
		{
			Character->OnDestroyed.AddUniqueDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		}
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
		int32* OverlapCount = OccupantOverlapCounts.Find(Character);
		if (OverlapCount == nullptr)
		{
			return;
		}

		--(*OverlapCount);
		if (*OverlapCount <= 0)
		{
			OccupantOverlapCounts.Remove(Character);
			Character->OnDestroyed.RemoveDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		}
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
		OccupantOverlapCounts.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupantDestroyed);
		RefreshOccupancy();
	}
}

void AmultiplayerMovingPlatform::RefreshOccupancy()
{
	RemoveInvalidOccupants();

	const int32 NewPlayerCount = OccupantOverlapCounts.Num();
	if (ReplicatedPlayerCount != NewPlayerCount)
	{
		ReplicatedPlayerCount = NewPlayerCount;
		OnRep_PlayerCount();
		ForceNetUpdate();
	}

	RefreshActivation();
}

void AmultiplayerMovingPlatform::HandleActivationPlateChanged(
	AmultiplayerPressurePlate* Plate,
	bool bIsActive)
{
	RefreshActivation();
}

void AmultiplayerMovingPlatform::RefreshActivation()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bShouldActivate = ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate
		? ActivationPlate != nullptr && ActivationPlate->IsPlateActive()
		: ReplicatedPlayerCount >= RequiredPlayers;

	Transporter->SetTransportActive(bShouldActivate);
}

void AmultiplayerMovingPlatform::RemoveInvalidOccupants()
{
	for (auto It = OccupantOverlapCounts.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid() || It.Value() <= 0)
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
