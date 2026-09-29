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
 * 多种合作机关共用的服务器区域人数统计组件。
 *
 * (*) 组件只解决“区域里有几个不同玩家”，具体激活规则和网络表现仍由所属机关负责，
 * 避免每个机关重复实现容易出错的 Overlap 代码。
 *
 * (**) 一个 Character 通常有胶囊体、网格体等多个碰撞组件，BeginOverlap 可能触发多次。
 * 因此按角色记录仍在区域内的重叠项计数（包含组件及其刚体）；不能只用 TSet，否则一个 EndOverlap
 * 都可能把仍在区域内的玩家提前移除。
 * (**) 玩家断线、Pawn 被替换或关卡卸载时不保证收到成对的 EndOverlap，因此首次进入时还要
 * 绑定 Character::OnDestroyed，并在 UnbindTrigger/EndPlay 中对称解绑所有外部 Delegate。
 *
 * 该组件不复制人数。规则只在服务器运行，门、平台和 GameState 最终复制各自真正需要的结果；
 * 客户端没必要重复维护一份可能与服务器不一致的触发区成员表。
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerPlayerOccupancyComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerPlayerOccupancyComponent();

	/**
	 * 绑定一个用于规则判定的触发体。
	 *
	 * 重复绑定会先清理旧触发体，再从新触发体的当前重叠组件重建计数；
	 * 重绑定前后最多广播一次最终人数。非服务器端直接关闭该触发体碰撞，避免两端各算一份。
	 * @param bInRequirePlayerControlledCharacter 为 true 时排除 AI 和非玩家控制的 Character。
	 */
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
	void UnbindOccupant(ACharacter* Character);
	// 增加该角色的重叠组件计数，首次进入时监听销毁和控制器变化。
	void AddOccupant(AActor* OtherActor);
	// 只登记一条有效角色重叠，不广播；实时进入和初始重建共用相同计数、解绑配对规则。
	void RecordOccupantOverlap(ACharacter* Character);
	// 减少重叠组件计数，最后一个组件离开时才移除角色。
	void RemoveOccupant(AActor* OtherActor);
	// 屏蔽“组件数变化但不同玩家数不变”的噪声事件。
	void BroadcastIfPlayerCountChanged(int32 PreviousPlayerCount);
	// 从触发体已经维护的组件重叠表恢复计数，避免绑定前已站在区域内的玩家被漏掉。
	void RebuildOccupantsFromCurrentOverlaps();
	// 只做底层解绑与清表，不广播；BindTrigger 用它把重绑定合并成一次状态变化。
	void UnbindTriggerInternal();
	// 移除角色的销毁和控制器回调后清表，不能只 Reset 容器而遗留外部 Delegate。
	void ClearOccupants();

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
	// 弱引用不保活 Pawn。物理重叠和玩家资格分开，全部由事件维护，不增加逐帧扫描。
	TMap<TWeakObjectPtr<ACharacter>, FOccupantRecord> Occupants;
	bool bRequirePlayerControlledCharacter = true;
};
