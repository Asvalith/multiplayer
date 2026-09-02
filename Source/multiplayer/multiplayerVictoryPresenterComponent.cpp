// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "multiplayerCoopPlayerController.h"
#include "multiplayerCoopGameState.h"

UmultiplayerVictoryPresenterComponent::UmultiplayerVictoryPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerVictoryPresenterComponent::RefreshBinding()
{
	// 先解绑旧 GameState，再寻找当前 World 的实例；地图加载后 GameState 指针会整体替换。
	AmultiplayerCoopGameState* PreviousGameState = CoopGameState;
	ClearBinding();

	const AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwner());
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
	{
		return;
	}

	UWorld* World = GetWorld();
	CoopGameState = World != nullptr
		? World->GetGameState<AmultiplayerCoopGameState>()
		: nullptr;
	if (CoopGameState == nullptr)
	{
		return;
	}

	if (CoopGameState != PreviousGameState)
	{
		// 地图切换后是新的比赛状态，允许新一局再次显示胜利界面。
		bVictoryNotified = false;
	}

	// AddUniqueDynamic 防止 BeginPlayingState 或重绑流程重复注册同一对象/函数组合。
	CoopGameState->OnGameWon.AddUniqueDynamic(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameWon);

	// (**) 绑定可能晚于胜利状态复制，必须补查当前值，不能只等下一次事件。
	if (CoopGameState->GetObjectiveState().bGameWon)
	{
		HandleGameWon();
	}
}

void UmultiplayerVictoryPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearBinding();
	bVictoryNotified = false;
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerVictoryPresenterComponent::ClearBinding()
{
	if (IsValid(CoopGameState))
	{
		CoopGameState->OnGameWon.RemoveDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleGameWon);
	}
	CoopGameState = nullptr;
}

void UmultiplayerVictoryPresenterComponent::HandleGameWon()
{
	if (bVictoryNotified)
	{
		return;
	}

	AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwner());
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
	{
		return;
	}

	// 先设置一次性标记再调用蓝图，避免蓝图执行期间的嵌套通知重复弹出表现。
	bVictoryNotified = true;
	PlayerController->ReceiveCoopGameWon();
}
