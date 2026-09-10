// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopPlayerController.h"

#include "multiplayerGameInstance.h"
#include "multiplayerGameMode.h"
#include "multiplayerLog.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "multiplayerVictoryWidget.h"

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
		// (*) 用所属控制器进入 PlayingState 作为本项目的连接确认点；独立复制对象仍可能稍后到达。
		if (UmultiplayerGameInstance* GameInstance =
			GetGameInstance<UmultiplayerGameInstance>())
		{
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

/** 只接受本地玩家入口；监听服务器的本地主机也沿同一条 RPC/规则路径发起重开。 */
void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	if (IsLocalController())
	{
		ServerRequestRestartCurrentRound();
	}
}

/** 服务器收到请求后只做转交；可靠投递不能替代 GameMode 的胜利检查和重复请求保护。 */
void AmultiplayerCoopPlayerController::ServerRequestRestartCurrentRound_Implementation()
{
	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld() != nullptr
			? GetWorld()->GetAuthGameMode<AmultiplayerGameMode>()
			: nullptr)
	{
		CoopGameMode->RequestRestartCurrentRound(this);
	}
}

/** 将用户退出意图交给 GameInstance，由它统一取消重连、清理会话并返回默认菜单。 */
void AmultiplayerCoopPlayerController::LeaveCoopSession()
{
	if (UmultiplayerGameInstance* GameInstance =
		GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->LeaveGame();
	}
}

/** 优先走项目退出流程；没有项目 GameInstance 时保留引擎默认返回行为作为兜底。 */
void AmultiplayerCoopPlayerController::ClientReturnToMainMenuWithTextReason_Implementation(
	const FText& ReturnReason)
{
	// 主机退出时，远端也走主动退出入口。先销毁本机会话并关闭重连，再返回菜单。
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
