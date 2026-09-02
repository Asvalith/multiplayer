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
 * 状态流转为：世界中可拾取 -> 被某个 Character 持有 -> 被插槽安装，或在携带路径中被插槽消费销毁。
 * Holder 和 bInstalled 是客户端恢复表现所需的最小状态：Holder 决定挂到哪个角色插槽，
 * bInstalled 决定钥匙是否已经离开拾取流程；自由状态下的世界位置由 ReplicateMovement 同步。
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

	// 返回复制的权威持有者；可能在网络更新或销毁清理期间暂时为空。
	ACharacter* GetHolder() const { return Holder; }

	bool IsHeldBy(const ACharacter* Character) const { return Holder == Character; }

	/** 携带路径：服务器清理双方持有关系后销毁钥匙，成功时返回 true。 */
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

	// 客户端 Holder 更新入口；只重建附着、碰撞和 Tick 等表现，不修改玩法结果。
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
	// 对称清理 Delegate、携带槽、网络 Owner 与附着关系；可被安装、消费和玩家销毁复用。
	void ReleaseHolder();
	// 根据当前 Holder 重建附着/分离状态，使服务器直接写入与客户端 OnRep 走同一表现路径。
	void ApplyHeldState();
	// Holder 变化后的公共收口，避免服务器路径和客户端路径产生两套视觉逻辑。
	void HandleHolderChanged();
	// Installed 变化后的公共收口，负责禁用拾取并刷新 Tick。
	void HandleInstalledChanged();
	// 仅自由且未安装的权威钥匙需要旋转 Tick；其他状态全部停 Tick。
	void RefreshVisualTick();

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
