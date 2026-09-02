// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopPlayerController.h"

#include "multiplayerGameInstance.h"
#include "multiplayerVictoryPresenterComponent.h"

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
