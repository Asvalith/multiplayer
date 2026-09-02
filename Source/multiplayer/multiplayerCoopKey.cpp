// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopKey.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "multiplayerCoopCarryComponent.h"
#include "multiplayerKeySocket.h"
#include "Net/UnrealNetwork.h"

AmultiplayerCoopKey::AmultiplayerCoopKey()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	// (*) 钥匙可能被玩家丢在任意世界位置，服务器需要把落点同步给客户端。
	SetReplicateMovement(true);

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	KeyMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("KeyMesh"));
	KeyMesh->SetupAttachment(SceneRoot);
	KeyMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	PickupTrigger = CreateDefaultSubobject<USphereComponent>(TEXT("PickupTrigger"));
	PickupTrigger->SetupAttachment(SceneRoot);
	PickupTrigger->SetSphereRadius(100.0f);
	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	PickupTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	PickupTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

void AmultiplayerCoopKey::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (RotationSpeedDegrees <= 0.0f || RotationAxis.IsNearlyZero())
	{
		return;
	}

	const FQuat RotationDelta(
		RotationAxis.GetSafeNormal(),
		FMath::DegreesToRadians(RotationSpeedDegrees * DeltaSeconds));
	KeyMesh->AddLocalRotation(RotationDelta);
}

void AmultiplayerCoopKey::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// 只有服务器监听拾取碰撞，客户端仅消费复制结果，避免抢拾时出现两套结论。
		PickupTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerCoopKey::HandlePickupOverlap);
	}
	else
	{
		PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}

	RefreshVisualTick();
}

void AmultiplayerCoopKey::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 可能来自关卡卸载，也可能来自插槽消费后的 Destroy；两种路径都必须先移除外部回调。
	if (Holder != nullptr)
	{
		Holder->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	}

	PickupTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerCoopKey::HandlePickupOverlap);
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopKey::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopKey, Holder);
	DOREPLIFETIME(AmultiplayerCoopKey, bInstalled);
}

void AmultiplayerCoopKey::HandlePickupOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || Holder != nullptr || bInstalled)
	{
		// (**) Overlap 可能重复或同帧到达，先检查权威和现有状态再做任何修改。
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character != nullptr && Character->IsPlayerControlled())
	{
		PickupBy(Character);
	}
}

void AmultiplayerCoopKey::PickupBy(ACharacter* Character)
{
	if (!HasAuthority() || Character == nullptr || Holder != nullptr || bInstalled)
	{
		return;
	}

	// 先兼容旧关卡的预绑定插槽；失败后仍可回落到普通携带路径，不把配置错误变成钥匙丢失。
	if (DestinationSocket != nullptr && DestinationSocket->StoreCollectedKey(this))
	{
		return;
	}

	// 先占用服务器携带槽，再提交 Holder，保证一个角色不能在相邻 Overlap 中同时拿到两把钥匙。
	UmultiplayerCoopCarryComponent* CarryComponent =
		Character->FindComponentByClass<UmultiplayerCoopCarryComponent>();
	if (CarryComponent == nullptr || !CarryComponent->TryCarryKey(this))
	{
		return;
	}

	Holder = Character;
	// (**) 玩家断线或 Pawn 被销毁时不一定产生 EndOverlap，用 OnDestroyed 释放持有关系。
	Holder->OnDestroyed.AddUniqueDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	// SetOwner 表示网络所有权；真正的视觉挂接由 ApplyHeldState 负责，两者含义不同。
	SetOwner(Character);
	HandleHolderChanged();
	ForceNetUpdate();
}

bool AmultiplayerCoopKey::InstallAtSocket(USceneComponent* SocketPoint)
{
	if (!HasAuthority() || SocketPoint == nullptr || bInstalled)
	{
		return false;
	}

	// 安装前先清理角色携带槽和 OnDestroyed 绑定，保持 bInstalled => Holder == nullptr 的状态约束。
	ReleaseHolder();
	bInstalled = true;
	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetOwner(SocketPoint->GetOwner());
	AttachToComponent(
		SocketPoint,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	HandleInstalledChanged();
	ForceNetUpdate();
	return true;
}

bool AmultiplayerCoopKey::ConsumeAtSocket()
{
	if (!HasAuthority() || Holder == nullptr)
	{
		return false;
	}

	// Destroy 之前先清理双方引用；不能等待 GC 或 EndPlay 猜测角色携带槽里保存了什么。
	ReleaseHolder();
	Destroy();
	return true;
}

void AmultiplayerCoopKey::HandleHolderDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority() || DestroyedActor != Holder)
	{
		return;
	}

	ReleaseHolder();
}

void AmultiplayerCoopKey::ReleaseHolder()
{
	if (Holder != nullptr)
	{
		// 外部对象的 Delegate 必须在解绑或销毁前移除，避免失效回调。
		Holder->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
		if (UmultiplayerCoopCarryComponent* CarryComponent =
			Holder->FindComponentByClass<UmultiplayerCoopCarryComponent>())
		{
			CarryComponent->ClearCarriedKey(this);
		}
	}

	// 先解除附着再清 Holder，使服务器保留当前世界位置；随后复制的 Transform 才是有效掉落位置。
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	// KeepWorldTransform 保留服务器上的掉落位置，重新复制后客户端不会跳回原点。
	Holder = nullptr;
	SetOwner(nullptr);
	ApplyHeldState();
	RefreshVisualTick();
	ForceNetUpdate();
}

void AmultiplayerCoopKey::OnRep_Holder()
{
	HandleHolderChanged();
}

void AmultiplayerCoopKey::OnRep_Installed()
{
	HandleInstalledChanged();
}

void AmultiplayerCoopKey::HandleHolderChanged()
{
	ApplyHeldState();
	RefreshVisualTick();
}

void AmultiplayerCoopKey::HandleInstalledChanged()
{
	if (!bInstalled)
	{
		return;
	}

	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RefreshVisualTick();
}

void AmultiplayerCoopKey::RefreshVisualTick()
{
	// 旋转只是待拾取表现；被携带或安装后关闭 Tick，避免每把钥匙长期空转。
	SetActorTickEnabled(
		Holder == nullptr
		&& !bInstalled
		&& RotationSpeedDegrees > 0.0f
		&& !RotationAxis.IsNearlyZero());
}

void AmultiplayerCoopKey::ApplyHeldState()
{
	// 碰撞只在权威端的自由状态开启；客户端即便已有相同几何体，也不能自行产生拾取结论。
	const bool bIsHeld = Holder != nullptr;
	PickupTrigger->SetCollisionEnabled(
		HasAuthority() && !bIsHeld && !bInstalled
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);

	if (!bIsHeld)
	{
		DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		return;
	}

	// 优先挂到骨骼网格的命名 Socket；没有网格时回退 Root，保证纯 C++ 测试 Pawn 仍能工作。
	USceneComponent* AttachParent = Holder->GetMesh();
	if (AttachParent == nullptr)
	{
		AttachParent = Holder->GetRootComponent();
	}

	if (AttachParent != nullptr)
	{
		AttachToComponent(
			AttachParent,
			FAttachmentTransformRules::SnapToTargetNotIncludingScale,
			CarrySocketName);
	}
}
