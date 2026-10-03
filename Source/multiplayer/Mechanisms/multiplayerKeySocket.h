// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerKeySocket.generated.h"

class AmultiplayerCoopKey;
class USceneComponent;
class UStaticMeshComponent;

/**
 * 接收预绑定钥匙的自动归位请求，成功后通过 GameMode 登记一次目标进度。
 * 插槽不检测玩家碰撞；钥匙安装位置由原生附件复制恢复，共享进度由 GameState 复制。
 * bActivated 仅作服务器防重复提交标记，不另复制一份 UI 状态。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerKeySocket : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerKeySocket();

	/**
	 * 仅服务器调用。先校验共享进度，再安装钥匙；失败解锁插槽，钥匙保留原位。
	 * @return 钥匙安装且进度登记成功时返回 true；重复请求返回 false。
	 */
	bool StoreCollectedKey(AmultiplayerCoopKey* Key);

private:
	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<UStaticMeshComponent> SocketMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Key Socket", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> KeyDisplayPoint;

	// 服务器本地的一次性门闩，不复制；共享结果由 GameState 的 ActivatedKeys 表达。
	bool bActivated = false;
};
