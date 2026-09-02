// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerPlayerOccupancyComponent.generated.h"

class ACharacter;
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
 * 因此必须按角色记录重叠组件计数；不能只用 TSet，否则任意一个组件 EndOverlap
 * 都可能把仍在区域内的玩家提前移除。
 * (**) 玩家断线、Pawn 被替换或关卡卸载时不保证收到成对的 EndOverlap，因此首次进入时还要
 * 绑定 Character::OnDestroyed，并在 UnbindTrigger/EndPlay 中对称解绑所有外部 Delegate。
 *
 * 该组件不复制人数。规则只在服务器运行，门、平台和胜利区域最终复制各自真正需要的结果；
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
	 * 重复绑定会先清理旧触发体和占用记录；非服务器端直接关闭该触发体碰撞，避免两端各算一份。
	 * @param bInRequirePlayerControlledCharacter 为 true 时排除 AI 和非玩家控制的 Character。
	 */
	void BindTrigger(
		UPrimitiveComponent* InTrigger,
		bool bInRequirePlayerControlledCharacter = true);

	// 对称移除 Overlap/OnDestroyed Delegate，并清空所有临时计数；可安全重复调用。
	void UnbindTrigger();

	// 返回有效弱引用的数量，不把同一角色的多个碰撞组件重复算作多个玩家。
	int32 GetPlayerCount() const;
	// 输出当前仍有效的不同角色，供合作门验证“不同玩家数”而非仅验证压力板数量。
	void GetOccupyingCharacters(TArray<ACharacter*>& OutCharacters) const;

	// 只在不同玩家总数发生变化时广播；同一玩家的第二个碰撞组件进入不会产生无效通知。
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

	// 统一执行类型、有效性和可选的玩家控制条件过滤。
	ACharacter* GetValidOccupant(AActor* OtherActor) const;
	// 增加该角色的重叠组件计数，首次进入时绑定 OnDestroyed。
	void AddOccupant(AActor* OtherActor);
	// 减少重叠组件计数，最后一个组件离开时才移除角色。
	void RemoveOccupant(AActor* OtherActor);
	// 屏蔽“组件数变化但不同玩家数不变”的噪声事件。
	void BroadcastIfPlayerCountChanged(int32 PreviousPlayerCount);
	// 移除所有角色销毁回调后清空表，不能只 Reset 容器而遗留外部 Delegate。
	void ClearOccupants();

	// 运行期绑定对象，不应被保存进关卡或复制给客户端。
	UPROPERTY(Transient)
	TObjectPtr<UPrimitiveComponent> BoundTrigger;

	// Key 是不延长角色生命周期的弱引用，Value 是该角色当前仍在区域内的碰撞组件数量。
	// 弱引用本身不会主动删除 Map 条目，所以 OnDestroyed 和 GetPlayerCount 的有效性检查都不可省略。
	TMap<TWeakObjectPtr<ACharacter>, int32> OverlapCounts;
	bool bRequirePlayerControlledCharacter = true;
};
