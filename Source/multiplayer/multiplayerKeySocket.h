// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerKeySocket.generated.h"

class ACharacter;
class AmultiplayerCoopKey;
class UBoxComponent;
class USceneComponent;
class UStaticMeshComponent;

/**
 * 在服务器消耗玩家携带的钥匙，并记录插槽是否已经激活。
 *
 * 当前存在两个兼容入口：玩家携带钥匙进入触发区时消费并销毁钥匙；旧关卡中钥匙预绑定
 * DestinationSocket 时则把钥匙安装到 KeyDisplayPoint。两条路径最终都只能经过一次
 * CommitServerActivation，再由 GameMode 增加共享进度。
 *
 * (*) Actor 保持网络权威身份，但不再复制 bActivated；客户端只需要 GameState 中的共享目标进度。
 * 这也意味着当前实现不提供“逐个插槽的客户端激活表现”，不能把 bActivated 当成可复制 UI 数据。
 * (**) Overlap 和外部直接安装都可能到达提交函数，因此提交前还要再次检查 bActivated，
 * 不能只依赖入口处的一次判断。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerKeySocket : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerKeySocket();

	// 仅供服务器规则或初始化检查；客户端没有该布尔值的复制保证。
	bool IsActivated() const { return bActivated; }

	/**
	 * 兼容由钥匙直接指定插槽的旧关卡数据：安装到显示点后提交目标。
	 * @return 只有权威端、未激活且钥匙成功进入 Installed 状态时返回 true。
	 */
	bool StoreCollectedKey(AmultiplayerCoopKey* Key);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleSocketOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

private:
	// 同时核对 CarryComponent::CarriedKey 与 Key::Holder，避免任一侧迟到清理导致误消费。
	AmultiplayerCoopKey* FindCarriedKey(ACharacter* Character) const;
	// 两种入口共用的唯一提交点；先关闭触发并锁定 bActivated，再通知 GameMode。
	void CommitServerActivation();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<UStaticMeshComponent> SocketMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Coop|Key Socket", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USceneComponent> KeyDisplayPoint;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Key Socket")
	TObjectPtr<UBoxComponent> ActivationTrigger;

	// 服务器本地的一次性门闩，不复制；共享结果由 GameState 的 ActivatedKeys 表达。
	bool bActivated = false;
};
