// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerMovingPlatform.h"

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
AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	// (*) 平台会影响玩家站立位置，连续变换由服务器生成并通过 ActorMovement 复制。
	SetReplicateMovement(true);
	// 常规网络更新频率设为 30Hz，不保证客户端恰好每秒收到 30 次。普通 Actor 的 ReplicateMovement 不包含
	// CharacterMovement 那套客户端预测与服务器校正，本项目也没有额外实现预测回滚。
	SetNetUpdateFrequency(30.0f);

	// 平台沿关卡预设轨道做运动，不把世界阻挡作为路径规则；Scene 根提供稳定坐标系，
	// 实际承载碰撞继续使用每个蓝图可配置的 PlatformMesh，避免固定 Box 与视觉尺寸不匹配。
	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);

	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetGenerateOverlapEvents(false);
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	// 保持原有父级，避免改变既有蓝图中由 PlatformMesh 缩放得到的触发区世界尺寸和位置。
	ActivationVolume->SetupAttachment(PlatformMesh);
	ActivationVolume->bEditableWhenInherited = true;
	ActivationVolume->SetRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	ActivationVolume->SetBoxExtent(FVector(130.0f, 130.0f, 120.0f));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
	Transporter =
		CreateDefaultSubobject<UmultiplayerTransporterComponent>(
			TEXT("Transporter"));

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

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlatformMesh->SetStaticMesh(CubeMesh.Object);
	}
}

/** 缓存固定端点；服务器只绑定选中的激活来源，并在绑定后读取一次当前状态。 */
void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	// (**) 端点是平台的子组件，移动前先保存世界坐标，否则端点会跟着平台一起移动。
	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	if (!HasAuthority())
	{
		// 客户端只接收平台 Transform；关闭本地触发体可减少无意义的 Overlap 计算。
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	if (ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy)
	{
		// 自身占用模式复用通用人数组件，由它处理多碰撞体和 Pawn 销毁。
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

/** 清理自身人数组件和外部压力板事件，防止已结束的机关继续接收玩法通知。 */
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

/** 仅自身占用模式消费人数变化；重新读当前人数后更新平台期望端点。 */
void AmultiplayerMovingPlatform::HandleOccupancyChanged(int32 /*玩家数量*/)
{
	if (!HasAuthority()
		|| ActivationSource != EMovingPlatformActivationSource::PlatformOccupancy)
	{
		return;
	}

	// 不直接相信事件参数，重新读取完整当前状态，统一所有触发来源的计算路径。
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
			: PlayerOccupancy->GetPlayerCount() >= GetRequiredOccupantCount();
	Transporter->SetTransportActive(bShouldActivate);
}
