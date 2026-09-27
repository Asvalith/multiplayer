// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Core/multiplayerLog.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

/*
 * 压力板把碰撞统计委托给 Occupancy，自己只组合规则和播放状态表现。
 * 占用变化与开关变化是两类事件：玩家进出可能不改变开关，但仍会影响合作门的不同玩家计数。
 */

/** 创建互相独立的触发区域和可移动网格；网格压下不会带动触发区改变人数检测范围。 */
AmultiplayerPressurePlate::AmultiplayerPressurePlate()
{
	// 逻辑通过事件驱动，Tick 只服务于压下/弹起的短暂视觉过渡。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// 压力板不承载权威位移，只复制激活状态并在各端播放相同的按压表现。
	SetReplicateMovement(false);
	// bPlateActive 低频变化，限制网络更新频率；状态改变时 ForceNetUpdate 提升发送时效。
	SetNetUpdateFrequency(5.0f);
	// 停止 Tick 不等于停止网络检查。离散机关平时休眠，修改复制状态前刷新休眠。
	// 使用 DormantAll 而非 Initial，也支持动态生成的机关。
	NetDormancy = DORM_DormantAll;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	SceneRoot->SetMobility(EComponentMobility::Movable);

	PlateMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateMesh"));
	PlateMesh->SetupAttachment(SceneRoot);
	PlateMesh->SetMobility(EComponentMobility::Movable);
	PlateMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlateMesh->SetGenerateOverlapEvents(false);
	PlateMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->bEditableWhenInherited = true;
	ActivationTrigger->SetRelativeLocation(FVector(0.0f, 0.0f, 60.0f));
	ActivationTrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlateMesh->SetStaticMesh(CubeMesh.Object);
	}
}

/** 缓存关卡摆放位置、绑定人数变化；服务器按配置追加目标监听并补算初始激活状态。 */
void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// 初始速度也是复制数据，不能在休眠期间静默改写。
		FlushNetDormancy();
		RuntimePressMoveSpeed = FmultiplayerGameplayConfig::Get(this).PlateMoveSpeed;
	}
	UE_LOG(LogMultiplayer, Verbose, TEXT("PressurePlate %s: Authority=%d PressMoveSpeed=%.1f"),
		*GetName(), HasAuthority(), RuntimePressMoveSpeed);

	// 用关卡实例中的实际摆放位置作为弹起基准，蓝图调整网格后不需要同步修改 C++ 常量。
	ReleasedRelativeLocation = PlateMesh->GetRelativeLocation();
	// 人数统计组件统一处理角色过滤、多碰撞体和销毁清理，压力板只监听最终不同玩家数变化。
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(
		ActivationTrigger,
		bRequirePlayerControlledCharacter);
	// 初次加载直接贴合当前复制状态，不播放从默认原点到目标点的错误过渡。
	ApplyPlateState(true);

	if (!HasAuthority())
	{
		return;
	}

	if (bRequireObjectiveComplete)
	{
		// 只有配置了目标前置条件才绑定 GameState，普通压力板不承担无用的全局进度监听。
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this,
				&AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
		}
	}
	EvaluatePlateState();
}

/** 撤销人数和目标监听；先取消本 Actor 的监听，再清空人数，避免清理时触发自身规则。 */
void AmultiplayerPressurePlate::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 对称解绑组件和外部 GameState，明确终止事件关系，不把弱委托的失效检查当成日常清理。
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

/** 按当前开关匀速压下或弹起；到达后停 Tick，静止状态无需逐帧重算。 */
void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (RuntimePressMoveSpeed <= 0.0f)
	{
		return;
	}

	const FVector TargetLocation =
		ReleasedRelativeLocation
		+ (bPlateActive ? PressedOffset : FVector::ZeroVector);
	// 各端仅根据复制的离散状态播放视觉插值；这里不产生服务器规则，也不反向写 bPlateActive。
	const FVector NewLocation = FMath::VInterpConstantTo(
		PlateMesh->GetRelativeLocation(),
		TargetLocation,
		DeltaSeconds,
		RuntimePressMoveSpeed);

	PlateMesh->SetRelativeLocation(NewLocation);
	if (NewLocation.Equals(TargetLocation, 0.25f))
	{
		// 容差内精确贴合终点，避免浮点尾差让 Tick 永久保持开启。
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
	}
}

/** 运行中只同步激活状态，速度仅初始同步；人数表和压下位置不持续发送。 */
void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// 压力板只发送离散逻辑状态，客户端网格位置由 OnRep 后的本地插值恢复。
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
	DOREPLIFETIME_CONDITION(AmultiplayerPressurePlate, RuntimePressMoveSpeed, COND_InitialOnly);
}

/** 向输出数组追加有效不同角色；服务器合作门用它合并多个压力板的玩家集合。 */
void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

/** 先更新本板开关，再通知上层占用改变，使监听者读取到相互一致的最新状态。 */
void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	// 先更新压力板状态，再发布完整的占用变化。这样门收到事件时能同时读取最新激活状态和玩家集合。
	EvaluatePlateState();
	if (HasAuthority())
	{
		// (**) 激活状态可能仍为 true，但不同玩家集合已经改变；不能用 ActiveChanged 代替该事件。
		OnPlateOccupancyChanged.Broadcast(this, PlayerCount);
	}
}

/** 玩家已在板上等待时，目标完成也要触发判定；不要求玩家重新进入触发区。 */
void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	// 目标可能在玩家已经站在板上时完成，必须重新求值才能立即激活。
	EvaluatePlateState();
}

/**
 * 服务器根据区域是否有人和可选目标条件判定激活；锁存模式在首次激活后保持开启。
 * (**) 这里仅跳过相同开关的通知，占用事件仍由调用方发送，不能一并丢弃。
 */
void AmultiplayerPressurePlate::EvaluatePlateState()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bObjectiveReady =
		!bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const int32 PlayerCount = PlayerOccupancy->GetPlayerCount();
	const bool bNewPlateActive =
		// 锁存模式一旦激活就保持为真；普通模式则随目标和区域人数实时变化。
		(bLatchOnceActivated && bPlateActive)
		|| (bObjectiveReady && PlayerCount > 0);

	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("PressurePlate[%s] Players=%d ObjectiveReady=%s Active=%s"),
		*GetName(),
		PlayerCount,
		bObjectiveReady ? TEXT("true") : TEXT("false"),
		bNewPlateActive ? TEXT("true") : TEXT("false"));

	if (bPlateActive == bNewPlateActive)
	{
		// 人数或目标事件可能重复到达；状态未变化时不产生网络、蓝图和机关级联通知。
		return;
	}

	// (**) 先刷新休眠再写属性，不能只依赖赋值后的 ForceNetUpdate。
	FlushNetDormancy();
	bPlateActive = bNewPlateActive;
	HandlePlateActiveChanged();
	ForceNetUpdate();
}

/** 消费服务器复制结果；远端不使用本地碰撞重算开关。 */
void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	// 客户端只消费服务器结果并更新表现，不在这里重新读取本地碰撞人数。
	HandlePlateActiveChanged();
}

/** 开启网格过渡并广播本机事件；蓝图扩展入口适合音效、材质等表现。 */
void AmultiplayerPressurePlate::HandlePlateActiveChanged()
{
	// 服务器直接写入和客户端 RepNotify 都经过同一出口，Listen Server 与远端客户端表现一致。
	ApplyPlateState(false);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

/** 初次加载直接贴合当前状态；后续变更启用 Tick，从当前网格位置继续过渡。 */
void AmultiplayerPressurePlate::ApplyPlateState(bool bSnapToTarget)
{
	const FVector TargetLocation =
		ReleasedRelativeLocation
		+ (bPlateActive ? PressedOffset : FVector::ZeroVector);
	if (bSnapToTarget)
	{
		// 初始化/恢复时直接放到正确位置；运行期变化则在下面开启 Tick 平滑移动。
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
		return;
	}

	SetActorTickEnabled(true);
}
