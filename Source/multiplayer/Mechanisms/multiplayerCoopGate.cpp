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

/** 各端恢复初始位置；服务器建立压力板依赖，并在绑定后补算一次，覆盖绑定前已经满足的条件。 */
void AmultiplayerCoopGate::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		FlushNetDormancy();
		RuntimeDoorMoveSpeed = FmultiplayerGameplayConfig::Get(this).DoorMoveSpeed;
	}
	UE_LOG(LogMultiplayer, Verbose, TEXT("Gate %s: Authority=%d DoorMoveSpeed=%.1f"),
		*GetName(), HasAuthority(), RuntimeDoorMoveSpeed);

	ApplyGateState(true);
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
				this,
				&AmultiplayerCoopGate::HandleObjectiveProgressChanged);
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
			this,
			&AmultiplayerCoopGate::HandleObjectiveProgressChanged);
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

/** 从编辑器配置筛选有效且不重复的板；配置不足时记录日志，后续判定保持关闭。 */
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

	if (HasAuthority()
		&& RuntimeRequiredPlates.Num() < GetRequiredPlateCount())
	{
		UE_LOG(
			LogMultiplayer,
			Error,
			TEXT("CoopGate[%s] invalid setup: Required=%d UniqueValidPlates=%d."),
			*GetName(),
			GetRequiredPlateCount(),
			RuntimeRequiredPlates.Num());
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
				this,
				&AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged);
			Plate->OnDestroyed.AddUniqueDynamic(
				this,
				&AmultiplayerCoopGate::HandleRequiredPlateDestroyed);
		}
	}
}

/** 对称解除仍有效压力板上的三个事件；销毁中的依赖由销毁回调单独移除。 */
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

/** 某块板开关改变后重算完整规则；单个事件的布尔值不能代表整组压力板。 */
void AmultiplayerCoopGate::HandleRequiredPlateChanged(AmultiplayerPressurePlate* Plate, bool bIsActive)
{
	EvaluateGateState();
}

/** 板仍激活时玩家也可能进出；补充此入口，避免不同玩家总数变化后门保持旧状态。 */
void AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged(
	AmultiplayerPressurePlate* Plate,
	int32 PlayerCount)
{
	EvaluateGateState();
}

/** 依赖被销毁时先解绑和移除引用，再按剩余依赖重算；保持开启模式仍遵从自身规则。 */
void AmultiplayerCoopGate::HandleRequiredPlateDestroyed(AActor* DestroyedActor)
{
	AmultiplayerPressurePlate* DestroyedPlate =
		Cast<AmultiplayerPressurePlate>(DestroyedActor);
	if (DestroyedPlate == nullptr)
	{
		return;
	}

	UnbindRequiredPlate(DestroyedPlate);
	RuntimeRequiredPlates.Remove(DestroyedPlate);

	UE_LOG(
		LogMultiplayer,
		Warning,
		TEXT("CoopGate[%s] required plate was destroyed; gate rules are now unsatisfied."),
		*GetName());
	EvaluateGateState();
}

/** 可选目标进度变化时补算门状态，支持玩家先踩板、目标随后完成的顺序。 */
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
	// 人数组件采用追加语义，共用一个临时数组收集后去重，不为每块板分别分配数组。
	for (ACharacter* Occupant : PlateOccupants)
	{
		if (Occupant != nullptr)
		{
			DistinctPlayers.Add(Occupant);
		}
	}

	const int32 RequiredCount = GetRequiredPlateCount();
	const bool bHasValidPlateSetup =
		RuntimeRequiredPlates.Num() >= RequiredCount;
	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bShouldOpen = bHasValidPlateSetup
		&& bObjectiveReady
		&& ActivePlateCount >= RequiredCount
		&& DistinctPlayers.Num() >= RequiredCount;
	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("CoopGate[%s] Evaluate: Plates=%d Active=%d Required=%d Players=%d ObjectiveRequired=%s ObjectiveReady=%s ShouldOpen=%s CurrentOpen=%s"),
		*GetName(),
		RuntimeRequiredPlates.Num(),
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
		// 先刷新休眠，再提交状态；晚加入通过初始复制获取当前值。
		FlushNetDormancy();
		bGateOpen = bNewGateOpen;
		ApplyGateState(false);
		ForceNetUpdate();
	}
}

void AmultiplayerCoopGate::OnRep_GateOpen()
{
	ApplyGateState(false);
}

FVector AmultiplayerCoopGate::GetMeshTargetLocation() const
{
	return bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
}

void AmultiplayerCoopGate::ApplyGateState(bool bSnapToTarget)
{
	if (bSnapToTarget)
	{
		DoorMesh->SetWorldLocation(GetMeshTargetLocation());
	}
	SetActorTickEnabled(!bSnapToTarget);
}
