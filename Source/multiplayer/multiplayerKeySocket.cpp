// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerKeySocket.h"

#include "multiplayerCoopCarryComponent.h"
#include "multiplayerCoopKey.h"
#include "multiplayerGameMode.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"

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

void AmultiplayerKeySocket::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ActivationTrigger->OnComponentBeginOverlap.RemoveDynamic(
		this,
		&AmultiplayerKeySocket::HandleSocketOverlap);
	Super::EndPlay(EndPlayReason);
}

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
