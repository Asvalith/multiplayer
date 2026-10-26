// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerCoopPlayerController.h"

#include "Core/multiplayerGameMode.h"
#include "Engine/World.h"
#include "Network/multiplayerGameInstance.h"
#include "TimerManager.h"
#include "InputCoreTypes.h"
#include "Components/InputComponent.h"

// [职责] 所属连接的请求层：握手、重开与退出经本控制器建立端点，不在控制器内重写玩法规则。
// 服务器只通过 Owned RPC 回复该玩家；界面反馈使用本地委托，其他客户端不参与请求锁。

// Pawn 死亡后只关闭自身输入；Controller 保留终局重开入口，服务器仍复核是否真的结束。
void AmultiplayerCoopPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &ThisClass::RequestRestartCurrentRound);
}


// 当前 World 中只有本地控制器订阅退出失败通知，服务器上的远端控制器不处理本地界面。
void AmultiplayerCoopPlayerController::BeginPlay()
{
	Super::BeginPlay();
	// 排除服务器为远端玩家创建的控制器，避免替其他玩家更新界面操作状态。
	if (!IsLocalController())
	{
		return;
	}
	// OnLeaveFailed 是本实例的本地委托；GameInstance 在退出 Travel 失败时通知此入口。
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.AddUObject(this, &AmultiplayerCoopPlayerController::HandleLeaveFailed);
	}
}

// 离开当前 World 时解除订阅，避免长期存在的 GameInstance 再通知旧控制器。
void AmultiplayerCoopPlayerController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(CombatHandshakeTimer);
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.RemoveAll(this);
	}
	Super::EndPlay(EndPlayReason);
}

// 本地控制器进入 PlayingState 后准备会话握手；接纳回执前不把连接视为业务就绪。
// [亮点] GI 保留本机连接意图，Owned Controller 提供请求端点，GameMode 决定是否开放角色权限。
void AmultiplayerCoopPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();
	// 服务器也持有远端玩家的 Controller；只有本地控制器发起握手，接纳前仍不能操作角色。
	if (!IsLocalController())
	{
		return;
	}
	// PlayingState 现在只发起握手；必须等服务器恢复完成的可靠回执才能确认连接成功。
	if (bCombatHandshakeSubmitted) return;
	if (HasAuthority()) { ServerEstablishCombatSession(FGuid(), 0); return; }
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		FGuid Ticket;
		if (GameInstance->PrepareCombatHandshake(this, Ticket, ClientHandshakeOperation))
		{
			bCombatHandshakeSubmitted = true;
			ServerEstablishCombatSession(Ticket, ClientHandshakeOperation);
		}
	}
}

// [职责] 所属客户端 -> 服务器：只提交票据与本机操作编号，GameMode 裁决接纳／恢复。
// [边界] 不接收位置、血量、资源或冷却，也不接受在已接纳会话上重复换号。
void AmultiplayerCoopPlayerController::ServerEstablishCombatSession_Implementation(FGuid Ticket, uint64 Operation)
{
	if (bCombatHandshakeSubmitted) return;
	bCombatHandshakeSubmitted = true;
	GetWorldTimerManager().ClearTimer(CombatHandshakeTimer);
	FGuid IssuedTicket;
	FText Error;
	bool bRetryable = false;
	AmultiplayerGameMode* Mode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>();
	const bool bAccepted = Mode && Mode->ClaimCombatSession(this, Ticket, IssuedTicket, Error, bRetryable);
	if (!bAccepted && Error.IsEmpty()) Error = NSLOCTEXT("Multiplayer", "CombatSessionUnavailable", "服务器暂时无法接纳玩家，请重新连接。");
	ClientCombatSessionResult(IssuedTicket, Operation, Error, bRetryable);
	if (!bAccepted) SetLifeSpan(1.0f); // 留出可靠失败回执的发送机会；未接纳 Pawn 始终不能移动或施法。
}

// 所属客户端验证回执仍属于当前握手，成功后才开放本地连接状态，迟到回执不能接管新尝试。
// [边界] 接纳回执和 Pawn/属性复制独立到达；可靠 RPC 不是所有 Actor、资源与 HUD 已就绪的屏障。
void AmultiplayerCoopPlayerController::ClientCombatSessionResult_Implementation(FGuid Ticket, uint64 Operation, const FText& Error, bool bRetryable)
{
	if (!IsLocalController() || HasAuthority()) return;
	if (UmultiplayerGameInstance* Instance = GetGameInstance<UmultiplayerGameInstance>())
	{
		if (Instance->CompleteCombatHandshake(this, Operation ? Operation : ClientHandshakeOperation, Ticket, Error, bRetryable))
		{
			bCombatSessionAccepted = true;
			Instance->NotifyClientConnected();
		}
	}
}

// 服务器切图或恢复失败回合时更新本机票据，票据不向其他玩家广播。
void AmultiplayerCoopPlayerController::ClientSetReconnectTicket_Implementation(FGuid Ticket)
{
	if (UmultiplayerGameInstance* Instance = GetGameInstance<UmultiplayerGameInstance>()) Instance->SetReconnectTicket(this, Ticket);
}

// 基类 Destroyed 会先销毁或解除 Pawn，再通知 Logout；这里必须在调用 Super 之前保存。
void AmultiplayerCoopPlayerController::Destroyed()
{
	if (AmultiplayerGameMode* Mode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>()) Mode->SaveCombatSession(this);
	Super::Destroyed();
}

// 正常离线销毁 Pawn 之前补存快照，与 Destroyed/Logout 共用服务器去重保存入口。
void AmultiplayerCoopPlayerController::PawnLeavingGame()
{
	if (AmultiplayerGameMode* Mode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>()) Mode->SaveCombatSession(this);
	Super::PawnLeavingGame();
}

// 本地操作状态先提交再广播，监听者读取同一份请求占用。
void AmultiplayerCoopPlayerController::SetVictoryAction(ECoopVictoryAction NewAction, const FText& Message)
{
	// 先提交本地操作状态，再广播按钮与提示；监听者同步回调时能读取新状态。
	VictoryAction = NewAction;
	OnVictoryActionChanged.Broadcast(NewAction == ECoopVictoryAction::Idle, Message);
}

// 所属客户端的重开按钮入口：锁定本地操作后向服务器提交请求，不在客户端重载地图。
void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	// 只接受所属客户端且没有在途操作的请求；服务器仍会独立检查是否允许重开。
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}
	// 未接纳时不建立在途请求；进入 PlayingState 不代表服务器已开放本局资格。
	if (!bCombatSessionAccepted)
	{
		SetVictoryAction(ECoopVictoryAction::Idle,
			NSLOCTEXT("Multiplayer", "RestartBeforeAdmission", "会话尚未就绪，请稍候再重开。"));
		return;
	}

	// 先占用再发 RPC；只有失败回执能解除等待，避免快速点击重复提交。
	SetVictoryAction(ECoopVictoryAction::RestartPending,NSLOCTEXT("Multiplayer", "RestartPending", "正在请求重新开始……"));
	// 可靠 Server RPC 经此控制器的所属连接发往服务器，不能把本地占位当作重开成功。
	if (VictoryAction == ECoopVictoryAction::RestartPending) ServerRequestRestartCurrentRound();
}

// [职责] 服务器为本玩家占用请求，再交 GameMode 检查整局资格；本地按钮锁不能授权重开。
void AmultiplayerCoopPlayerController::ServerRequestRestartCurrentRound_Implementation()
{
	// 重复的在途请求不另发失败：否则会把真正等待 Travel 的本地操作错误解锁。
	if (bServerRestartPending)
	{
		return;
	}
	if (!bCombatSessionAccepted)
	{
		// 首个未接纳请求仍须有终结回执，不能让旧客户端或直接调用永久等待。
		ClientRestartFailed(NSLOCTEXT("Multiplayer", "RestartBeforeAdmission", "会话尚未就绪，请稍候再重开。"));
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

// 服务器要求客户端返回菜单时，交 GameInstance 关闭自动重连并处理本机退出。
void AmultiplayerCoopPlayerController::ClientReturnToMainMenuWithTextReason_Implementation(
	const FText& ReturnReason)
{
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
