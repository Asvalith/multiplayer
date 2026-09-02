// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopGameState.h"

#include "Net/UnrealNetwork.h"

void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// UPROPERTY(ReplicatedUsing) 只描述通知方式，DOREPLIFETIME 才真正把属性注册进网络复制列表。
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
	// (**) 在唯一写入口维护状态不变量，客户端永远不会看到负数、超量进度或零目标胜利。
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
		// 相同快照不重复广播和强制网络更新，减少无意义的 UI 回调与网络发送。
		return;
	}

	ObjectiveState = SanitizedState;
	// (*) RepNotify 不会因服务器本地赋值而自动执行，服务器要主动调用共用处理函数。
	HandleObjectiveStateChanged();
	// ForceNetUpdate 只提高进入下一次网络更新的优先级，并不是“此行后所有客户端立刻收到”的同步屏障。
	ForceNetUpdate();
}

void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	HandleObjectiveStateChanged();
}

void AmultiplayerCoopGameState::HandleObjectiveStateChanged()
{
	// 监听者根据完整当前值刷新，不依赖每一个中间快照都被网络逐次送达。
	OnObjectiveProgressChanged.Broadcast(
		ObjectiveState.ActivatedKeys,
		ObjectiveState.RequiredKeys);

	if (ObjectiveState.bGameWon)
	{
		OnGameWon.Broadcast();
	}
}
