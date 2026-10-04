// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerCoopPlayerController.h"

#include "Core/multiplayerGameMode.h"
#include "Engine/World.h"
#include "Network/multiplayerGameInstance.h"


void AmultiplayerCoopPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController())
	{
		return;
	}
	//绑定事件
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.AddUObject(this, &AmultiplayerCoopPlayerController::HandleLeaveFailed);
	}
}

void AmultiplayerCoopPlayerController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 解绑事件
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.RemoveAll(this);
	}
	Super::EndPlay(EndPlayReason);
}

//进入可游玩状态
void AmultiplayerCoopPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();
	//服务器也为远端玩家创建 PlayerController，但只有所属客户端能确认本地连接已经进入可操作状态。
	if (!IsLocalController())
	{
		return;
	}
	//连接成功后通知 GameInstance，避免客户端在连接后立即断开
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->NotifyClientConnected();
	}
}

//VictoryAction状态修改
void AmultiplayerCoopPlayerController::SetVictoryAction(ECoopVictoryAction NewAction, const FText& Message)
{
	VictoryAction = NewAction;
	OnVictoryActionChanged.Broadcast(NewAction == ECoopVictoryAction::Idle, Message);
}

/// <summary>
/// 重新开始当前回合的本地入口；仅本地玩家可发起重开，且不允许在重开或退出中途重复提交。
/// </summary>
//本地占用
void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}

	// 先占用再发 RPC；只有失败回执能解除等待，避免快速点击重复提交。
	SetVictoryAction(ECoopVictoryAction::RestartPending,NSLOCTEXT("Multiplayer", "RestartPending", "正在请求重新开始……"));
	ServerRequestRestartCurrentRound();
}

//服务端：接收请求并做权威校验（防重入，状态校验，权威校验，规则校验）
void AmultiplayerCoopPlayerController::ServerRequestRestartCurrentRound_Implementation()
{
	if (bServerRestartPending)
	{
		return;
	}
	bServerRestartPending = true;
	UWorld* World = GetWorld();
	AmultiplayerGameMode* CoopGameMode =World != nullptr ? World->GetAuthGameMode<AmultiplayerGameMode>() : nullptr;
	if (CoopGameMode == nullptr || !CoopGameMode->RequestRestartCurrentRound(this))
	{
		NotifyRestartFailed(NSLOCTEXT("Multiplayer", "RestartRejected", "暂时无法重开，请重试或退出房间。"));
	}
}

//服务端：统一失败通知入口
void AmultiplayerCoopPlayerController::NotifyRestartFailed(const FText& Reason)
{
	// GameMode 可能在返回 false 前已同步通知失败；共用此出口避免重复回执。
	if (!HasAuthority() || !bServerRestartPending)
	{
		return;
	}
	bServerRestartPending = false;
	ClientRestartFailed(Reason);
}

//客户端：收到失败回执后解除等待，恢复本地界面。
void AmultiplayerCoopPlayerController::ClientRestartFailed_Implementation(const FText& Reason)
{
	// 只恢复对应操作；迟到的重开回执不能解锁正在退出的界面。
	if (VictoryAction == ECoopVictoryAction::RestartPending)
	{
		SetVictoryAction(ECoopVictoryAction::Idle, Reason);
	}
}

/// <summary>
/// 退出会话的本地入口；仅本地玩家可发起退出，且不允许在重开或退出中途重复提交。
/// </summary>
//本地玩家退出房间
void AmultiplayerCoopPlayerController::LeaveCoopSession()
{
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}
	UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>();
	if (GameInstance == nullptr)
	{
		return;
	}

	SetVictoryAction(ECoopVictoryAction::Leaving,NSLOCTEXT("Multiplayer", "LeavePending", "正在退出房间……"));
	GameInstance->LeaveGame();
}

//退出失败回调；本地玩家收到失败回执后解除等待，恢复本地界面。
void AmultiplayerCoopPlayerController::HandleLeaveFailed(const FText& Reason)
{
	if (VictoryAction == ECoopVictoryAction::Leaving)
	{
		SetVictoryAction(ECoopVictoryAction::Idle, Reason);
	}
}

//服务器要求客户端返回菜单时，关闭自动重连，避免退出后再次连回。
void AmultiplayerCoopPlayerController::ClientReturnToMainMenuWithTextReason_Implementation(
	const FText& ReturnReason)
{
	// 服务器主动要求返回菜单时关闭自动重连，避免退出后再次连回。
	if (UmultiplayerGameInstance* GameInstance =GetGameInstance<UmultiplayerGameInstance>())
	{
		// 服务端要求退出可打断重开等待；否则退出失败后会遗留 RestartPending 锁。
		SetVictoryAction(ECoopVictoryAction::Leaving,
			NSLOCTEXT("Multiplayer", "LeavePending", "正在退出房间……"));
		GameInstance->LeaveGame();
		return;
	}

	Super::ClientReturnToMainMenuWithTextReason_Implementation(ReturnReason);
}
