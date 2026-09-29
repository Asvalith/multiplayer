// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerCoopPlayerController.h"

#include "Network/multiplayerGameInstance.h"
#include "Core/multiplayerGameMode.h"
#include "Core/multiplayerLog.h"
#include "UI/multiplayerVictoryPresenterComponent.h"
#include "UI/multiplayerVictoryWidget.h"

/*
 * 本地控制器接收胜利结果并管理视口；重开沿 Controller RPC 交给服务器规则层，
 * 退出则交给跨 World 存活的 GameInstance。Widget 不直接承担这些网络职责。
 */
AmultiplayerCoopPlayerController::AmultiplayerCoopPlayerController()
{
	// 使用默认子组件保证服务器和客户端控制器具有一致结构；组件内部会自行筛选 LocalController。
	VictoryPresenter = CreateDefaultSubobject<UmultiplayerVictoryPresenterComponent>(TEXT("VictoryPresenter"));
}

void AmultiplayerCoopPlayerController::BeginPlayingState()
{
	Super::BeginPlayingState();

	// 服务器也为远端玩家创建 PlayerController，但只有所属客户端能确认本地连接已经进入可操作状态。
	if (IsLocalController())
	{
		// 菜单的 UIOnly 修改的是共享视口；换控制器后显式恢复，不能依赖旧菜单析构。
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
		// (*) 用所属控制器进入 PlayingState 作为本项目的连接确认点；独立复制对象仍可能稍后到达。
		if (UmultiplayerGameInstance* GameInstance =
			GetGameInstance<UmultiplayerGameInstance>())
		{
			GameInstance->OnLeaveFailed.RemoveAll(this);
			GameInstance->OnLeaveFailed.AddUObject(
				this, &AmultiplayerCoopPlayerController::HandleLeaveFailed);
			GameInstance->NotifyClientConnected();
		}
	}

	// GameState 可能在 Travel 后被替换，每次进入 PlayingState 都刷新绑定；组件内部负责去重和旧引用清理。
	if (VictoryPresenter != nullptr)
	{
		VictoryPresenter->RefreshBinding();
	}
}

void AmultiplayerCoopPlayerController::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UmultiplayerGameInstance* GameInstance = GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->OnLeaveFailed.RemoveAll(this);
	}
	RemoveVictoryScreen();
	Super::EndPlay(EndPlayReason);
}

/** 创建一次本地胜利界面，并把输入焦点交给可交互按钮；重复通知复用已有展示结果。 */
void AmultiplayerCoopPlayerController::PresentCoopVictory()
{
	if (!IsLocalController() || VictoryWidget != nullptr)
	{
		return;
	}

	VictoryWidget = CreateWidget<UmultiplayerVictoryWidget>(
		this,
		UmultiplayerVictoryWidget::StaticClass());
	if (VictoryWidget == nullptr)
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Victory UI could not be created."));
		return;
	}

	VictoryWidget->AddToViewport(100);
	VictoryWidget->SetActionFeedback(VictoryAction == ECoopVictoryAction::Idle, FText::GetEmpty());
	// 胜利界面接管输入，避免点击按钮的同时继续操纵角色；退出时与 RemoveVictoryScreen 对称恢复。
	bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	if (const TSharedPtr<SWidget> InitialFocus =
		VictoryWidget->GetInitialFocusWidget();
		InitialFocus.IsValid())
	{
		InputMode.SetWidgetToFocus(InitialFocus);
	}
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	UE_LOG(LogMultiplayer, Log, TEXT("Victory UI displayed with restart and leave actions."));

	// 蓝图事件是可选的表现扩展，不再承担“是否存在胜利界面”的基础职责。
	ReceiveCoopGameWon();
}

/** 只接受所属客户端的本地入口，经 Controller RPC 交给 DS 判断是否允许重开。 */
void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}

	// 先占用再发 RPC；只有失败回执能解除等待，避免快速点击重复提交。
	SetVictoryAction(ECoopVictoryAction::RestartPending,
		NSLOCTEXT("Multiplayer", "RestartPending", "正在请求重新开始……"));
	ServerRequestRestartCurrentRound();
}

/** 服务器收到请求后只做转交；可靠投递不能替代 GameMode 的胜利检查和重复请求保护。 */
void AmultiplayerCoopPlayerController::ServerRequestRestartCurrentRound_Implementation()
{
	if (bServerRestartPending)
	{
		return;
	}

	bServerRestartPending = true;
	AmultiplayerGameMode* CoopGameMode =
		GetWorld() != nullptr
			? GetWorld()->GetAuthGameMode<AmultiplayerGameMode>()
			: nullptr;
	if (CoopGameMode == nullptr || !CoopGameMode->RequestRestartCurrentRound(this))
	{
		NotifyRestartFailed(NSLOCTEXT("Multiplayer", "RestartRejected", "暂时无法重开，请重试或退出房间。"));
	}
}

/** 先释放服务器请求，再通知所属客户端，避免通知期间重入旧请求。 */
void AmultiplayerCoopPlayerController::NotifyRestartFailed(const FText& Reason)
{
	if (!HasAuthority() || !bServerRestartPending)
	{
		return;
	}
	bServerRestartPending = false;
	ClientRestartFailed(Reason);
}

/** 可靠 RPC 与单请求限制保证失败先于重试；不把正在退出的界面改回空闲。 */
void AmultiplayerCoopPlayerController::ClientRestartFailed_Implementation(const FText& Reason)
{
	if (VictoryAction == ECoopVictoryAction::RestartPending)
	{
		SetVictoryAction(ECoopVictoryAction::Idle, Reason);
	}
}

/** 将退出意图交给 GameInstance，由它统一取消重连、断开连接并返回菜单。 */
void AmultiplayerCoopPlayerController::LeaveCoopSession()
{
	if (!IsLocalController() || VictoryAction != ECoopVictoryAction::Idle)
	{
		return;
	}
	if (UmultiplayerGameInstance* GameInstance =
		GetGameInstance<UmultiplayerGameInstance>())
	{
		SetVictoryAction(ECoopVictoryAction::Leaving,
			NSLOCTEXT("Multiplayer", "LeavePending", "正在退出房间……"));
		GameInstance->LeaveGame();
	}
}

/** 状态只有一份；默认 Widget 与蓝图调用都走控制器，不各自维护一把永久按钮锁。 */
void AmultiplayerCoopPlayerController::SetVictoryAction(ECoopVictoryAction NewAction, const FText& Message)
{
	VictoryAction = NewAction;
	if (VictoryWidget != nullptr)
	{
		VictoryWidget->SetActionFeedback(NewAction == ECoopVictoryAction::Idle, Message);
	}
}

/** 退出失败只恢复本地操作，不伪造房间已恢复或重连成功。 */
void AmultiplayerCoopPlayerController::HandleLeaveFailed(const FText& Reason)
{
	if (VictoryAction == ECoopVictoryAction::Leaving)
	{
		SetVictoryAction(ECoopVictoryAction::Idle, Reason);
	}
}

/** 优先走项目退出流程；没有项目 GameInstance 时保留引擎默认返回行为作为兜底。 */
void AmultiplayerCoopPlayerController::ClientReturnToMainMenuWithTextReason_Implementation(
	const FText& ReturnReason)
{
	// 服务器主动要求返回菜单时关闭自动重连，避免退出后再次连回。
	if (UmultiplayerGameInstance* GameInstance =
		GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->LeaveGame();
		return;
	}

	Super::ClientReturnToMainMenuWithTextReason_Implementation(ReturnReason);
}

/** RemoveFromParent 解除视口挂接，置空成员释放本类持有的 UObject 引用；实际回收由 UE 管理。 */
void AmultiplayerCoopPlayerController::RemoveVictoryScreen()
{
	if (VictoryWidget != nullptr)
	{
		VictoryWidget->RemoveFromParent();
		VictoryWidget = nullptr;
	}

	if (IsLocalController())
	{
		bShowMouseCursor = false;
		SetInputMode(FInputModeGameOnly());
	}
}
