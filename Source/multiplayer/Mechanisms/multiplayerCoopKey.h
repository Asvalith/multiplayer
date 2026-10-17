// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.generated.h"

class AmultiplayerCoopKey;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;
class AmultiplayerKeySocket;

/**
 * 玩家触碰后，服务器将钥匙安装到关卡预绑定的 DestinationSocket。
 * 安装失败时保留原位，玩家可再次触碰重试；不提供携带、丢弃或插槽碰撞交付。
 * 客户端通过 bInstalled 恢复展示状态，通过 Actor 原生附件复制恢复安装位置。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopKey : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopKey();

	/** 仅服务器调用。附着失败不改变钥匙状态；成功后不可再次拾取。 */
	bool InstallAtSocket(USceneComponent* SocketPoint);

	// 仅未安装时旋转展示网格，不修改根组件位置。
	virtual void Tick(float DeltaSeconds) override;

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandlePickupOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	// 只刷新碰撞和 Tick，不覆盖 Actor 原生附件复制。
	UFUNCTION()
	void OnRep_Installed();

private:
	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<UStaticMeshComponent> KeyMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USphereComponent> PickupTrigger;

	// 保留字段名与实例配置，现有关卡的钥匙—插槽关联无需重绑。
	UPROPERTY(EditInstanceOnly, Category = "Coop|Key")
	TObjectPtr<AmultiplayerKeySocket> DestinationSocket;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual", meta = (ClampMin = "0.0"))
	float RotationSpeedDegrees = 90.0f;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual")
	FVector RotationAxis = FVector::UpVector;

	UPROPERTY(ReplicatedUsing = OnRep_Installed)
	bool bInstalled = false;
};

// Copyright Epic Games, Inc. All Rights Reserved.

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
