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
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

/*
 * 合作门分成两条路径：服务器由事件重算开关状态，各端由开关状态播放网格运动。
 * 所有规则来源最后汇入 EvaluateGateState，表现统一汇入 ApplyGateState，避免各写一套判定。
 */

/** 建立可在蓝图中调整的门网格和两个端点；默认只具备 Tick 能力，静止时不执行 Tick。 */
AmultiplayerCoopGate::AmultiplayerCoopGate()
{
	// Actor Tick 用于网格过渡而非常驻规则轮询；构造时关闭，状态改变后再按需开启。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// (*) 门只复制离散开关状态，网格移动由各端按相同端点插值完成。
	SetReplicateMovement(false);
	// 布尔状态偶发变化，常规网络更新频率设为 5Hz；真正改变时用 ForceNetUpdate 请求尽快发送。
	SetNetUpdateFrequency(5.0f);
	// 本地网格过渡 Tick 与网络休眠独立；开关不变时不持续比较复制属性。
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

	// 客户端和服务器都先按本机已有状态复原网格；规则依赖只由服务器维护。
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

/** 门退出运行时撤销外部事件绑定，并丢弃本次关卡使用的压力板集合。 */
void AmultiplayerCoopGate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 运行时压力板集合和 GameState 都是外部对象，销毁门之前必须逐一解除 Delegate。
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

/** 仅在开关过渡期间执行匀速移动；接近端点时精确对齐并停止后续 Tick。 */
void AmultiplayerCoopGate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// 配置尚未随初始复制到达时暂不运动，避免把 0 速度误当成直接到达目标。
	if (RuntimeDoorMoveSpeed <= 0.0f)
	{
		return;
	}

	const FVector TargetLocation = bGateOpen
		? OpenPoint->GetComponentLocation()
		: ClosedPoint->GetComponentLocation();
	// 门的运动是离散状态的本地表现，不依赖逐帧 Transform 复制；最终端点由 bGateOpen 决定。
	const FVector NewLocation = FMath::VInterpConstantTo(
		DoorMesh->GetComponentLocation(),
		TargetLocation,
		DeltaSeconds,
		RuntimeDoorMoveSpeed);

	DoorMesh->SetWorldLocation(NewLocation);

	if (NewLocation.Equals(TargetLocation, 0.5f))
	{
		DoorMesh->SetWorldLocation(TargetLocation);
		// 静止时关闭 Tick，只在开关状态变化后的过渡期间计算。
		SetActorTickEnabled(false);
	}
}

/** 注册运行期需要同步的开关属性；UPROPERTY 声明与这里的注册缺一不可。 */
void AmultiplayerCoopGate::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 不复制 DoorMesh 位置，只复制决定位置的最小逻辑状态。
	DOREPLIFETIME(AmultiplayerCoopGate, bGateOpen);
	DOREPLIFETIME_CONDITION(AmultiplayerCoopGate, RuntimeDoorMoveSpeed, COND_InitialOnly);
}

/** 返回至少为一的配置门槛；不根据当前有效板数降低要求，避免错误配置变成更容易开门。 */
int32 AmultiplayerCoopGate::GetRequiredPlateCount() const
{
	// 需求数量是玩法配置，不能因空引用或重复引用而静默降低，否则错误配置会提前开门。
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

/** 服务器同时订阅开关、占用和销毁事件，覆盖开门条件依赖的全部变化来源。 */
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

/** 每块板的三个订阅成组解除，避免销毁路径和普通清理路径维护不同的解绑列表。 */
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
	// 不把单块板的事件参数当成完整结论；重新遍历所有依赖才能正确处理多板组合。
	EvaluateGateState();
}

/** 板仍激活时玩家也可能进出；补充此入口，避免不同玩家总数变化后门保持旧状态。 */
void AmultiplayerCoopGate::HandleRequiredPlateOccupancyChanged(
	AmultiplayerPressurePlate* Plate,
	int32 PlayerCount)
{
	// 即使压力板仍保持激活，不同玩家集合也可能已经变化，门必须重新组合完整条件。
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
	// 钥匙目标可能是额外前置条件，收到进度变化后与当前压力板状态一起重算。
	EvaluateGateState();
}

/**
 * 服务器组合有效板数、激活板数、不同角色数和可选目标条件，决定门应当开还是关。
 * (*) TSet 按角色身份合并跨板占用，同一角色站在两块板上仍只算一人。
 * (**) 多种事件可能连续调用此函数；只有最终开关变化才启动表现和请求网络更新。
 */
void AmultiplayerCoopGate::EvaluateGateState()
{
	if (!HasAuthority())
	{
		return;
	}

	int32 ActivePlateCount = 0;
	// (**) 除了板数还要统计不同角色，防止同一角色同时覆盖两块板。
	// 常见双人/少量成员使用内联存储，超过 4 人仍可扩容；容量不是玩法人数限制。
	// 这里只减少事件求值时的小额临时分配，不宣称未经测量的帧率收益。
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
	// 常规模式实时跟随条件；保持开启模式只允许 false -> true，不再因玩家离板回退。
	const bool bNewGateOpen = bStayOpenOnceActivated ? (bGateOpen || bShouldOpen) : bShouldOpen;

	if (bGateOpen != bNewGateOpen)
	{
		// 相同状态不广播、不启 Tick、不强制网络更新，避免多个来源重复求值带来无效工作。
		// 先刷新休眠，再提交状态；晚加入通过初始复制获取当前值。
		FlushNetDormancy();
		bGateOpen = bNewGateOpen;
		HandleGateStateChanged();
		ForceNetUpdate();
	}
}

/** 客户端收到复制状态后的入口；这里只消费结果，不重新参与服务器规则计算。 */
void AmultiplayerCoopGate::OnRep_GateOpen()
{
	// RepNotify 只把复制结果交给表现层；客户端不会调用 EvaluateGateState 参与规则判定。
	HandleGateStateChanged();
}

/** 服务器主动写状态和客户端 RepNotify 共用表现入口，照顾监听服务器本机画面。 */
void AmultiplayerCoopGate::HandleGateStateChanged()
{
	ApplyGateState(false);
}

/** 初始化时直接定位；运行期变更只开启 Tick，移动过程仍由 Tick 统一处理。 */
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
