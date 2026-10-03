// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerCoopGameState.h"

#include "Core/multiplayerLog.h"
#include "Net/UnrealNetwork.h"

void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCoopGameState, ObjectiveState);
}

void AmultiplayerCoopGameState::ApplyAuthoritativeState(
	const FmultiplayerCoopObjectiveState& NewObjectiveState)
{
	if (!HasAuthority())
	{
		return;
	}

	FmultiplayerCoopObjectiveState SanitizedState = NewObjectiveState;
	// 没有目标时不允许胜利，进度限制在目标范围内。
	SanitizedState.RequiredKeys = FMath::Max(0, SanitizedState.RequiredKeys);
	SanitizedState.ActivatedKeys = FMath::Clamp(
		SanitizedState.ActivatedKeys,
		0,
		SanitizedState.RequiredKeys);
	if (SanitizedState.RequiredKeys == 0)
	{
		SanitizedState.bGameWon = false;
	}

	if (ObjectiveState.ActivatedKeys == SanitizedState.ActivatedKeys
		&& ObjectiveState.RequiredKeys == SanitizedState.RequiredKeys
		&& ObjectiveState.bGameWon == SanitizedState.bGameWon)
	{
		return;
	}

	ObjectiveState = SanitizedState;
	OnRep_ObjectiveState();
	ForceNetUpdate();
}

/*
 * 进度监听者可能同步触发判胜。先保存本次快照和胜利转换，避免外层重复广播内层的胜利。
 */
void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	const FmultiplayerCoopObjectiveState StateSnapshot = ObjectiveState;
	const bool bBecameGameWon =
		!bLastNotifiedGameWon && StateSnapshot.bGameWon;
	bLastNotifiedGameWon = StateSnapshot.bGameWon;

	OnObjectiveProgressChanged.Broadcast(
		StateSnapshot.ActivatedKeys,
		StateSnapshot.RequiredKeys);

	if (bBecameGameWon)
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop objective: victory transition broadcast."));
		OnGameWon.Broadcast();
	}
}
