// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameState.h"
#include "multiplayerCoopGameState.generated.h"

/**
 * 一次网络更新中发送的合作目标快照。
 *
 * 将“当前进度、目标上限、是否胜利”归在一个业务结构中，并使用同一 RepNotify 刷新表现，
 * 避免业务层依赖多个 OnRep 的调用顺序；这不等于跨 Actor、RPC 或附件复制的原子事务。
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
 * GameMode 判定规则并写入快照；客户端根据最新快照刷新，不依赖收到每个中间值。
 * 两个委托都是本地通知，不是 RPC；服务器主动调用 OnRep，客户端由复制触发。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopGameState : public AGameState
{
	GENERATED_BODY()

public:
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 对外状态查询与权威写入。
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

	/** 服务器唯一写入口：修正范围，忽略相同快照，提交后通知本地监听者并请求复制。 */
	void ApplyAuthoritativeState(
		const FmultiplayerCoopObjectiveState& NewObjectiveState);

	// 对外本地事件。
	// 本机进度刷新事件：服务器写入和客户端 OnRep 都会触发，监听者无需区分数据来源。
	FmultiplayerObjectiveProgressEvent OnObjectiveProgressChanged;

	// 仅本机观察到 false -> true 的胜利状态转换时广播一次；具体 UI 由本地 HUD 负责。
	FmultiplayerGameWonEvent OnGameWon;

protected:
	// 两端共用的通知出口；广播前保存快照，避免监听者同步改状态造成重复胜利通知。
	UFUNCTION()
	void OnRep_ObjectiveState();

private:
	// GameMode 写、GameState 复制；客户端不能通过本对象提交玩法结果。
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState)
	FmultiplayerCoopObjectiveState ObjectiveState;

	// 记录本机上一次已经进入通知流程的胜利状态，用于只发布 false -> true 转换。
	bool bLastNotifiedGameWon = false;
};
