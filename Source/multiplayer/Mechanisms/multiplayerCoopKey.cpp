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
 * 状态改变后由 RefreshKeyState 关闭，避免已持有或已归位的钥匙继续空转。
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
	RefreshKeyState();
}

/*
 * 对称解除触发体和持有者上的外部事件。无论是关卡卸载、钥匙被消费还是 Actor 主动销毁，
 * 都不能把指向本对象的动态 Delegate 留在外部对象中。
 */
void AmultiplayerCoopKey::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 可能来自关卡卸载，也可能来自插槽消费后的 Destroy；两种路径都必须先移除外部回调。
	if (HasAuthority())
	{
		ReleaseHolder();
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
	if (!HasAuthority() || !IsValid(Character) || Character->IsActorBeingDestroyed()
		|| IsActorBeingDestroyed() || Holder != nullptr || bInstalled)
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
	// 附着只由服务器写，客户端通过 Actor 附件复制恢复；Holder 的 RepNotify 不再重复挂接。
	USceneComponent* AttachParent = Character->GetMesh();
	if (AttachParent == nullptr)
	{
		AttachParent = Character->GetRootComponent();
	}
	if (AttachParent == nullptr || !AttachToComponent(
		AttachParent, FAttachmentTransformRules::SnapToTargetNotIncludingScale, CarrySocketName))
	{
		ReleaseHolder();
		return;
	}
	// 网络 Owner 和物理附着是不同关系，二者均在拾取成功后确定。
	SetOwner(Character);
	RefreshKeyState();
	ForceNetUpdate();
}

/*
 * 预绑定自动归位路径的服务器提交函数。成功后清空可能存在的持有关系、关闭拾取碰撞、
 * 将钥匙吸附到插槽显示点，并复制 Installed 状态和附件关系。
 */
bool AmultiplayerCoopKey::InstallAtSocket(USceneComponent* SocketPoint)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bInstalled
		|| !IsValid(SocketPoint) || !SocketPoint->IsRegistered() || SocketPoint == GetRootComponent()
		|| SocketPoint->GetWorld() != GetWorld())
	{
		return false;
	}

	// 先占用安装状态以拦截重入；本类使用非物理 SceneRoot，附着校验失败不会先释放携带关系。
	bInstalled = true;
	if (!AttachToComponent(
		SocketPoint,
		FAttachmentTransformRules::SnapToTargetNotIncludingScale))
	{
		bInstalled = false;
		return false;
	}
	ReleaseHolder();
	SetOwner(SocketPoint->GetOwner());
	RefreshKeyState();
	ForceNetUpdate();
	return true;
}

/*
 * 普通携带路径到达插槽后的服务器提交函数。钥匙已完成目标后不再需要保留独立展示 Actor，
 * Destroy 被拒绝时保留原状态；成功销毁会在 EndPlay 对称清理 Holder/携带槽。
 */
bool AmultiplayerCoopKey::ConsumeAtSocket()
{
	if (!HasAuthority() || IsActorBeingDestroyed() || Holder == nullptr || bInstalled)
	{
		return false;
	}

	return Destroy();
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
	// 先分离，再恢复碰撞；恢复碰撞可能立即被另一个玩家拾取，此后不再执行旧的分离操作。
	DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	RefreshKeyState();
	ForceNetUpdate();
}

/*
 * 只清理持有关系，不提前恢复自由状态或发送中间状态。
 * 安装随后直接挂到插槽，消费随后销毁；只有玩家销毁后的掉落路径才恢复碰撞、附件和 Tick。
 */
void AmultiplayerCoopKey::ReleaseHolder()
{
	if (Holder == nullptr)
	{
		return;
	}

	Holder->OnDestroyed.RemoveDynamic(this, &AmultiplayerCoopKey::HandleHolderDestroyed);
	if (UmultiplayerCoopCarryComponent* CarryComponent =
		Holder->FindComponentByClass<UmultiplayerCoopCarryComponent>())
	{
		CarryComponent->ClearCarriedKey(this);
	}

	Holder = nullptr;
	SetOwner(nullptr);
}

/* Holder 与 Installed 可以先后到达；这里只刷新外观，不覆盖引擎已经复制的附件关系。 */
void AmultiplayerCoopKey::OnRep_Holder()
{
	RefreshKeyState();
}

/* 客户端收到 Installed 后关闭自由状态表现；实际插槽附件关系由 Actor 附件复制恢复。 */
void AmultiplayerCoopKey::OnRep_Installed()
{
	RefreshKeyState();
}

/* 不重复维护“自由”枚举；由现有状态推导。碰撞必须最后更新，避免重入后写回旧 Tick/附着。 */
void AmultiplayerCoopKey::RefreshKeyState()
{
	const bool bFree = Holder == nullptr && !bInstalled;
	SetActorTickEnabled(
		bFree
		&& RotationSpeedDegrees > 0.0f
		&& !RotationAxis.IsNearlyZero());
	PickupTrigger->SetCollisionEnabled(
		HasAuthority() && bFree
			? ECollisionEnabled::QueryOnly
			: ECollisionEnabled::NoCollision);
}
