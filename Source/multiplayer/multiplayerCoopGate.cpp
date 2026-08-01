// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr uint8 PlateABit = 1 << 0;
	constexpr uint8 PlateBBit = 1 << 1;
	constexpr float PressedPlateOffset = -8.0f;
}

AmultiplayerCoopGate::AmultiplayerCoopGate()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(5.0f);
	SetMinNetUpdateFrequency(2.0f);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	DoorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DoorMesh"));
	DoorMesh->SetupAttachment(SceneRoot);
	DoorMesh->SetMobility(EComponentMobility::Movable);
	DoorMesh->SetCollisionProfileName(TEXT("BlockAll"));
	DoorMesh->SetRelativeLocation(FVector(300.0f, 0.0f, 200.0f));
	DoorMesh->SetRelativeScale3D(FVector(0.3f, 2.0f, 2.0f));

	PlateAMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateAMesh"));
	PlateAMesh->SetupAttachment(SceneRoot);
	PlateAMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlateAMesh->SetRelativeLocation(FVector(0.0f, -180.0f, 10.0f));
	PlateAMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	PlateBMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateBMesh"));
	PlateBMesh->SetupAttachment(SceneRoot);
	PlateBMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlateBMesh->SetRelativeLocation(FVector(0.0f, 180.0f, 10.0f));
	PlateBMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	PlateATrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("PlateATrigger"));
	PlateATrigger->SetupAttachment(SceneRoot);
	PlateATrigger->SetRelativeLocation(FVector(0.0f, -180.0f, 60.0f));
	PlateATrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	PlateATrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PlateATrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlateATrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlateBTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("PlateBTrigger"));
	PlateBTrigger->SetupAttachment(SceneRoot);
	PlateBTrigger->SetRelativeLocation(FVector(0.0f, 180.0f, 60.0f));
	PlateBTrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	PlateBTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PlateBTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlateBTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		DoorMesh->SetStaticMesh(CubeMesh.Object);
		PlateAMesh->SetStaticMesh(CubeMesh.Object);
		PlateBMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerCoopGate::BeginPlay()
{
	Super::BeginPlay();

	DoorClosedRelativeLocation = DoorMesh->GetRelativeLocation();
	PlateAReleasedRelativeLocation = PlateAMesh->GetRelativeLocation();
	PlateBReleasedRelativeLocation = PlateBMesh->GetRelativeLocation();

	PlateATrigger->OnComponentBeginOverlap.AddDynamic(this, &AmultiplayerCoopGate::HandlePlateABeginOverlap);
	PlateATrigger->OnComponentEndOverlap.AddDynamic(this, &AmultiplayerCoopGate::HandlePlateAEndOverlap);
	PlateBTrigger->OnComponentBeginOverlap.AddDynamic(this, &AmultiplayerCoopGate::HandlePlateBBeginOverlap);
	PlateBTrigger->OnComponentEndOverlap.AddDynamic(this, &AmultiplayerCoopGate::HandlePlateBEndOverlap);

	ApplyPlateState();
	ApplyGateState(true);
}

void AmultiplayerCoopGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector TargetLocation = DoorClosedRelativeLocation + (bGateOpen ? DoorOpenOffset : FVector::ZeroVector);
	const FVector NewLocation = FMath::VInterpConstantTo(
		DoorMesh->GetRelativeLocation(),
		TargetLocation,
		DeltaSeconds,
		DoorMoveSpeed);

	DoorMesh->SetRelativeLocation(NewLocation);

	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		DoorMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
	}
}

void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
	DOREPLIFETIME(AmultiplayerCoopGate, ActivePlateMask);
}

void AmultiplayerCoopGate::HandlePlateABeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	AddPlateOccupant(PlateAOccupants, OtherActor);
}

void AmultiplayerCoopGate::HandlePlateAEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	RemovePlateOccupant(PlateAOccupants, OtherActor);
}

void AmultiplayerCoopGate::HandlePlateBBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	AddPlateOccupant(PlateBOccupants, OtherActor);
}

void AmultiplayerCoopGate::HandlePlateBEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	RemovePlateOccupant(PlateBOccupants, OtherActor);
}

void AmultiplayerCoopGate::AddPlateOccupant(
	TSet<TWeakObjectPtr<ACharacter>>& Occupants,
	AActor* OtherActor)
{
	if (!HasAuthority())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr || Character->GetController() == nullptr)
	{
		return;
	}

	Occupants.Add(Character);
	EvaluateGateState();
}

void AmultiplayerCoopGate::RemovePlateOccupant(
	TSet<TWeakObjectPtr<ACharacter>>& Occupants,
	AActor* OtherActor)
{
	if (!HasAuthority())
	{
		return;
	}

	if (ACharacter* Character = Cast<ACharacter>(OtherActor))
	{
		Occupants.Remove(Character);
		EvaluateGateState();
	}
}

void AmultiplayerCoopGate::RemoveInvalidOccupants(TSet<TWeakObjectPtr<ACharacter>>& Occupants)
{
	for (auto It = Occupants.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void AmultiplayerCoopGate::EvaluateGateState()
{
	if (!HasAuthority())
	{
		return;
	}

	RemoveInvalidOccupants(PlateAOccupants);
	RemoveInvalidOccupants(PlateBOccupants);

	uint8 NewPlateMask = 0;
	if (!PlateAOccupants.IsEmpty())
	{
		NewPlateMask |= PlateABit;
	}
	if (!PlateBOccupants.IsEmpty())
	{
		NewPlateMask |= PlateBBit;
	}

	if (ActivePlateMask != NewPlateMask)
	{
		ActivePlateMask = NewPlateMask;
		OnRep_ActivePlateMask();
	}

	const bool bShouldOpen = HasTwoDistinctPlayers();
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;
	if (bGateOpen != bNewGateOpen)
	{
		bGateOpen = bNewGateOpen;
		OnRep_GateOpen();
	}

	ForceNetUpdate();
}

bool AmultiplayerCoopGate::HasTwoDistinctPlayers() const
{
	for (const TWeakObjectPtr<ACharacter>& PlateAPlayer : PlateAOccupants)
	{
		if (!PlateAPlayer.IsValid())
		{
			continue;
		}

		for (const TWeakObjectPtr<ACharacter>& PlateBPlayer : PlateBOccupants)
		{
			if (PlateBPlayer.IsValid() && PlateAPlayer != PlateBPlayer)
			{
				return true;
			}
		}
	}

	return false;
}

void AmultiplayerCoopGate::OnRep_GateOpen()
{
	ApplyGateState(false);
	OnGateStateChanged.Broadcast(bGateOpen);
	ReceiveGateVisualStateChanged(bGateOpen);
}

void AmultiplayerCoopGate::OnRep_ActivePlateMask()
{
	ApplyPlateState();
	OnPlateStateChanged.Broadcast(ActivePlateMask);
	ReceivePlateVisualStateChanged(ActivePlateMask);
}

void AmultiplayerCoopGate::ApplyGateState(bool bSnapToTarget)
{
	const FVector TargetLocation = DoorClosedRelativeLocation + (bGateOpen ? DoorOpenOffset : FVector::ZeroVector);
	if (bSnapToTarget)
	{
		DoorMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	SetActorTickEnabled(true);
}

void AmultiplayerCoopGate::ApplyPlateState()
{
	const bool bPlateAActive = (ActivePlateMask & PlateABit) != 0;
	const bool bPlateBActive = (ActivePlateMask & PlateBBit) != 0;

	PlateAMesh->SetRelativeLocation(
		PlateAReleasedRelativeLocation + FVector(0.0f, 0.0f, bPlateAActive ? PressedPlateOffset : 0.0f));
	PlateBMesh->SetRelativeLocation(
		PlateBReleasedRelativeLocation + FVector(0.0f, 0.0f, bPlateBActive ? PressedPlateOffset : 0.0f));
}
