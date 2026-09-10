// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "multiplayerCoopPlayerController.h"
#include "multiplayerCoopGameState.h"

UmultiplayerVictoryPresenterComponent::UmultiplayerVictoryPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

/**
 * 从“可能尚未到达的 GameState”接上本地界面：先清理旧订阅，再选择等待就绪或立即绑定。
 * 绑定完成后补读当前值，覆盖晚加入或胜利先于监听发生的情况；同一 GameState 重绑保留去重标记。
 */
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
		// BeginPlayingState 与 GameState 到达没有可依赖的先后保证；等待 World 的明确就绪事件，
		// 避免每帧查询或用任意延迟猜测网络时序。
		BindGameStateSetEvent(World);
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

/** 每个等待阶段只订阅一次 World 就绪事件，避免重复刷新时累积回调。 */
void UmultiplayerVictoryPresenterComponent::BindGameStateSetEvent(UWorld* World)
{
	if (World == nullptr || GameStateSetEventHandle.IsValid())
	{
		return;
	}

	GameStateEventWorld = World;
	GameStateSetEventHandle = World->GameStateSetEvent.AddUObject(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameStateSet);
}

/** World 仍有效时移除委托；无论 World 是否已销毁，都清空本地句柄与弱引用。 */
void UmultiplayerVictoryPresenterComponent::ClearGameStateSetEvent()
{
	if (UWorld* BoundWorld = GameStateEventWorld.Get();
		BoundWorld != nullptr && GameStateSetEventHandle.IsValid())
	{
		BoundWorld->GameStateSetEvent.Remove(GameStateSetEventHandle);
	}
	GameStateSetEventHandle.Reset();
	GameStateEventWorld.Reset();
}

void UmultiplayerVictoryPresenterComponent::HandleGameStateSet(
	AGameStateBase* GameState)
{
	// 事件属于已绑定的 World；仍校验实际类型，防止错误 GameMode 的 GameState 被当作合作状态。
	if (Cast<AmultiplayerCoopGameState>(GameState) == nullptr)
	{
		return;
	}

	RefreshBinding();
}

void UmultiplayerVictoryPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearBinding();
	bVictoryNotified = false;
	Super::EndPlay(EndPlayReason);
}

/** 同时收回“等待 GameState 就绪”和“监听胜利”两条订阅，供重绑与 EndPlay 共用。 */
void UmultiplayerVictoryPresenterComponent::ClearBinding()
{
	ClearGameStateSetEvent();
	if (IsValid(CoopGameState))
	{
		CoopGameState->OnGameWon.RemoveDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleGameWon);
	}
	CoopGameState = nullptr;
}

/**
 * 本地一次性转发。标记在调用 UI 前设置，防止外部蓝图同步触发第二次通知。
 * 此标记表示展示请求已发出；如果 Widget 创建失败，本组件没有自动重试策略。
 */
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

	// 先设置一次性标记再进入表现层，避免创建界面或蓝图扩展期间的嵌套通知重复弹出。
	bVictoryNotified = true;
	PlayerController->PresentCoopVictory();
}
