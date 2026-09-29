// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerKeySocket.h"

#include "Mechanisms/multiplayerCoopCarryComponent.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Core/multiplayerGameMode.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"

/*
 * 构造阶段搭建插槽外观、归位显示点和玩家触发区。插槽保留网络 Actor 身份，
 * 但不复制自己的本地门闩；两名玩家真正共享的是 GameState 中的总进度。
 */
AmultiplayerKeySocket::AmultiplayerKeySocket()
{
	PrimaryActorTick.bCanEverTick = false;
	// 保持网络 Actor 身份，确保客户端 HasAuthority() 为 false；插槽自身没有额外复制属性。
	bReplicates = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	SocketMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SocketMesh"));
	SocketMesh->SetupAttachment(SceneRoot);
	SocketMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	KeyDisplayPoint = CreateDefaultSubobject<USceneComponent>(TEXT("KeyDisplayPoint"));
	KeyDisplayPoint->SetupAttachment(SceneRoot);
	KeyDisplayPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 75.0f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->SetBoxExtent(FVector(100.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
}

/*
 * 只有服务器绑定玩家进入事件，客户端关闭触发体并等待权威进度同步。
 * 这样角色携带钥匙进入插槽时，不会在每台机器上各自消费一次。
 */
void AmultiplayerKeySocket::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// 插槽规则只在服务器执行，客户端通过 GameState 获取共享进度。
		ActivationTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
			this,
			&AmultiplayerKeySocket::HandleSocketOverlap);
	}
	else
	{
		ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
}

/* 关卡卸载或插槽销毁时解除碰撞回调，避免外部组件继续调用即将结束生命周期的 Actor。 */
void AmultiplayerKeySocket::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerKeySocket::HandleSocketOverlap);
	Super::EndPlay(EndPlayReason);
}

/*
 * 预绑定入口和普通携带入口共用提交顺序，不在校验进度之前修改钥匙。
 */
bool AmultiplayerKeySocket::StoreCollectedKey(AmultiplayerCoopKey* Key)
{
	return CommitServerActivation(Key, true);
}

/*
 * 普通携带路径的服务器入口。角色进入区域后，同时核对角色携带槽和 Key::Holder；
 * 只有两边仍指向同一关系时才消费钥匙并登记进度。
 */
void AmultiplayerKeySocket::HandleSocketOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	if (!HasAuthority() || bActivated)
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr || !Character->IsPlayerControlled())
	{
		return;
	}

	// 不能仅凭“角色进入区域”增加进度，必须找到双方记录一致的实际携带钥匙。
	CommitServerActivation(FindCarriedKey(Character), false);
}

/*
 * 先占用本插槽，再由 GameMode 校验共享进度；借用回调只同步执行一次，不另建事务框架。
 * 只有钥匙操作成功才关闭触发区并发布进度；失败仅释放门闩，保留原钥匙和可重试的触发区。
 */
bool AmultiplayerKeySocket::CommitServerActivation(AmultiplayerCoopKey* Key, bool bInstall)
{
	if (!HasAuthority() || bActivated || !IsValid(Key) || Key->IsActorBeingDestroyed())
	{
		return false;
	}

	AmultiplayerGameMode* CoopGameMode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>();
	if (CoopGameMode == nullptr)
	{
		return false;
	}
	bActivated = true;
	const bool bCommitted = CoopGameMode->RegisterActivatedKey([this, Key, bInstall]()
	{
		if (!(bInstall ? Key->InstallAtSocket(KeyDisplayPoint) : Key->ConsumeAtSocket()))
		{
			return false;
		}
		ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return true;
	});
	if (!bCommitted)
	{
		bActivated = false;
	}
	return bCommitted;
}

/*
 * 服务器只读查询角色当前携带物，并交叉验证钥匙的反向 Holder。
 * 双向检查不是两份网络真相，而是用服务器缓存发现迟到清理或失效引用。
 */
AmultiplayerCoopKey* AmultiplayerKeySocket::FindCarriedKey(
	ACharacter* Character) const
{
	if (Character == nullptr)
	{
		return nullptr;
	}

	const UmultiplayerCoopCarryComponent* CarryComponent =
		Character->FindComponentByClass<UmultiplayerCoopCarryComponent>();
	AmultiplayerCoopKey* Key =
		CarryComponent != nullptr ? CarryComponent->GetCarriedKey() : nullptr;
	// (**) 同时核对携带槽和钥匙 Holder，任何一侧出现迟到清理都不会误消费钥匙。
	return Key != nullptr && Key->IsHeldBy(Character) ? Key : nullptr;
}
