// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.generated.h"

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

	// 仅未安装时旋转展示网格，不修改根组件位置。
	virtual void Tick(float DeltaSeconds) override;

	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 仅服务器调用。附着失败不改变钥匙状态；成功后不可再次拾取。 */
	bool InstallAtSocket(USceneComponent* SocketPoint);

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
	void RefreshKeyState();

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
