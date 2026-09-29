// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerCoopGameState.h"

#include "Core/multiplayerLog.h"
#include "Net/UnrealNetwork.h"

/*
 * 把完整目标快照注册到 UE 属性复制系统。GameState 在服务器和所有客户端都存在，
 * 因而适合发布共享结果；它不接受客户端写入，真正的修改入口仍由服务器 GameMode 调用。
 */
void AmultiplayerCoopGameState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	// UPROPERTY(ReplicatedUsing) 只描述通知方式，DOREPLIFETIME 才真正把属性注册进网络复制列表。
	DOREPLIFETIME(AmultiplayerCoopGameState, ObjectiveState);
}
/*
 * 服务器唯一写入口。提交前统一修正字段关系，相同快照直接忽略；成功提交会立即通知
 * 服务器本地规则监听者，并请求尽快复制给远端客户端。
 * (*) RepNotify 只会因接收复制在客户端触发，服务器本地赋值需要主动走共用通知路径。
 */
void AmultiplayerCoopGameState::ApplyAuthoritativeState(
	const FmultiplayerCoopObjectiveState& NewObjectiveState)
{
	if (!HasAuthority())
	{
		return;
	}

	FmultiplayerCoopObjectiveState SanitizedState = NewObjectiveState;
	// (**) 提交前修正负数和超量进度；没有目标时也不允许提交胜利状态。
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
	HandleObjectiveStateChanged();
	// ForceNetUpdate 请求尽快进行网络更新，并不是“此行后所有客户端立刻收到”的同步屏障。
	ForceNetUpdate();
}

/* 客户端收到服务器快照后的入口，只发布本地观察事件，不反向修改权威状态。 */
void AmultiplayerCoopGameState::OnRep_ObjectiveState()
{
	HandleObjectiveStateChanged();
}

/*
 * 服务器本地写入和客户端 RepNotify 的共同通知出口。先复制当前快照并记录本次胜利转换，
 * 再广播进度，是为了防止监听者在广播过程中同步提交下一份状态。
 * (**) Delegate 可以同步重入；若广播后再读成员，外层调用可能把内层产生的胜利重复广播。
 */
void AmultiplayerCoopGameState::HandleObjectiveStateChanged()
{
	const FmultiplayerCoopObjectiveState StateSnapshot = ObjectiveState;
	const bool bBecameGameWon =
		!bLastNotifiedGameWon && StateSnapshot.bGameWon;
	bLastNotifiedGameWon = StateSnapshot.bGameWon;

	// 监听者根据本次完整快照刷新，不依赖每一个中间快照都被网络逐次送达。
	OnObjectiveProgressChanged.Broadcast(
		StateSnapshot.ActivatedKeys,
		StateSnapshot.RequiredKeys);

	if (bBecameGameWon)
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop objective: victory transition broadcast."));
		OnGameWon.Broadcast();
	}
}
