// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerCoopGameState.h"
#include "Net/UnrealNetwork.h"

/// <summary>
/// 保存目标状态，并在变化时通知别人
/// </summary>


//注册要同步的状态
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

void AmultiplayerCoopGameState::OnRep_EncounterState() { OnEncounterChanged.Broadcast(); }

//服务器更新状态
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
	//广播
	OnRep_ObjectiveState();
	//强制网络更新同步
	ForceNetUpdate();
}

//状态变化时通知别人
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
