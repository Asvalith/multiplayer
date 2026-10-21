// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Mechanisms/multiplayerMeshMovement.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

// 压力板规则层：服务器按当前人数、目标和锁存配置决定开关，门/平台只订阅结果。
// 客户端仅按复制开关播放网格按压，不维护第二份占用或逐帧向服务器上报。

// 创建压力板网格、占用触发器与统计组件，静止时关闭 Tick 并保持网络休眠。
AmultiplayerPressurePlate::AmultiplayerPressurePlate()
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

	PlayerOccupancy = CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(TEXT("PlayerOccupancy"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlateMesh->SetStaticMesh(CubeMesh.Object);
	}
}

// 各端保存编辑器初始位置；仅服务器读取规则并绑定人数/目标，客户端关闭触发器。
void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

	ReleasedRelativeLocation = PlateMesh->GetRelativeLocation();
	PlateMesh->SetRelativeLocation(GetMeshTargetLocation());
	SetActorTickEnabled(false);

	if (!HasAuthority())
	{
		ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// 初始速度也是复制数据，不能在休眠期间静默改写。
	FlushNetDormancy();
	RuntimePressMoveSpeed = FmultiplayerGameplayConfig::Get(this).PlateMoveSpeed;

	if (bRequireObjectiveComplete)
	{
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this, &AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
		}
	}

	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this, &AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(ActivationTrigger, bRequirePlayerControlledCharacter);
	EvaluatePlateState();
}

// 先解绑自身监听再清占用，避免卸载时的清零通知重新执行本板逻辑。
void AmultiplayerPressurePlate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先取消本 Actor 的监听，再清空人数，避免清理时触发自身规则。
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this, &AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this, &AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

// 各端仅在过渡中更新相对网格位置，到位即停，不逐帧轮询激活规则。
void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (MultiplayerMeshMovement::MoveTo(*PlateMesh, GetMeshTargetLocation(), DeltaSeconds,
		RuntimePressMoveSpeed, 0.25f, MultiplayerMeshMovement::ESpace::Relative))
	{
		SetActorTickEnabled(false);
	}
}

// 同步开关与初始速度；当前人数由服务器本地事件消费，不额外复制。
void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
	DOREPLIFETIME_CONDITION(AmultiplayerPressurePlate, RuntimePressMoveSpeed, COND_InitialOnly);
}

// 追加当前有效不同角色供合作门跨板去重，不清空调用者已经收集的结果。
void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

/** 先更新本板开关，再通知上层占用改变，使监听者读取到相互一致的最新状态。 */
void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	EvaluatePlateState();
	// 激活布尔值不变时也必须通知门重新统计不同玩家。
	OnPlateOccupancyChanged.Broadcast(this, PlayerCount);
}

// 钥匙条件改变时重算当前板状态，不能只在进入/离开压力板时检查前置目标。
void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys, int32 RequiredKeys)
{
	EvaluatePlateState();
}

// 由服务器订阅入口调用：读取完整当前条件，只有真实状态变化才唤醒休眠并复制。
void AmultiplayerPressurePlate::EvaluatePlateState()
{
	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bNewPlateActive = (bLatchOnceActivated && bPlateActive)
		|| (bObjectiveReady && PlayerOccupancy->GetPlayerCount() > 0);

	if (bPlateActive == bNewPlateActive)
	{
		return;
	}

	// 先刷新休眠再写属性，不能只依赖赋值后的 ForceNetUpdate。
	FlushNetDormancy();
	bPlateActive = bNewPlateActive;
	OnRep_PlateActive();
	ForceNetUpdate();
}

// 两端共用状态发布出口：启动本地过渡并通知依赖机关/蓝图表现，不在此反向改规则。
void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	SetActorTickEnabled(true);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

// 当前开关只决定是否叠加按压偏移，释放时准确回到关卡保存的初始相对位置。
FVector AmultiplayerPressurePlate::GetMeshTargetLocation() const
{
	return ReleasedRelativeLocation + (bPlateActive ? PressedOffset : FVector::ZeroVector);
}
