// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerCoopGameState.h"
#include "Net/UnrealNetwork.h"

// 共享结果层：服务器提交目标/波次快照，客户端通过复制恢复当前结果。
// 本类只存储和发布结果，不判断钥匙资格、出生规则或战斗命中。

// 注册目标与波次快照复制；本地委托本身不经过网络。
void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	//属性同步注册
	DOREPLIFETIME(AmultiplayerCoopGameState, ObjectiveState);
	DOREPLIFETIME(AmultiplayerCoopGameState, EncounterState);
}

// 只发布实际变化，不因计时器或角色状态重复通知增加网络和HUD工作。
void AmultiplayerCoopGameState::ApplyEncounterState(const FmultiplayerCombatEncounterState& NewState)
{
	if (!HasAuthority()) return;
	FmultiplayerCombatEncounterState Next = NewState;
	Next.TotalWaves = FMath::Clamp(Next.TotalWaves, 0, 10);
	Next.Wave = FMath::Clamp(Next.Wave, 0, Next.TotalWaves);
	Next.LivingEnemies = FMath::Max(0, Next.LivingEnemies);
	if (EncounterState.Wave == Next.Wave && EncounterState.TotalWaves == Next.TotalWaves
		&& EncounterState.LivingEnemies == Next.LivingEnemies && EncounterState.bActive == Next.bActive) return;
	EncounterState = Next;
	OnRep_EncounterState();
	ForceNetUpdate();
}

// 各端通知本机 HUD 补读最新波次；晚加入不重放之前的刷怪事件。
void AmultiplayerCoopGameState::OnRep_EncounterState() { OnEncounterChanged.Broadcast(); }

// 发布当前快照而非历史增量：晚加入能直接读取已完成的目标，不依赖重播拾取 RPC。
void AmultiplayerCoopGameState::ApplyAuthoritativeState(const FmultiplayerCoopObjectiveState& NewObjectiveState)
{
	if (!HasAuthority())
	{
		return;
	}

	//净化输入
	FmultiplayerCoopObjectiveState SanitizedState = NewObjectiveState;
	SanitizedState.RequiredKeys = FMath::Max(0, SanitizedState.RequiredKeys);
	SanitizedState.ActivatedKeys = FMath::Clamp(SanitizedState.ActivatedKeys,0,SanitizedState.RequiredKeys);
	if (SanitizedState.RequiredKeys == 0)
	{
		SanitizedState.bGameWon = false;
	}
	if (SanitizedState.bGameWon) SanitizedState.bGameLost = false;
	//跳过无意义的更新
	if (ObjectiveState.ActivatedKeys == SanitizedState.ActivatedKeys
		&& ObjectiveState.RequiredKeys == SanitizedState.RequiredKeys
		&& ObjectiveState.bGameWon == SanitizedState.bGameWon
		&& ObjectiveState.bGameLost == SanitizedState.bGameLost)
	{
		return;
	}
	//更新
	ObjectiveState = SanitizedState;
	// 服务器显式调用仅通知本机监听者；远端仍通过属性复制进入各自的 OnRep。
	OnRep_ObjectiveState();
	//强制网络更新同步
	ForceNetUpdate();
}

// 复制到达后通知本机订阅者；C++ 委托不跨网络，HUD 应重读快照而非自行累加进度。
void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	//保存快照
	const FmultiplayerCoopObjectiveState StateSnapshot = ObjectiveState;
	//计算胜利（边沿触发）
	const bool bBecameGameWon = !bLastNotifiedGameWon && StateSnapshot.bGameWon;
	bLastNotifiedGameWon = StateSnapshot.bGameWon;
	const bool bBecameGameLost = !bLastNotifiedGameLost && StateSnapshot.bGameLost;
	bLastNotifiedGameLost = StateSnapshot.bGameLost;
	//广播进度变化
	OnObjectiveProgressChanged.Broadcast(StateSnapshot.ActivatedKeys,StateSnapshot.RequiredKeys);
	//广播胜利事件
	if (bBecameGameWon)
	{
		OnGameWon.Broadcast();
	}
	if (bBecameGameLost) OnGameLost.Broadcast();
}
