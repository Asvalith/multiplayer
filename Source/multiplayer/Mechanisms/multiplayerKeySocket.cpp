// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerKeySocket.h"

#include "Mechanisms/multiplayerCoopKey.h"
#include "Core/multiplayerGameMode.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

AmultiplayerKeySocket::AmultiplayerKeySocket()
{
	PrimaryActorTick.bCanEverTick = false;
	// 保持网络 Actor 身份，供客户端解析钥匙的附着父节点。
	bReplicates = true;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);

	SocketMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SocketMesh"));
	SocketMesh->SetupAttachment(SceneRoot);
	SocketMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	KeyDisplayPoint = CreateDefaultSubobject<USceneComponent>(TEXT("KeyDisplayPoint"));
	KeyDisplayPoint->SetupAttachment(SceneRoot);
	KeyDisplayPoint->SetRelativeLocation(FVector(0.0f, 0.0f, 75.0f));
}

bool AmultiplayerKeySocket::StoreCollectedKey(AmultiplayerCoopKey* Key)
{
	if (!HasAuthority() || IsActorBeingDestroyed() || bActivated || !IsValid(Key) || Key->IsActorBeingDestroyed())
	{
		return false;
	}

	AmultiplayerGameMode* CoopGameMode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>();
	if (CoopGameMode == nullptr)
	{
		return false;
	}
	// 回调与进度广播均同步执行；在调用前占位，防止同一插槽重入登记。
	bActivated = true;
	bActivated = CoopGameMode->RegisterActivatedKey([this, Key]()
	{
		return Key->InstallAtSocket(KeyDisplayPoint);
	});
	return bActivated;
}
