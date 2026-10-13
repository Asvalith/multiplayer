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

//构造函数：初始化组件与配置
AmultiplayerCoopGate::AmultiplayerCoopGate()
{

	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	//关闭移动复制
	SetReplicateMovement(false);
	//低频网络更新
	SetNetUpdateFrequency(5.0f);
	//默认网络休眠模式
	NetDormancy = DORM_DormantAll;

	//根组件可移动
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	SceneRoot->SetMobility(EComponentMobility::Movable);
	//门网格组件
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

	//从引擎自带资源里加载一个立方体，作为门的默认网格
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		DoorMesh->SetStaticMesh(CubeMesh.Object);
	}
}

//生命周期开始时的初始化
void AmultiplayerCoopGate::BeginPlay()
{
	Super::BeginPlay();
	//设置门的初始位置为目标位置，并禁用Actor的Tick
	DoorMesh->SetWorldLocation(GetMeshTargetLocation());
	SetActorTickEnabled(false);
	if (!HasAuthority())
	{
		return;
	}
	//客户端到这里就返回了，服务器才继续执行后续逻辑

	//刷新网络休眠状态，并设置门的移动速度。
	FlushNetDormancy();
	RuntimeDoorMoveSpeed = FmultiplayerGameplayConfig::Get(this).DoorMoveSpeed;
	//运行时依赖的压力板列表重置，并预留空间
	RuntimeRequiredPlates.Reset();
	RuntimeRequiredPlates.Reserve(RequiredPlates.Num());

	for (AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (!IsValid(Plate) || Plate->IsActorBeingDestroyed() || RuntimeRequiredPlates.Contains(Plate))
		{
			continue;
		}
		//将有效的压力板添加到运行时列表中，并绑定相关事件处理函数
		RuntimeRequiredPlates.Add(Plate);
		//订阅压力板的激活状态变化、占用人数变化和销毁事件，以便在这些事件发生时更新门的状态
		Plate->OnPlateActiveChanged.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
		Plate->OnPlateOccupancyChanged.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged);
		Plate->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateDestroyed);
	}
	//检查运行时压力板数量是否满足要求，如果不满足则记录错误日志
	if (RuntimeRequiredPlates.Num() < GetRequiredPlateCount())
	{
		UE_LOG(LogMultiplayer, Error,
			TEXT("CoopGate[%s] invalid setup: Required=%d UniqueValidPlates=%d."),
			*GetName(), GetRequiredPlateCount(), RuntimeRequiredPlates.Num());
	}
	//如果需要完成目标，则获取游戏状态并订阅目标进度变化事件
	if (bRequireObjectiveComplete)
	{
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this, &AmultiplayerCoopGate::HandleObjectiveProgressChanged);
		}
	}
	//评估门的初始状态，决定是否需要打开或关闭门
	EvaluateGateState();
}

//endplay生命周期结束时的清理
void AmultiplayerCoopGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (AmultiplayerPressurePlate* Plate : RuntimeRequiredPlates)
	{
		if (IsValid(Plate))
		{
			UnbindRequiredPlate(Plate);
		}
	}
	RuntimeRequiredPlates.Reset();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this, &AmultiplayerCoopGate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

//Tick函数：每帧更新门的位置，直到到达目标位置，关闭tick
void AmultiplayerCoopGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (MultiplayerMeshMovement::MoveTo(*DoorMesh, GetMeshTargetLocation(), DeltaSeconds,
		RuntimeDoorMoveSpeed, 0.5f, MultiplayerMeshMovement::ESpace::World))
	{
		SetActorTickEnabled(false);
	}
}

//网络复制相关函数：注册需要复制的属性
void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
	DOREPLIFETIME_CONDITION(AmultiplayerCoopGate, RuntimeDoorMoveSpeed, COND_InitialOnly);
}

//解绑压力板事件处理函数
void AmultiplayerCoopGate::UnbindRequiredPlate(AmultiplayerPressurePlate* Plate)
{
	Plate->OnPlateActiveChanged.RemoveDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateChanged);
	Plate->OnPlateOccupancyChanged.RemoveDynamic(this, &AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged);
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

//处理压力板被销毁的情况：解绑事件并从运行时列表中移除，然后重新评估门的状态
void AmultiplayerCoopGate::HandleRequiredPlateDestroyed(AActor* DestroyedActor)
{
	AmultiplayerPressurePlate* DestroyedPlate = Cast<AmultiplayerPressurePlate>(DestroyedActor);
	if (DestroyedPlate == nullptr)
	{
		return;
	}

	UnbindRequiredPlate(DestroyedPlate);
	RuntimeRequiredPlates.Remove(DestroyedPlate);

	EvaluateGateState();
}

//处理目标进度变化事件：重新评估门的状态
void AmultiplayerCoopGate::HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys)
{
	EvaluateGateState();
}

//评估门的状态：根据压力板的激活状态和占用人数，以及目标完成情况，决定是否打开或关闭门
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
	// GetOccupyingCharacters 只追加有效角色，再跨板去重。
	for (ACharacter* Occupant : PlateOccupants)
	{
		DistinctPlayers.Add(Occupant);
	}

	const int32 RequiredCount = GetRequiredPlateCount();
	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	//三个条件同时满足时，门才会打开：目标完成、激活的压力板数量达到要求、不同玩家数量达到要求
	const bool bShouldOpen = bObjectiveReady&& ActivePlateCount >= RequiredCount&& DistinctPlayers.Num() >= RequiredCount;
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;
	//状态变化时，刷新网络休眠状态，更新门的状态，并强制网络更新
	if (bGateOpen != bNewGateOpen)
	{
		// 先刷新休眠，再提交状态；晚加入通过初始复制获取当前值。
		FlushNetDormancy();
		bGateOpen = bNewGateOpen;
		OnRep_GateOpen();
		ForceNetUpdate();
	}
}

//Replicated函数：当门的状态发生变化时，启用Actor的Tick以更新门的位置
void AmultiplayerCoopGate::OnRep_GateOpen()
{
	SetActorTickEnabled(true);
}

//获取门的目标位置：根据门的状态返回开门点或关门点的位置
FVector AmultiplayerCoopGate::GetMeshTargetLocation() const
{
	return bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
}
