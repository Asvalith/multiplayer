// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGate.h"

#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Character.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerLog.h"
#include "multiplayerPressurePlate.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerCoopGate::AmultiplayerCoopGate()
{
	// Actor Tick 用于网格过渡而非常驻规则轮询；构造时关闭，状态改变后再按需开启。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// (*) 门只复制离散开关状态，网格移动由各端按相同端点插值完成。
	SetReplicateMovement(false);
	// 只复制偶发变化的布尔状态，5Hz 上限已足够；ForceNetUpdate 会在真正改变时推动尽快发送。
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

	// 客户端和服务器都先按本机已有状态复原网格；只有服务器继续绑定规则事件。
	ApplyGateState(true);
	if (!HasAuthority())
	{
		return;
	}

	// 外部压力板和目标进度都可能改变开门条件，因此统一绑定后立即做一次当前状态求值。
	BindRequiredPlates();
	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
			this,
			&AmultiplayerCoopGate::HandleObjectiveProgressChanged);
	}
	EvaluateGateState();
}

void AmultiplayerCoopGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// RequiredPlates 和 GameState 都是外部对象，销毁门之前必须逐一解除 Delegate。
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
	// 门的运动是离散状态的本地表现，不依赖逐帧 Transform 复制；最终端点由 bGateOpen 决定。
	const FVector NewLocation = FMath::VInterpConstantTo(
		DoorMesh->GetComponentLocation(),
		TargetLocation,
		DeltaSeconds,
		DoorMoveSpeed);

	DoorMesh->SetWorldLocation(NewLocation);

	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		DoorMesh->SetWorldLocation(TargetLocation);
		// 静止时关闭 Tick，只在开关状态变化后的过渡期间计算。
		SetActorTickEnabled(false);
	}
}

void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 不复制 DoorMesh 位置，只复制决定位置的最小逻辑状态。
	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
}

int32 AmultiplayerCoopGate::GetRequiredPlateCount() const
{
	if (RequiredPlates.Num() == 0)
	{
		// 保留原始配置值用于诊断；EvaluateGateState 还会用 bHasValidPlateSetup 阻止空配置开门。
		return RequiredActivePlateCount;
	}

	// 关卡误填超过数组长度时收紧到可达范围，避免配置出一个永远无法满足的数量。
	return FMath::Clamp(RequiredActivePlateCount, 1, RequiredPlates.Num());
}

void AmultiplayerCoopGate::BindRequiredPlates()
{
	for (AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (Plate != nullptr)
		{
			// AddUniqueDynamic 让重复初始化或数组重复引用不会为同一对象/函数增加完全相同的绑定。
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
	// 不把单块板的事件参数当成完整结论；重新遍历所有依赖才能正确处理多板组合。
	EvaluateGateState();
}

void AmultiplayerCoopGate::HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys)
{
	// 钥匙目标可能是额外前置条件，收到进度变化后与当前压力板状态一起重算。
	EvaluateGateState();
}

void AmultiplayerCoopGate::EvaluateGateState()
{
	if (!HasAuthority())
	{
		return;
	}

	int32 ActivePlateCount = 0;
	// (**) 除了板数还要统计不同角色，防止同一角色同时覆盖两块板。
	TSet<ACharacter*> DistinctPlayers;
	for (const AmultiplayerPressurePlate* Plate : RequiredPlates)
	{
		if (Plate == nullptr || !Plate->IsPlateActive())
		{
			continue;
		}

		++ActivePlateCount;

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

	const int32 RequiredCount = GetRequiredPlateCount();
	const bool bHasValidPlateSetup = RequiredPlates.Num() > 0 && RequiredCount > 0;
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
		RequiredPlates.Num(),
		ActivePlateCount,
		RequiredCount,
		DistinctPlayers.Num(),
		bRequireObjectiveComplete ? TEXT("true") : TEXT("false"),
		bObjectiveReady ? TEXT("true") : TEXT("false"),
		bShouldOpen ? TEXT("true") : TEXT("false"),
		bGateOpen ? TEXT("true") : TEXT("false"));
	// 常规模式实时跟随条件；保持开启模式只允许 false -> true，不再因玩家离板回退。
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;

	if (bGateOpen != bNewGateOpen)
	{
		// 相同状态不广播、不启 Tick、不强制网络更新，避免多个来源重复求值带来无效工作。
		bGateOpen = bNewGateOpen;
		HandleGateStateChanged();
		ForceNetUpdate();
	}
}

void AmultiplayerCoopGate::OnRep_GateOpen()
{
	// RepNotify 只把复制结果交给表现层；客户端不会调用 EvaluateGateState 参与规则判定。
	HandleGateStateChanged();
}

void AmultiplayerCoopGate::HandleGateStateChanged()
{
	ApplyGateState(false);
}

void AmultiplayerCoopGate::ApplyGateState(bool bSnapToTarget)
{
	const FVector TargetLocation = bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
	if (bSnapToTarget)
	{
		// BeginPlay/状态恢复直接对齐，避免客户端加载时先看到默认关闭位置再播放一次伪动画。
		DoorMesh->SetWorldLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	// 运行期状态改变才启用过渡 Tick，到达端点后由 Tick 自行关闭。
	SetActorTickEnabled(true);
}
