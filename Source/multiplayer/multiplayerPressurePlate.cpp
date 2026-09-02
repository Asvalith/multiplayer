// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerLog.h"
#include "multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerPressurePlate::AmultiplayerPressurePlate()
{
	// 逻辑通过事件驱动，Tick 只服务于压下/弹起的短暂视觉过渡。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// 压力板不承载权威位移，只复制激活状态并在各端播放相同的按压表现。
	SetReplicateMovement(false);
	// bPlateActive 低频变化，保持较低网络频率；状态改变时 ForceNetUpdate 提升发送时效。
	SetNetUpdateFrequency(5.0f);
	SetMinNetUpdateFrequency(2.0f);

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

void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

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

void AmultiplayerPressurePlate::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 对称解绑组件和外部 GameState；仅依赖 UObject 自动销毁不能移除对方保存的动态 Delegate。
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

void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	const FVector TargetLocation =
		ReleasedRelativeLocation
		+ (bPlateActive ? PressedOffset : FVector::ZeroVector);
	// 各端仅根据复制的离散状态播放视觉插值；这里不产生服务器规则，也不反向写 bPlateActive。
	const FVector NewLocation = FMath::VInterpConstantTo(
		PlateMesh->GetRelativeLocation(),
		TargetLocation,
		DeltaSeconds,
		PressMoveSpeed);

	PlateMesh->SetRelativeLocation(NewLocation);
	if (NewLocation.Equals(TargetLocation, 0.25f))
	{
		// 容差内精确贴合终点，避免浮点尾差让 Tick 永久保持开启。
		PlateMesh->SetRelativeLocation(TargetLocation);
		SetActorTickEnabled(false);
	}
}

void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// 压力板只发送离散逻辑状态，客户端网格位置由 OnRep 后的本地插值恢复。
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
}

void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	// 事件参数只表示触发原因；统一从 PlayerOccupancy 和 GameState 读取同一时刻的完整条件。
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	// 目标可能在玩家已经站在板上时完成，必须重新求值才能立即激活。
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::EvaluatePlateState()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bObjectiveReady =
		!bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bHasOccupants = PlayerOccupancy->GetPlayerCount() > 0;
	const bool bNewPlateActive =
		// 锁存模式一旦激活就保持为真；普通模式则随目标和区域人数实时变化。
		(bLatchOnceActivated && bPlateActive)
		|| (bObjectiveReady && bHasOccupants);

	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("PressurePlate[%s] Players=%d ObjectiveReady=%s Active=%s"),
		*GetName(),
		PlayerOccupancy->GetPlayerCount(),
		bObjectiveReady ? TEXT("true") : TEXT("false"),
		bNewPlateActive ? TEXT("true") : TEXT("false"));

	if (bPlateActive == bNewPlateActive)
	{
		// 人数或目标事件可能重复到达；状态未变化时不产生网络、蓝图和机关级联通知。
		return;
	}

	bPlateActive = bNewPlateActive;
	HandlePlateActiveChanged();
	ForceNetUpdate();
}

void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	// 客户端只消费服务器结果并更新表现，不在这里重新读取本地碰撞人数。
	HandlePlateActiveChanged();
}

void AmultiplayerPressurePlate::HandlePlateActiveChanged()
{
	// 服务器直接写入和客户端 RepNotify 都经过同一出口，Listen Server 与远端客户端表现一致。
	ApplyPlateState(false);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

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
