// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerCoopGate.h"

#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Core/multiplayerLog.h"
#include "Mechanisms/multiplayerMeshMovement.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerCoopGate::AmultiplayerCoopGate()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(5.0f);
	NetDormancy = DORM_DormantAll;

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
	ClosedPoint->ArrowColor = FColor::Red;

	OpenPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("OpenPoint"));
	OpenPoint->SetupAttachment(SceneRoot);
	OpenPoint->SetMobility(EComponentMobility::Movable);
	OpenPoint->bEditableWhenInherited = true;
	OpenPoint->SetRelativeLocation(FVector(300.0f, 0.0f, 600.0f));
	OpenPoint->ArrowColor = FColor::Green;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		DoorMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerCoopGate::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		FlushNetDormancy();
		RuntimeDoorMoveSpeed = FmultiplayerGameplayConfig::Get(this).DoorMoveSpeed;
	}

	DoorMesh->SetWorldLocation(GetMeshTargetLocation());
	SetActorTickEnabled(false);
	if (!HasAuthority())
	{
		return;
	}

	RebuildRuntimeRequiredPlates();
	BindRequiredPlates();
	if (bRequireObjectiveComplete)
	{
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this, &AmultiplayerCoopGate::HandleObjectiveProgressChanged);
		}
	}
	EvaluateGateState();
}

void AmultiplayerCoopGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindRequiredPlates();
	RuntimeRequiredPlates.Reset();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this, &AmultiplayerCoopGate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (MultiplayerMeshMovement::MoveTo(*DoorMesh, GetMeshTargetLocation(), DeltaSeconds,
		RuntimeDoorMoveSpeed, 0.5f, MultiplayerMeshMovement::ESpace::World))
	{
		SetActorTickEnabled(false);
	}
}

void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
	DOREPLIFETIME_CONDITION(AmultiplayerCoopGate, RuntimeDoorMoveSpeed, COND_InitialOnly);
}

int32 AmultiplayerCoopGate::GetRequiredPlateCount() const
{
	return FMath::Max(1, RequiredActivePlateCount);
}

void AmultiplayerCoopGate::RebuildRuntimeRequiredPlates()
{
	RuntimeRequiredPlates.Reset();
	RuntimeRequiredPlates.Reserve(RequiredPlates.Num());
	for (AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (IsValid(Plate)
			&& !Plate->IsActorBeingDestroyed()
			&& !RuntimeRequiredPlates.Contains(Plate))
		{
			RuntimeRequiredPlates.Add(Plate);
		}
	}

	if (RuntimeRequiredPlates.Num() < GetRequiredPlateCount())
	{
		UE_LOG(LogMultiplayer, Error,
			TEXT("CoopGate[%s] invalid setup: Required=%d UniqueValidPlates=%d."),
			*GetName(), GetRequiredPlateCount(), RuntimeRequiredPlates.Num());
	}
}

void AmultiplayerCoopGate::BindRequiredPlates()
{
	for (AmultiplayerPressurePlate* Plate : RuntimeRequiredPlates)
	{
		if (IsValid(Plate))
		{
			// 激活状态变化覆盖普通开关条件；占用变化覆盖“仍激活但不同玩家集合已改变”。
			Plate->OnPlateActiveChanged.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
			Plate->OnPlateOccupancyChanged.AddUniqueDynamic(
				this, &AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged);
			Plate->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateDestroyed);
		}
	}
}

void AmultiplayerCoopGate::UnbindRequiredPlates()
{
	for (AmultiplayerPressurePlate* Plate : RuntimeRequiredPlates)
	{
		if (IsValid(Plate))
		{
			UnbindRequiredPlate(Plate);
		}
	}
}

void AmultiplayerCoopGate::UnbindRequiredPlate(AmultiplayerPressurePlate* Plate)
{
	Plate->OnPlateActiveChanged.RemoveDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
	Plate->OnPlateOccupancyChanged.RemoveDynamic(
		this, &AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged);
	Plate->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateDestroyed);
}

void AmultiplayerCoopGate::HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive)
{
	EvaluateGateState();
}

void AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged(
	AmultiplayerPressurePlate* Plate, int32 PlayerCount)
{
	EvaluateGateState();
}

void AmultiplayerCoopGate::HandleRequiredPlateDestroyed(AActor* DestroyedActor)
{
	AmultiplayerPressurePlate* DestroyedPlate = Cast<AmultiplayerPressurePlate>(DestroyedActor);
	if (DestroyedPlate == nullptr)
	{
		return;
	}

	UnbindRequiredPlate(DestroyedPlate);
	RuntimeRequiredPlates.Remove(DestroyedPlate);

	UE_LOG(LogMultiplayer, Warning,
		TEXT("CoopGate[%s] required plate was destroyed; gate rules are now unsatisfied."),
		*GetName());
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

	int32 ActivePlateCount = 0;
	// 跨板按角色去重；内联容量 4 只减少小额分配，不限制人数。
	TSet<ACharacter*, DefaultKeyFuncs<ACharacter*>, TInlineSetAllocator<4>> DistinctPlayers;
	TArray<ACharacter*> PlateOccupants;
	for (const AmultiplayerPressurePlate* Plate : RuntimeRequiredPlates)
	{
		if (!IsValid(Plate) || !Plate->IsPlateActive())
		{
			continue;
		}

		++ActivePlateCount;

		Plate->GetOccupyingCharacters(PlateOccupants);
	}
	// GetOccupyingCharacters 追加到同一数组，再跨板去重。
	for (ACharacter* Occupant : PlateOccupants)
	{
		if (Occupant != nullptr)
		{
			DistinctPlayers.Add(Occupant);
		}
	}

	const int32 RequiredCount = GetRequiredPlateCount();
	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bShouldOpen = bObjectiveReady
		&& ActivePlateCount >= RequiredCount
		&& DistinctPlayers.Num() >= RequiredCount;
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;

	if (bGateOpen != bNewGateOpen)
	{
		// 先刷新休眠，再提交状态；晚加入通过初始复制获取当前值。
		FlushNetDormancy();
		bGateOpen = bNewGateOpen;
		OnRep_GateOpen();
		ForceNetUpdate();
	}
}

void AmultiplayerCoopGate::OnRep_GateOpen()
{
	SetActorTickEnabled(true);
}

FVector AmultiplayerCoopGate::GetMeshTargetLocation() const
{
	return bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
}
