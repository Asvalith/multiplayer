// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Core/multiplayerLog.h"
#include "Mechanisms/multiplayerMeshMovement.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

/** 创建互相独立的触发区域和可移动网格；网格压下不会带动触发区改变人数检测范围。 */
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
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(
		ActivationTrigger,
		bRequirePlayerControlledCharacter);
	ApplyPlateState(true);

	if (!HasAuthority())
	{
		return;
	}

	if (bRequireObjectiveComplete)
	{
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
	if (MultiplayerMeshMovement::MoveTo(*PlateMesh, GetMeshTargetLocation(), DeltaSeconds,
		RuntimePressMoveSpeed, 0.25f, MultiplayerMeshMovement::ESpace::Relative))
	{
		SetActorTickEnabled(false);
	}
}

void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
	DOREPLIFETIME_CONDITION(AmultiplayerPressurePlate, RuntimePressMoveSpeed, COND_InitialOnly);
}

void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

/** 先更新本板开关，再通知上层占用改变，使监听者读取到相互一致的最新状态。 */
void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	EvaluatePlateState();
	if (HasAuthority())
	{
		// 激活布尔值不变时也必须通知门重新统计不同玩家。
		OnPlateOccupancyChanged.Broadcast(this, PlayerCount);
	}
}

/** 玩家已在板上等待时，目标完成也要触发判定；不要求玩家重新进入触发区。 */
void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
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
	const int32 PlayerCount = PlayerOccupancy->GetPlayerCount();
	const bool bNewPlateActive =
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
		return;
	}

	// 先刷新休眠再写属性，不能只依赖赋值后的 ForceNetUpdate。
	FlushNetDormancy();
	bPlateActive = bNewPlateActive;
	HandlePlateActiveChanged();
	ForceNetUpdate();
}

void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	HandlePlateActiveChanged();
}

void AmultiplayerPressurePlate::HandlePlateActiveChanged()
{
	ApplyPlateState(false);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

FVector AmultiplayerPressurePlate::GetMeshTargetLocation() const
{
	return ReleasedRelativeLocation + (bPlateActive ? PressedOffset : FVector::ZeroVector);
}

void AmultiplayerPressurePlate::ApplyPlateState(bool bSnapToTarget)
{
	if (bSnapToTarget)
	{
		PlateMesh->SetRelativeLocation(GetMeshTargetLocation());
	}
	SetActorTickEnabled(!bSnapToTarget);
}
