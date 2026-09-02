// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopPlayerController.h"

#include "multiplayerGameInstance.h"
#include "multiplayerGameMode.h"
#include "multiplayerLog.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "multiplayerVictoryWidget.h"

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
		// (*) 进入 PlayingState 后角色才真正可操作，用它确认重连成功比 Travel 回调更稳妥。
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
	bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(VictoryWidget->TakeWidget());
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	SetInputMode(InputMode);
	UE_LOG(LogMultiplayer, Log, TEXT("Victory UI displayed with restart and leave actions."));

	// 蓝图事件是可选的表现扩展，不再承担“是否存在胜利界面”的基础职责。
	ReceiveCoopGameWon();
}

void AmultiplayerCoopPlayerController::RequestRestartCurrentRound()
{
	if (IsLocalController())
	{
		ServerRequestRestartCurrentRound();
	}
}

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

void AmultiplayerCoopPlayerController::LeaveCoopSession()
{
	if (UmultiplayerGameInstance* GameInstance =
		GetGameInstance<UmultiplayerGameInstance>())
	{
		GameInstance->LeaveGame();
	}
}

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
