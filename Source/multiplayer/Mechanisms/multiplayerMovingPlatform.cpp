// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Core/multiplayerGameplayConfig.h"

#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Mechanisms/multiplayerTransporterComponent.h"
#include "UObject/ConstructorHelpers.h"

/*
 * 平台 Actor 负责组件装配与激活来源选择，Transporter 负责移动，Occupancy 负责人数。
 * 这三部分各有一份职责，触发来源变化最终都转换成 SetTransportActive 的布尔目标。
 */
// 创建平台、触发区和可视化端点，开启服务器位置复制；Actor 本身不运行 Tick。
AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);

	// 根与承载网格都必须可移动；服务器移动 Actor，客户端消费其复制位置。
	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);
	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetGenerateOverlapEvents(false);
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));
	// 仅查询 Pawn 重叠，是否满足人数条件交给 PlayerOccupancy。
	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->bEditableWhenInherited = true;
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	// 人数统计与轨道移动各自复用独立组件，本 Actor 只选择激活来源。
	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
	Transporter =
		CreateDefaultSubobject<UmultiplayerTransporterComponent>(
			TEXT("Transporter"));
	// 起止箭头供编辑器调整，BeginPlay 必须在平台移动前缓存其世界坐标。
	StartPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("StartPoint"));
	StartPoint->SetupAttachment(PlatformRoot);
	StartPoint->SetMobility(EComponentMobility::Movable);
	StartPoint->bEditableWhenInherited = true;
	StartPoint->ArrowColor = FColor::Red;

	TargetPoint = CreateDefaultSubobject<UArrowComponent>(TEXT("TargetPoint"));
	TargetPoint->SetupAttachment(PlatformRoot);
	TargetPoint->SetMobility(EComponentMobility::Movable);
	TargetPoint->bEditableWhenInherited = true;
	TargetPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 500.0f));
	TargetPoint->ArrowColor = FColor::Green;
	// 引擎立方体只作默认外观，关卡蓝图可替换网格。
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlatformMesh->SetStaticMesh(CubeMesh.Object);
	}
}

// 客户端接收服务器速度后回填非物理根组件，供 CMC 离开基座时继承。
void AmultiplayerMovingPlatform::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	Super::PostNetReceiveVelocity(NewVelocity);
	// 运动复制已带有 LinearVelocity，不新增逐帧 RPC。网格速度为零时 CMC 会回退读取 Actor 根速度。
	if (PlatformRoot) PlatformRoot->ComponentVelocity = NewVelocity;
}

// 服务器缓存固定轨道、读取人数并绑定唯一激活来源；客户端只关闭无用触发器。
void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// 端点是平台子组件，移动后继续读取会使目标跟着平台走。
	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	RequiredPlayers = FmultiplayerGameplayConfig::Get(this).PlatformRequiredPlayers;
	if (ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy)
	{
		// 自身占用模式由统计组件处理不同玩家去重。
		PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupancyChanged);
		PlayerOccupancy->BindTrigger(ActivationVolume);
	}
	else
	{
		// 外部压力板模式不需要平台自己的检测区域，两套激活来源保持互斥。
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		if (ActivationPlate != nullptr)
		{
			ActivationPlate->OnPlateActiveChanged.AddUniqueDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
			ActivationPlate->OnDestroyed.AddUniqueDynamic(
				this,
				&AmultiplayerMovingPlatform::HandleActivationPlateDestroyed);
		}
	}

	RefreshActivation();
}

// 对称解除人数与外部压力板订阅，支持关卡卸载和运行期销毁。
void AmultiplayerMovingPlatform::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 无论当前选择哪种激活来源都清理，避免配置切换后遗漏旧订阅。
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (ActivationPlate != nullptr)
	{
		ActivationPlate->OnPlateActiveChanged.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleActivationPlateChanged);
		ActivationPlate->OnDestroyed.RemoveDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleActivationPlateDestroyed);
	}

	Super::EndPlay(EndPlayReason);
}

/** 人数变化后按当前激活来源更新平台期望端点。 */
void AmultiplayerMovingPlatform::HandleOccupancyChanged(int32 /*玩家数量*/)
{
	// 权威和激活来源统一由 RefreshActivation 检查；参数可能已被重入通知更新。
	RefreshActivation();
}

/** 外部压力板开关改变时重算激活，实际移动仍统一由 Transporter 执行。 */
void AmultiplayerMovingPlatform::HandleActivationPlateChanged(
	AmultiplayerPressurePlate* Plate,
	bool bIsActive)
{
	// Plate/bIsActive 只说明触发来源；RefreshActivation 会复核配置引用和当前状态。
	RefreshActivation();
}

/** 依赖压力板销毁后清空引用并重算；单程平台不因失活强制返回起点。 */
void AmultiplayerMovingPlatform::HandleActivationPlateDestroyed(
	AActor* DestroyedActor)
{
	if (DestroyedActor != ActivationPlate)
	{
		return;
	}

	ActivationPlate = nullptr;
	RefreshActivation();
}

/** 服务器把互斥的两类触发来源转换成一个目标状态，避免各自重复实现移动控制。 */
void AmultiplayerMovingPlatform::RefreshActivation()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bShouldActivate =
		ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate
			? ActivationPlate != nullptr && ActivationPlate->IsPlateActive()
			: PlayerOccupancy->GetPlayerCount() >= RequiredPlayers;
	Transporter->SetTransportActive(bShouldActivate);
}
