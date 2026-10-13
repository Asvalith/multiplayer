// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerCoopPlayerController.h"

#include "Core/multiplayerGameMode.h"
#include "Engine/World.h"
#include "Network/multiplayerGameInstance.h"


// 当前 World 中只有本地控制器订阅退出失败通知，服务器上的远端控制器不处理本地界面。
void AmultiplayerCoopPlayerController::BeginPlay()
{
	Super::BeginPlay();
	// 排除服务器为远端玩家创建的控制器，避免替其他玩家更新界面操作状态。
	if (!IsLocalController())
	{
		return;
	}
	//绑定事件
	// OnLeaveFailed 是同进程委托；GameInstance 在退出 Travel 失败时通知此入口。
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.AddUObject(this, &AmultiplayerCoopPlayerController::HandleLeaveFailed);
	}
}

// 离开当前 World 时解除订阅，避免长期存在的 GameInstance 再通知旧控制器。
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
// 本地控制器进入 PlayingState 后确认连接；此时不保证所有机关复制和 UI 初始化已完成。
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
	// 先提交本地操作状态，再广播按钮与提示；监听者同步回调时能读取新状态。
	VictoryAction = NewAction;
	OnVictoryActionChanged.Broadcast(NewAction == ECoopVictoryAction::Idle, Message);
}

//本地占用
// 所属客户端的重开按钮入口：锁定本地操作后向服务器提交请求，不在客户端重载地图。
void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	// 只接受所属客户端且没有在途操作的请求；服务器仍会独立检查是否允许重开。
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}

	// 先占用再发 RPC；只有失败回执能解除等待，避免快速点击重复提交。
	SetVictoryAction(ECoopVictoryAction::RestartPending,NSLOCTEXT("Multiplayer", "RestartPending", "正在请求重新开始……"));
	// 可靠 Server RPC 经此控制器的所属连接发往服务器，不能把本地占位当作重开成功。
	ServerRequestRestartCurrentRound();
}

//接收请求并做权威校验（防重入，状态校验，权威校验，规则校验）
void AmultiplayerCoopPlayerController::ServerRequestRestartCurrentRound_Implementation()
{
	if (bServerRestartPending)
	{
		return;
	}
	// 先占用该玩家的请求；GameMode 另有整局占位，阻止两名玩家同时重载地图。
	bServerRestartPending = true;
	// 服务器取得权威规则入口；控制器只转交请求，不自行决定胜利和重开条件。
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
	// 先解除服务器侧占位，再向所属客户端发送一次可靠失败回执。
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


//本地玩家退出房间
void AmultiplayerCoopPlayerController::LeaveCoopSession()
{
	// 普通按钮退出与重开互斥；服务器要求退出的入口可以打断重开等待。
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}
	// 连接计时器、地址和返回菜单由 GameInstance 管理，控制器不重复维护这份流程。
	UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>();
	if (GameInstance == nullptr)
	{
		return;
	}

	// 先提交按钮占位，再执行退出；GameInstance 会关闭自动重连并安排返回菜单。
	SetVictoryAction(ECoopVictoryAction::Leaving,NSLOCTEXT("Multiplayer", "LeavePending", "正在退出房间……"));
	GameInstance->LeaveGame();
}

//退出失败回调；本地玩家收到失败回执后解除等待，恢复本地界面。
// 此回调来自 GameInstance 的本地委托，不是 Client RPC，也不恢复旧连接或旧 World。
void AmultiplayerCoopPlayerController::HandleLeaveFailed(const FText& Reason)
{
	// 只解除本次退出操作，避免其他状态被迟到的失败通知覆盖。
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

	// 没有项目 GameInstance 时交回引擎默认返回菜单流程。
	Super::ClientReturnToMainMenuWithTextReason_Implementation(ReturnReason);
}
