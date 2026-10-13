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
/** 创建平台、触发区和可视化端点，开启服务器位置复制；Actor 本身不运行 Tick。 */


//构造组件
AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(30.0f);

	//组件声明为可移动，避免蓝图中缩放后被标记为静态，导致客户端无法正确复制位置。
	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);
	//组件声明为可移动，避免蓝图中缩放后被标记为静态，导致客户端无法正确复制位置。
	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetGenerateOverlapEvents(false);
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));
	//盒形碰撞题体组件
	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->bEditableWhenInherited = true;
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	//判定玩家数量、移动的组件和起止点箭头都作为子组件，方便关卡编辑器中可视化调整。
	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
	Transporter =
		CreateDefaultSubobject<UmultiplayerTransporterComponent>(
			TEXT("Transporter"));
	//设置轨道
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
	//初始化一次
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlatformMesh->SetStaticMesh(CubeMesh.Object);
	}
}

// 服务器移动时复制速度给客户端，避免 CMC 回退到零速度导致乘客跳跃继承错误。
void AmultiplayerMovingPlatform::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	Super::PostNetReceiveVelocity(NewVelocity);
	// 运动复制已带有 LinearVelocity，不新增逐帧 RPC。网格速度为零时 CMC 会回退读取 Actor 根速度。
	if (PlatformRoot) PlatformRoot->ComponentVelocity = NewVelocity;
}

// 服务器在 BeginPlay 时读取配置表的玩家数量要求，客户端不需要知道。
void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		//客户端不需要处理激活逻辑，直接return
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	//初始化轨道起止点
	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	// 服务器读取配置表的玩家数量要求，客户端不需要知道。
	RequiredPlayers = FmultiplayerGameplayConfig::Get(this).PlatformRequiredPlayers;
	if (ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy)
	{
		//把 ActivationVolume交给 PlayerOccupancy，让它用这个盒子检测玩家
		PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
			this,
			&AmultiplayerMovingPlatform::HandleOccupancyChanged);
		PlayerOccupancy->BindTrigger(ActivationVolume);
	}
	else
	{
		// 外部压力板模式不需要平台自己的检测区域，两套激活来源保持互斥。
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		//若压力板有效，则绑定板子激活状态变化和销毁事件
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

//清理绑定
void AmultiplayerMovingPlatform::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 无论当前选择哪种来源都执行对称解绑，支持关卡卸载和运行期销毁。
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

/** 依赖压力板销毁后清空引用并取消激活；单程或往返行为由运动组件配置决定。 */
void AmultiplayerMovingPlatform::HandleActivationPlateDestroyed(
	AActor* DestroyedActor)
{
	if (DestroyedActor != ActivationPlate)
	{
		return;
	}

	ActivationPlate = nullptr;
	// 外部依赖消失后立刻回到未激活规则；是否返回起点仍由 Transporter 配置决定。
	RefreshActivation();
}

/** 服务器把互斥的两类触发来源转换成一个目标状态，避免各自重复实现移动控制。 */
void AmultiplayerMovingPlatform::RefreshActivation()
{
	if (!HasAuthority())
	{
		return;
	}

	// 两种模式最终只产出一个布尔目标，具体移动和 Tick 生命周期交给 Transporter。
	const bool bShouldActivate =
		ActivationSource == EMovingPlatformActivationSource::ExternalPressurePlate
			? ActivationPlate != nullptr && ActivationPlate->IsPlateActive()
			: PlayerOccupancy->GetPlayerCount() >= RequiredPlayers;
	Transporter->SetTransportActive(bShouldActivate);
}
