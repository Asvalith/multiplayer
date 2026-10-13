// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerKeySocket.h"

#include "Mechanisms/multiplayerCoopKey.h"
#include "Core/multiplayerGameMode.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"

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
