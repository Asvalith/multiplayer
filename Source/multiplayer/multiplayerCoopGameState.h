// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "multiplayerCoopGameState.generated.h"

/**
 * 一次网络更新中发送的合作目标快照。
 *
 * 将“当前进度、目标上限、是否胜利”放在同一结构体内复制，客户端收到的是同一时刻的完整组合，
 * 不会因为三个独立属性先后到达而短暂显示出互相矛盾的状态。
 */
USTRUCT()
struct FmultiplayerCoopObjectiveState
{
	GENERATED_BODY()

	// 已经被服务器接受的插槽数量，始终限制在 [0, RequiredKeys]。
	UPROPERTY()
	int32 ActivatedKeys = 0;

	// 当前关卡需要完成的插槽总数；为 0 时目标不成立，也不允许进入胜利状态。
	UPROPERTY()
	int32 RequiredKeys = 0;

	// 最终胜利结果。它只能由服务器 GameMode 在目标和人数均满足时写入。
	UPROPERTY()
	bool bGameWon = false;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerObjectiveProgressEvent,
	int32,
	ActivatedKeys,
	int32,
	RequiredKeys);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerGameWonEvent);

/**
 * 向所有客户端复制的合作目标状态。
 * (*) GameMode 只存在于服务器，GameState 会复制给客户端，因此规则由 GameMode 判定，
 * 共享进度由 GameState 发布。
 * (*) 将进度和胜利放进同一个快照复制，可避免多个属性分批到达时出现短暂的矛盾状态。
 * (**) 属性复制保证客户端最终得到服务器的最新状态，但不承诺每一个中间值都被逐次观察到；
 * 因此界面和机关表现应根据“当前快照”刷新，不能依赖收到过所有历史变化。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	// 注册 ObjectiveState 的复制规则；仅声明 ReplicatedUsing 并不会自动进入复制列表。
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 只读暴露当前快照，外部规则不能绕过 ApplyAuthoritativeState 直接修改字段。
	const FmultiplayerCoopObjectiveState& GetObjectiveState() const
	{
		return ObjectiveState;
	}

	// RequiredKeys 必须大于 0，避免空关卡被误判为“0/0 已完成”。
	bool IsObjectiveComplete() const
	{
		return ObjectiveState.RequiredKeys > 0
			&& ObjectiveState.ActivatedKeys >= ObjectiveState.RequiredKeys;
	}

	/**
	 * 权威状态的唯一写入口。
	 *
	 * 写入前统一修正范围并拒绝完全相同的快照，防止无效广播、无意义的 UI 刷新和额外网络更新。
	 * (**) RepNotify 会在客户端收到复制时执行，但服务器修改属性后不会自动执行，
	 * 所以服务器需要主动走同一套通知逻辑，保证 Listen Server 主机和远端客户端表现一致。
	 */
	void ApplyAuthoritativeState(
		const FmultiplayerCoopObjectiveState& NewObjectiveState);

	// 本机进度刷新事件：服务器写入和客户端 OnRep 都会触发，监听者无需区分数据来源。
	FmultiplayerObjectiveProgressEvent OnObjectiveProgressChanged;

	// 当前快照为胜利时广播；具体 UI 仍由本地 PlayerController 的表现组件负责。
	FmultiplayerGameWonEvent OnGameWon;

protected:
	// 客户端收到 ObjectiveState 后的复制通知，转入两端共用的 HandleObjectiveStateChanged。
	UFUNCTION()
	void OnRep_ObjectiveState();

private:
	// 统一发布进度和胜利事件，避免服务器路径与客户端路径各写一套表现通知。
	void HandleObjectiveStateChanged();

	// GameMode 写、GameState 复制；客户端不能通过本对象提交玩法结果。
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState)
	FmultiplayerCoopObjectiveState ObjectiveState;
};
