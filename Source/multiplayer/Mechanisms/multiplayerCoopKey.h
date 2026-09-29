// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopKey.generated.h"

class ACharacter;
class USceneComponent;
class USphereComponent;
class UStaticMeshComponent;
class AmultiplayerKeySocket;

/**
 * 由服务器决定归属并复制给客户端的合作钥匙。
 *
 * 当前有两条明确流程：关卡预绑定 DestinationSocket 时，玩家触碰后钥匙直接自动归位；没有预绑定
 * 时，钥匙先由角色携带，角色进入插槽后钥匙被消费。Holder 和 bInstalled 是客户端恢复表现所需的
 * 最小状态：Holder 表示归属，bInstalled 表示已经归位；服务器修改附着，客户端仅由 Actor 内建
 * AttachmentReplication 恢复附着关系。自由位置由 ReplicateMovement 同步，携带交付后销毁。
 *
 * 拾取使用服务器端 Overlap，不需要客户端提交“我捡到了”的自定义 RPC。服务器根据自己的碰撞
 * 世界和当前状态作决定，对两个客户端近乎同时触碰同一把钥匙的情况按事件顺序只接受第一个。
 *
 * (*) 客户端只根据 Holder 和 bInstalled 更新表现，拾取、丢弃和安装均由服务器修改。
 * (**) Overlap 可能重复触发，也可能被两名玩家近乎同时触发；修改状态前必须再次检查
 * Holder、bInstalled 和角色携带槽，才能避免重复拾取。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopKey : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopKey();

	// Tick 仅用于自由钥匙的旋转展示；持有或安装后会关闭，避免长期空转。
	virtual void Tick(float DeltaSeconds) override;

	// 注册 Holder 与 bInstalled；自由状态的 Transform 由 Actor 的 ReplicateMovement 负责。
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	bool IsHeldBy(const ACharacter* Character) const { return Character != nullptr && Holder == Character && !bInstalled; }

	/** 携带路径：只有 Destroy 被接受才消费，双方持有关系在 EndPlay 中清理。 */
	bool ConsumeAtSocket();
	/** 兼容预绑定插槽路径：服务器将钥匙固定到显示点并进入不可拾取的 Installed 状态。 */
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

	// RepNotify 只刷新碰撞/Tick，不与 Actor 附件复制争抢附着关系。
	UFUNCTION()
	void OnRep_Holder();

	// 客户端安装状态更新入口；与 Holder 的处理分开，允许两种属性按任意顺序到达后收敛。
	UFUNCTION()
	void OnRep_Installed();

	// Pawn 销毁不保证触发 EndOverlap，因此服务器监听 Holder::OnDestroyed 主动释放关系。
	UFUNCTION()
	void HandleHolderDestroyed(AActor* DestroyedActor);

private:
	// 服务器拾取事务：先占用角色携带槽，再写 Holder、网络 Owner 和本地表现。
	void PickupBy(ACharacter* Character);
	// 只清理 Delegate、携带槽、Holder 和网络 Owner；安装、销毁、掉落各自提交最终表现。
	void ReleaseHolder();
	// 状态与附着先提交，最后恢复服务器自由状态的拾取碰撞；此后不得继续按旧状态写入。
	void RefreshKeyState();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<UStaticMeshComponent> KeyMesh;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key")
	TObjectPtr<USphereComponent> PickupTrigger;

	UPROPERTY(EditAnywhere, Category = "Coop|Key")
	FName CarrySocketName = TEXT("KeySocket");

	// 兼容旧关卡中“钥匙直接指定插槽”的摆放方式；新逻辑优先由插槽主动接收钥匙。
	UPROPERTY(EditInstanceOnly, Category = "Coop|Key")
	TObjectPtr<AmultiplayerKeySocket> DestinationSocket;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual", meta = (ClampMin = "0.0"))
	float RotationSpeedDegrees = 90.0f;

	UPROPERTY(EditAnywhere, Category = "Coop|Key|Visual")
	FVector RotationAxis = FVector::UpVector;

	// 安装后不可再次拾取。安装路径下应与 Holder == nullptr 保持一致。
	UPROPERTY(ReplicatedUsing = OnRep_Installed)
	bool bInstalled = false;

	// 钥匙归属的唯一复制来源；携带组件中的 CarriedKey 只是服务器缓存。
	UPROPERTY(ReplicatedUsing = OnRep_Holder)
	TObjectPtr<ACharacter> Holder;
};
