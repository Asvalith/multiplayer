// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerCoopKey.h"

#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "Mechanisms/multiplayerCoopCarryComponent.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Net/UnrealNetwork.h"

/*
 * 构造阶段只建立网格和服务器拾取触发体，并声明需要复制的 Actor。
 * 自由状态的位置由 ReplicateMovement 同步；旋转只作用于网格，是两端可独立播放的外观效果。
 */
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

/*
 * 仅在钥匙处于自由、未安装状态时旋转网格。该 Tick 不参与拾取判定，也不修改可复制的根变换；
 * 状态改变后由 RefreshVisualTick 立即关闭，避免已持有或已归位的钥匙继续空转。
 */
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

/*
 * 服务器绑定拾取 Overlap，客户端关闭同一触发体，只根据复制状态恢复表现。
 * 这样两个客户端同时碰到钥匙时，只有服务器碰撞世界会给出一次最终归属。
 */
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

/*
 * 对称解除触发体和持有者上的外部事件。无论是关卡卸载、钥匙被消费还是 Actor 主动销毁，
 * 都不能把指向本对象的动态 Delegate 留在外部对象中。
 */
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

/* Holder 表示持有关系，bInstalled 表示已归位；根位置和附件关系另外由 Actor 的内建复制处理。 */
void AmultiplayerCoopKey::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopKey, Holder);
	DOREPLIFETIME(AmultiplayerCoopKey, bInstalled);
}

/*
 * 服务器拾取入口。调用来自碰撞系统，可能同帧到达多次，所以先复核 Holder、安装状态和角色类型，
 * 再进入 PickupBy 完成真正的状态提交。
 */
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

/*
 * 完成一次服务器拾取事务。若关卡为钥匙预先配置了 DestinationSocket，则触碰钥匙后直接尝试
 * 归位到该插槽；没有预绑定或归位失败时，才进入普通“角色携带，随后走进插槽”的备用路径。
 * (**) 两条路径互斥，不能把预绑定自动归位描述成玩家已经携带了钥匙。
 */
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

/*
 * 预绑定自动归位路径的服务器提交函数。成功后清空可能存在的持有关系、关闭拾取碰撞、
 * 将钥匙吸附到插槽显示点，并复制 Installed 状态和附件关系。
 */
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

/*
 * 普通携带路径到达插槽后的服务器提交函数。钥匙已完成目标后不再需要保留独立展示 Actor，
 * 因此先对称清理 Holder/携带槽，再销毁钥匙；插槽随后登记共享进度。
 */
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

/*
 * 持有者销毁的兜底路径，仅服务器处理。断线或 Pawn 替换未必产生碰撞离开事件，
 * 因此必须主动释放双方引用，让钥匙回到可拾取状态。
 */
void AmultiplayerCoopKey::HandleHolderDestroyed(AActor* DestroyedActor)
{
	if (!HasAuthority() || DestroyedActor != Holder)
	{
		return;
	}

	ReleaseHolder();
}

/*
 * 持有关系的唯一清理出口：解绑 OnDestroyed、清角色携带槽、解除附件和网络 Owner，
 * 最后刷新碰撞与 Tick。集中收口可避免安装、消费和断线清理各漏掉一部分状态。
 */
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
	Holder = nullptr;
	SetOwner(nullptr);
	ApplyHeldState();
	RefreshVisualTick();
	ForceNetUpdate();
}

/* 客户端收到 Holder 后只恢复附件和外观，不重新执行服务器拾取规则。 */
void AmultiplayerCoopKey::OnRep_Holder()
{
	HandleHolderChanged();
}

/* 客户端收到 Installed 后关闭自由状态表现；实际插槽附件关系由 Actor 附件复制恢复。 */
void AmultiplayerCoopKey::OnRep_Installed()
{
	HandleInstalledChanged();
}

/* 服务器赋值和客户端收到 Holder 后共用的表现刷新入口，避免两端各维护一份状态分支。 */
void AmultiplayerCoopKey::HandleHolderChanged()
{
	ApplyHeldState();
	RefreshVisualTick();
}

/* 归位后关闭拾取和旋转；安装位置由附件关系处理，这里不再次移动钥匙。 */
void AmultiplayerCoopKey::HandleInstalledChanged()
{
	if (!bInstalled)
	{
		return;
	}

	PickupTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RefreshVisualTick();
}

/* 根据当前复制状态统一决定是否需要旋转 Tick，避免各状态分支分别开关后发生遗漏。 */
void AmultiplayerCoopKey::RefreshVisualTick()
{
	// 旋转只是待拾取表现；被携带或安装后关闭 Tick，避免每把钥匙长期空转。
	SetActorTickEnabled(
		Holder == nullptr
		&& !bInstalled
		&& RotationSpeedDegrees > 0.0f
		&& !RotationAxis.IsNearlyZero());
}

/*
 * 根据 Holder 重建当前机器上的碰撞和附件表现。服务器直接赋值与客户端 RepNotify 都复用此处，
 * 但只有服务器会重新开启可拾取碰撞，客户端始终没有本地判定权。
 */
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

	// 优先挂到骨骼网格的命名 Socket；没有骨骼网格时回退到角色根组件。
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
