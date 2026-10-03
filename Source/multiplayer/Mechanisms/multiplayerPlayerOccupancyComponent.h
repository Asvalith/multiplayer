// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerPlayerOccupancyComponent.generated.h"

class ACharacter;
class APawn;
class AController;
class UPrimitiveComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerOccupancyChangedEvent,
	int32,
	PlayerCount);

/**
 * 服务器区域占用统计，机关自行决定激活规则与复制结果。
 * 按角色累计组件/刚体重叠次数，避免一个组件离开就把整名玩家移除。
 * 控制器变化刷新玩家资格；角色销毁补足未收到 EndOverlap 的清理。
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerPlayerOccupancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerPlayerOccupancyComponent();

	/** 重绑定时重建已有重叠，最终成员变化只通知一次；客户端关闭本地触发体碰撞。 */
	void BindTrigger(
		UPrimitiveComponent* InTrigger,
		bool bInRequirePlayerControlledCharacter = true);

	// 对称移除重叠、销毁、控制器变化 Delegate，并清空临时计数；可安全重复调用。
	void UnbindTrigger();

	// 返回有效弱引用的数量，不把同一角色的多个碰撞组件重复算作多个玩家。
	int32 GetPlayerCount() const;
	// 追加当前仍有效的不同角色，不清空输出数组；供合作门跨压力板合并玩家集合。
	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	// 普通进出在人数变化时广播；重绑定时即使人数相同，成员更换也广播，供门重算玩家集合。
	FmultiplayerOccupancyChangedEvent OnOccupancyChanged;

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	// 静默清理供普通解绑和重绑定复用，由调用方统一通知最终状态。
	void UnbindTriggerInternal();

	// 这些监听只由 BindTrigger 的服务器分支安装；事件直接更新成员记录。
	UFUNCTION()
	void HandleBeginOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex,
		bool bFromSweep,
		const FHitResult& SweepResult);

	UFUNCTION()
	void HandleEndOverlap(
		UPrimitiveComponent* OverlappedComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComponent,
		int32 OtherBodyIndex);

	UFUNCTION()
	void HandleOccupantDestroyed(AActor* DestroyedActor);

	UFUNCTION()
	void HandleControllerChanged(APawn* Pawn, AController* OldController, AController* NewController);

	// 先记录物理重叠候选，不在此排除未被玩家控制的角色，否则原地 Possess 无法补入人数。
	ACharacter* GetOverlapCandidate(AActor* OtherActor) const;
	bool CanCountAsPlayer(const ACharacter* Character) const;
	// 只登记一条有效角色重叠，不广播；实时进入和初始重建共用相同计数、解绑配对规则。
	void RecordOccupantOverlap(ACharacter* Character);
	void UnbindOccupant(ACharacter* Character);
	// 屏蔽“组件数变化但不同玩家数不变”的噪声事件。
	void BroadcastIfPlayerCountChanged(int32 PreviousPlayerCount);

	// 运行期绑定对象，不应被保存进关卡或复制给客户端。
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> BoundTrigger;

	struct FOccupantRecord
	{
		int32 OverlapCount = 0;
		// 记录上一次已通知的资格，控制器事件到来后才能比较“变更前”和“变更后”。
		bool bCountsAsPlayer = false;
	};

	// TMap 按角色定位记录；一个角色的多个碰撞体只增加计数，不能用集合丢掉重叠次数。
	// 计数归零立即移除条目，查询无需重复检查 OverlapCount。
	// 弱引用不保活 Pawn。物理重叠和玩家资格分开，全部由事件维护，不增加逐帧扫描。
	TMap<TWeakObjectPtr<ACharacter>, FOccupantRecord> Occupants;
	bool bRequirePlayerControlledCharacter = true;
};
