// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerCoopKey.h"

#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "Core/multiplayerGameMode.h"
#include "Core/multiplayerLog.h"
#include "Net/UnrealNetwork.h"


/////仅用于检测玩家，视觉表现，提交插槽
//构造函数初始化组件
AmultiplayerCoopKey::AmultiplayerCoopKey()
{
	//有tick函数，默认不启用
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	//参与网络复制，客户端能够看到相同表现
	bReplicates = true;
	//开启移动位置自动同步
	SetReplicateMovement(true);
	//根组件
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	//网格组件，不参与碰撞，旋转展示
	KeyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("KeyMesh"));
	KeyMesh->SetupAttachment(SceneRoot);
	KeyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	//拾取触发器，球形范围，只有Pawn重叠时才触发
	//设置单独碰撞通道，避免与其他物体干扰
	PickupTrigger = CreateDefaultSubobject<USphereComponent>(TEXT("PickupTrigger"));
	PickupTrigger->SetupAttachment(SceneRoot);
	PickupTrigger->SetSphereRadius(100.0f);
	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

// Tick函数，旋转展示
void AmultiplayerCoopKey::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (RotationSpeedDegrees <= 0.0f || RotationAxis.IsNearlyZero())
	{
		return;
	}
	//旋转轴归一化，避免缩放影响旋转速度
	const FQuat RotationDelta(RotationAxis.GetSafeNormal(),FMath::DegreesToRadians(RotationSpeedDegrees * DeltaSeconds));
	KeyMesh->AddLocalRotation(RotationDelta);
}

// BeginPlay函数，初始化重叠回调
void AmultiplayerCoopKey::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		if (!IsValid(DestinationSocket))
		{
			UE_LOG(LogMultiplayer, Warning, TEXT("Key[%s] has no DestinationSocket; collection is disabled until configured."), *GetName());
		}
		PickupTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerCoopKey::HandlePickupOverlap);
	}
	// 初始安装状态也要刷新碰撞和旋转展示，避免复制后客户端显示不一致。
	OnRep_Installed();
}

//EndPlay函数，移除重叠回调
void AmultiplayerCoopKey::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	PickupTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerCoopKey::HandlePickupOverlap);
	Super::EndPlay(EndPlayReason);
}

//复制属性注册，bInstalled用于同步安装状态
void AmultiplayerCoopKey::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopKey, bInstalled);
}

//拾取检测，玩家角色重叠时尝试存储钥匙到插槽(仅服务端执行）
void AmultiplayerCoopKey::HandlePickupOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bInstalled)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (IsValid(Character) && !Character->IsActorBeingDestroyed() && Character->IsPlayerControlled()
		&& IsValid(DestinationSocket))
	{
		DestinationSocket->StoreCollectedKey(this);
	}
}

//安装钥匙到插槽，成功返回true，失败返回false（仅服务端执行）
bool AmultiplayerCoopKey::InstallAtSocket(USceneComponent* SocketPoint)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bInstalled
		|| !IsValid(SocketPoint) || !SocketPoint->IsRegistered() || SocketPoint == GetRootComponent()
		|| SocketPoint->GetWorld() != GetWorld())
	{
		return false;
	}

	//先占位
	//附着失败则回滚安装状态
	bInstalled = true;
	if (!AttachToComponent(
		SocketPoint,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale))
	{
		bInstalled = false;
		return false;
	}
	//设置所有者为插槽的拥有者，便于权限管理和网络同步
	//设置归属后立即调用复制属性回调，更新旋转展示和拾取触发器状态
	SetOwner(SocketPoint->GetOwner());
	OnRep_Installed();
	ForceNetUpdate();
	return true;
}

//复制属性回调，更新旋转展示和拾取触发器状态
void AmultiplayerCoopKey::OnRep_Installed()
{
	const bool bFree = !bInstalled;
	SetActorTickEnabled(
		bFree
		&& RotationSpeedDegrees > 0.0f
		&& !RotationAxis.IsNearlyZero());
	PickupTrigger->SetCollisionEnabled(
		HasAuthority() && bFree
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);
}

// Copyright Epic Games, Inc. All Rights Reserved.

//构造函数初始化
AmultiplayerKeySocket::AmultiplayerKeySocket()
{
	PrimaryActorTick.bCanEverTick = false;
	//视觉表现需要网络同步
	bReplicates = true;
	//场景根组件
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	//网格组件，仅视觉
	SocketMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SocketMesh"));
	SocketMesh->SetupAttachment(SceneRoot);
	SocketMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	//钥匙展示点组件，钥匙安装时的相对位置
	KeyDisplayPoint = CreateDefaultSubobject<USceneComponent>(TEXT("KeyDisplayPoint"));
	KeyDisplayPoint->SetupAttachment(SceneRoot);
	KeyDisplayPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 75.0f));
}

//存入钥匙，仅服务端调用，返回是否成功登记
bool AmultiplayerKeySocket::StoreCollectedKey(AmultiplayerCoopKey* Key)
{
	//非服务端、插槽已激活、钥匙无效或销毁中，均不登记
	if (!HasAuthority() || IsActorBeingDestroyed() || bActivated || !IsValid(Key) || Key->IsActorBeingDestroyed())
	{
		return false;
	}
	//获取游戏模式，若无则无法登记
	AmultiplayerGameMode* CoopGameMode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>();
	if (CoopGameMode == nullptr)
	{
		return false;
	}
	//先标记激活，避免重入；注册回调，若安装失败则不再尝试
	bActivated = true;
	bActivated = CoopGameMode->RegisterActivatedKey([this, Key]()
	{
		return Key->InstallAtSocket(KeyDisplayPoint);
	});
	return bActivated;
}
