// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerKeySocket.h"

#include "multiplayerCoopCarryComponent.h"
#include "multiplayerCoopKey.h"
#include "multiplayerGameMode.h"
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
 * 供钥匙预绑定路径和自动测试调用的服务器入口。它要求钥匙先成功归位到显示点，
 * 再锁定插槽并登记进度，避免出现“进度增加了但钥匙没有归位”的半完成状态。
 */
bool AmultiplayerKeySocket::StoreCollectedKey(AmultiplayerCoopKey* Key)
{
	// InstallAtSocket 成功后钥匙已经进入终态；只有这时才允许提交共享进度，保持表现和规则一致。
	if (!HasAuthority() || bActivated || Key == nullptr || !Key->InstallAtSocket(KeyDisplayPoint))
	{
		return false;
	}

	CommitServerActivation();

	return true;
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
	AmultiplayerCoopKey* Key = FindCarriedKey(Character);
	if (Key == nullptr || !Key->ConsumeAtSocket())
	{
		return;
	}

	CommitServerActivation();
}

/*
 * 两条钥匙路径共用的一次性提交点。先关闭触发区和设置本地门闩，再通知 GameMode；
 * (**) 这个顺序能挡住重复 Overlap，以及 GameMode 通知链中可能同步发生的再次调用。
 */
void AmultiplayerKeySocket::CommitServerActivation()
{
	if (!HasAuthority() || bActivated)
	{
		// (**) 所有入口最终都会到这里，二次检查可拦住重复 Overlap 和同帧重复提交。
		return;
	}

	// 先锁门闩并关闭碰撞，再调用外部 GameMode；即使后续产生嵌套事件也无法重复进入提交。
	bActivated = true;
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
	{
		CoopGameMode->RegisterActivatedKey();
	}
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
