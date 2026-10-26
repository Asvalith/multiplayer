// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameState.h"
#include "multiplayerCoopGameState.generated.h"

/**
 * [职责] 合作目标的复制快照，表达当前结果，不记录每次中间事件。
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
	// 战斗团灭是独立终局；倒地且仍可救援时不写失败。
	UPROPERTY() bool bGameLost = false;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerObjectiveProgressEvent,
	int32,
	ActivatedKeys,
	int32,
	RequiredKeys);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FmultiplayerGameWonEvent);

// 关卡战斗快照独立于钥匙提交；晚加入能看到当前波次，不重放旧的刷怪事件。
USTRUCT(BlueprintType)
struct FmultiplayerCombatEncounterState
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly) int32 Wave = 0;
	UPROPERTY(BlueprintReadOnly) int32 TotalWaves = 0;
	UPROPERTY(BlueprintReadOnly) int32 LivingEnemies = 0;
	UPROPERTY(BlueprintReadOnly) bool bActive = false;
};

/**
 * [职责] 向所有客户端复制合作目标、终局和波次的当前状态。
 * GameMode 判定规则并写入快照；客户端根据最新快照刷新，不依赖收到每个中间值。
 * 目标、终局与波次委托都是本地通知，不是 RPC；服务器主动调用 OnRep，客户端由复制触发。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopGameState : public AGameState
{
	GENERATED_BODY()

public:
	// 声明目标与波次字段复制，不复制本地通知委托。
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 对外状态查询与权威写入。
	// 只读暴露当前快照，外部规则不能绕过 ApplyAuthoritativeState 直接修改字段。
	const FmultiplayerCoopObjectiveState& GetObjectiveState() const
	{
		return ObjectiveState;
	}

	// 查询本机最新胜利快照，不在客户端重算终局。
	UFUNCTION(BlueprintPure, Category = "Coop|Objective")
	bool HasGameWon() const { return ObjectiveState.bGameWon; }
	// 查询本机最新团灭快照；倒地与团灭的判定仍由服务器负责。
	UFUNCTION(BlueprintPure, Category = "Coop|Objective") bool HasGameLost() const { return ObjectiveState.bGameLost; }

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
	UPROPERTY(BlueprintAssignable, Category = "Coop|Objective")
	FmultiplayerObjectiveProgressEvent OnObjectiveProgressChanged;

	// 仅本机观察到 false -> true 的胜利状态转换时广播一次；具体 UI 由本地 HUD 负责。
	UPROPERTY(BlueprintAssignable, Category = "Coop|Objective")
	FmultiplayerGameWonEvent OnGameWon;
	UPROPERTY(BlueprintAssignable, Category = "Coop|Objective") FmultiplayerGameWonEvent OnGameLost;
	// 返回当前波次副本，供界面初始化补读而不是依赖历史事件。
	UFUNCTION(BlueprintPure, Category = "Coop|Combat") FmultiplayerCombatEncounterState GetEncounterState() const { return EncounterState; }
	// 仅服务器提交实际发生变化的波次/存活数量，并通知本机与复制客户端。
	void ApplyEncounterState(const FmultiplayerCombatEncounterState& NewState);
	UPROPERTY(BlueprintAssignable, Category = "Coop|Combat") FmultiplayerGameWonEvent OnEncounterChanged;

protected:
	// 两端共用的通知出口；广播前保存快照，避免监听者同步改状态造成重复胜利通知。
	UFUNCTION()
	void OnRep_ObjectiveState();
	// 波次字段到达后仅发布本机刷新事件，界面从当前快照读取完整数据。
	UFUNCTION() void OnRep_EncounterState();

private:
	// GameMode 写、GameState 复制；客户端不能通过本对象提交玩法结果。
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState)
	FmultiplayerCoopObjectiveState ObjectiveState;
	UPROPERTY(ReplicatedUsing = OnRep_EncounterState) FmultiplayerCombatEncounterState EncounterState;

	// 本机通知去重游标，不是第二份权威终局状态；广播前先推进，抵御监听者同步重入。
	bool bLastNotifiedGameWon = false;
	bool bLastNotifiedGameLost = false;
};
