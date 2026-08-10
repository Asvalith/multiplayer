// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGate.h"

#include "Components/SceneComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerPressurePlate.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

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
	SceneRoot->SetMobility(EComponentMobility::Movable);

	DoorMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DoorMesh"));
	DoorMesh->SetupAttachment(SceneRoot);
	DoorMesh->SetMobility(EComponentMobility::Movable);
	DoorMesh->SetCollisionProfileName(TEXT("BlockAll"));
	DoorMesh->SetRelativeLocation(FVector(300.0f, 0.0f, 200.0f));
	DoorMesh->SetRelativeScale3D(FVector(0.3f, 2.0f, 2.0f));

	ClosedPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("ClosedPoint"));
	ClosedPoint->SetupAttachment(SceneRoot);
	ClosedPoint->SetMobility(EComponentMobility::Movable);
	ClosedPoint->bEditableWhenInherited = true;
	ClosedPoint->SetRelativeLocation(FVector(300.0f, 0.0f, 200.0f));
	CastChecked<UArrowComponent>(ClosedPoint)->ArrowColor = FColor::Red;

	OpenPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("OpenPoint"));
	OpenPoint->SetupAttachment(SceneRoot);
	OpenPoint->SetMobility(EComponentMobility::Movable);
	OpenPoint->bEditableWhenInherited = true;
	OpenPoint->SetRelativeLocation(FVector(300.0f, 0.0f, 600.0f));
	CastChecked<UArrowComponent>(OpenPoint)->ArrowColor = FColor::Green;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		DoorMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerCoopGate::BeginPlay()
{
	Super::BeginPlay();

	BindRequiredPlates();
	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
			this,
			&AmultiplayerCoopGate::HandleObjectiveProgressChanged);
	}
	EvaluateGateState();
	ApplyGateState(true);
}

void AmultiplayerCoopGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindRequiredPlates();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerCoopGate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector TargetLocation = bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
	const FVector NewLocation = FMath::VInterpConstantTo(
		DoorMesh->GetComponentLocation(),
		TargetLocation,
		DeltaSeconds,
		DoorMoveSpeed);

	DoorMesh->SetWorldLocation(NewLocation);

	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		DoorMesh->SetWorldLocation(TargetLocation);
		SetActorTickEnabled(false);
	}
}

void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
	DOREPLIFETIME(AmultiplayerCoopGate, ActivePlateCount);
}

int32 AmultiplayerCoopGate::GetRequiredPlateCount() const
{
	if (RequiredPlates.Num() == 0)
	{
		return RequiredActivePlateCount;
	}

	return FMath::Clamp(RequiredActivePlateCount, 1, RequiredPlates.Num());
}

void AmultiplayerCoopGate::BindRequiredPlates()
{
	for (AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (Plate != nullptr)
		{
			Plate->OnPlateActiveChanged.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
		}
	}
}

void AmultiplayerCoopGate::UnbindRequiredPlates()
{
	for (AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (Plate != nullptr)
		{
			Plate->OnPlateActiveChanged.RemoveDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
		}
	}
}

void AmultiplayerCoopGate::HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive)
{
	EvaluateGateState();
}

void AmultiplayerCoopGate::HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys)
{
	EvaluateGateState();
}

void AmultiplayerCoopGate::EvaluateGateState()
{
	if (!HasAuthority())
	{
		return;
	}

	int32 NewActivePlateCount = 0;
	TSet<ACharacter*> DistinctPlayers;
	for (const AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (Plate == nullptr || !Plate->IsPlateActive())
		{
			continue;
		}

		++NewActivePlateCount;

		TArray<ACharacter*> PlateOccupants;
		Plate->GetOccupyingCharacters(PlateOccupants);
		for (ACharacter* Occupant : PlateOccupants)
		{
			if (Occupant != nullptr)
			{
				DistinctPlayers.Add(Occupant);
			}
		}
	}

	if (ActivePlateCount != NewActivePlateCount)
	{
		ActivePlateCount = NewActivePlateCount;
		OnRep_ActivePlateCount();
	}

	const int32 RequiredCount = GetRequiredPlateCount();
	const bool bHasValidPlateSetup = RequiredPlates.Num() > 0 && RequiredCount > 0;
	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bShouldOpen = bHasValidPlateSetup
		&& bObjectiveReady
		&& ActivePlateCount >= RequiredCount
		&& DistinctPlayers.Num() >= RequiredCount;
	UE_LOG(
		LogTemp,
		Log,
		TEXT("CoopGate[%s] Evaluate: Plates=%d Active=%d Required=%d Players=%d ObjectiveRequired=%s ObjectiveReady=%s ShouldOpen=%s CurrentOpen=%s"),
		*GetName(),
		RequiredPlates.Num(),
		ActivePlateCount,
		RequiredCount,
		DistinctPlayers.Num(),
		bRequireObjectiveComplete ? TEXT("true") : TEXT("false"),
		bObjectiveReady ? TEXT("true") : TEXT("false"),
		bShouldOpen ? TEXT("true") : TEXT("false"),
		bGateOpen ? TEXT("true") : TEXT("false"));
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;

	if (bGateOpen != bNewGateOpen)
	{
		bGateOpen = bNewGateOpen;
		OnRep_GateOpen();
	}

	ForceNetUpdate();
}

void AmultiplayerCoopGate::OnRep_GateOpen()
{
	ApplyGateState(false);
	OnGateStateChanged.Broadcast(bGateOpen);
	ReceiveGateVisualStateChanged(bGateOpen);
}

void AmultiplayerCoopGate::OnRep_ActivePlateCount()
{
	OnPlateProgressChanged.Broadcast(ActivePlateCount, GetRequiredPlateCount());
	ReceivePlateProgressChanged(ActivePlateCount, GetRequiredPlateCount());
}

void AmultiplayerCoopGate::ApplyGateState(bool bSnapToTarget)
{
	const FVector TargetLocation = bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
	if (bSnapToTarget)
	{
		DoorMesh->SetWorldLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	SetActorTickEnabled(true);
}
