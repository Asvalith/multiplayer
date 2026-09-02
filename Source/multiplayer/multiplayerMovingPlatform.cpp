// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerMovingPlatform.h"

#include "Components/ArrowComponent.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "multiplayerPlayerOccupancyComponent.h"
#include "multiplayerPressurePlate.h"
#include "multiplayerTransporterComponent.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerMovingPlatform::AmultiplayerMovingPlatform()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	// (*) 平台会影响玩家站立位置，连续变换由服务器生成并通过 ActorMovement 复制。
	SetReplicateMovement(true);
	// 移动时最高按 30Hz 发送，稳定后允许降到 10Hz；在合作 Demo 中平衡平滑度与带宽。
	// 这不是客户端渲染帧率，网络层仍会对收到的移动快照做平滑处理。
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);

	PlatformRoot = CreateDefaultSubobject<USceneComponent>(TEXT("PlatformRoot"));
	SetRootComponent(PlatformRoot);
	PlatformRoot->SetMobility(EComponentMobility::Movable);

	PlatformMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlatformMesh"));
	PlatformMesh->SetupAttachment(PlatformRoot);
	PlatformMesh->SetMobility(EComponentMobility::Movable);
	PlatformMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlatformMesh->SetRelativeScale3D(FVector(2.5f, 2.5f, 0.25f));

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
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

void AmultiplayerMovingPlatform::BeginPlay()
{
	Super::BeginPlay();

	// (**) 端点是平台的子组件，移动前先保存世界坐标，否则端点会跟着平台一起移动。
	Transporter->ConfigureWorldTargets(
		StartPoint->GetComponentLocation(),
		TargetPoint->GetComponentLocation());

	// 统一绑定组件事件，实际规则入口仍会检查 Authority 和 ActivationSource。
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerMovingPlatform::HandleOccupancyChanged);

	if (!HasAuthority())
	{
		// 客户端只接收平台 Transform；关闭本地触发体可减少无意义的 Overlap 计算。
		ActivationVolume->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	if (ActivationSource == EMovingPlatformActivationSource::PlatformOccupancy)
	{
		// 自身占用模式复用通用人数组件，由它处理多碰撞体和 Pawn 销毁。
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
		}
	}

	RefreshActivation();
}

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
	}

	Super::EndPlay(EndPlayReason);
}

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

void AmultiplayerMovingPlatform::HandleActivationPlateChanged(
	AmultiplayerPressurePlate* Plate,
	bool bIsActive)
{
	// Plate/bIsActive 只说明触发来源；RefreshActivation 会复核配置引用和当前状态。
	RefreshActivation();
}

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
