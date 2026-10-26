// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerCoopGameState.h"
#include "Net/UnrealNetwork.h"

// 共享结果层：服务器提交目标/波次快照，客户端通过复制恢复当前结果。
// 本类只存储和发布结果，不判断钥匙资格、出生规则或战斗命中。

// 注册目标与波次快照复制；本地委托本身不经过网络。
void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
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

// [亮点] 服务器写当前快照并立即通知本机；晚加入客户端经复制补读，无需重播拾取 RPC。
// [边界] 此处只规范化并发布结果；钥匙安装、人数和胜负资格仍由 GameMode 判定。
void AmultiplayerCoopGameState::ApplyAuthoritativeState(const FmultiplayerCoopObjectiveState& NewObjectiveState)
{
	if (!HasAuthority())
	{
		return;
	}

	FmultiplayerCoopObjectiveState SanitizedState = NewObjectiveState;
	SanitizedState.RequiredKeys = FMath::Max(0, SanitizedState.RequiredKeys);
	SanitizedState.ActivatedKeys = FMath::Clamp(SanitizedState.ActivatedKeys,0,SanitizedState.RequiredKeys);
	if (SanitizedState.RequiredKeys == 0)
	{
		SanitizedState.bGameWon = false;
	}
	if (SanitizedState.bGameWon) SanitizedState.bGameLost = false;
	if (ObjectiveState.ActivatedKeys == SanitizedState.ActivatedKeys
		&& ObjectiveState.RequiredKeys == SanitizedState.RequiredKeys
		&& ObjectiveState.bGameWon == SanitizedState.bGameWon
		&& ObjectiveState.bGameLost == SanitizedState.bGameLost)
	{
		return;
	}
	ObjectiveState = SanitizedState;
	// 服务器显式调用仅通知本机监听者；远端仍通过属性复制进入各自的 OnRep。
	OnRep_ObjectiveState();
	ForceNetUpdate();
}

// [亮点] 广播前认领本轮终局边沿，监听者同步改状态也不会重复认领同一次胜负。
// [边界] 委托只通知本机，HUD 初始化还须补读快照；不保证收到所有中间网络状态。
void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	const FmultiplayerCoopObjectiveState StateSnapshot = ObjectiveState;
	const bool bBecameGameWon = !bLastNotifiedGameWon && StateSnapshot.bGameWon;
	bLastNotifiedGameWon = StateSnapshot.bGameWon;
	const bool bBecameGameLost = !bLastNotifiedGameLost && StateSnapshot.bGameLost;
	bLastNotifiedGameLost = StateSnapshot.bGameLost;
	OnObjectiveProgressChanged.Broadcast(StateSnapshot.ActivatedKeys,StateSnapshot.RequiredKeys);
	if (bBecameGameWon)
	{
		OnGameWon.Broadcast();
	}
	if (bBecameGameLost) OnGameLost.Broadcast();
}
