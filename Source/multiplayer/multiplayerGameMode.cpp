// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameMode.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "multiplayerCharacter.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerCoopPlayerController.h"
#include "multiplayerKeySocket.h"
#include "multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerControllerClass = AmultiplayerCoopPlayerController::StaticClass();

	// 玩法逻辑放在 C++，模型、动画等资源组合放在蓝图，便于美术替换且避免硬编码资源细节。
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != nullptr)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}

void AmultiplayerGameMode::BeginPlay()
{
	Super::BeginPlay();

	// GameMode 只在服务器生成，这里创建第一份完整快照；客户端随后通过 GameState 复制得到同一配置。
	// 初始化也走 ApplyAuthoritativeState，不直接写私有属性，确保范围修正和本地事件路径始终一致。
	if (AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>())
	{
		FmultiplayerCoopObjectiveState InitialState;
		InitialState.RequiredKeys = ResolveRequiredKeys();
		CoopState->ApplyAuthoritativeState(InitialState);
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Coop objective configured: RequiredKeys=%d"),
			InitialState.RequiredKeys);
	}
}

bool AmultiplayerGameMode::RegisterActivatedKey()
{
	// 即使当前函数通常由服务器插槽调用，仍保留 Authority 检查，防止以后新增入口时破坏写权限边界。
	if (!HasAuthority())
	{
		return false;
	}

	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr
		|| CoopState->GetObjectiveState().bGameWon
		|| CoopState->IsObjectiveComplete())
	{
		return false;
	}

	// 复制旧快照、只推进一个字段，再通过唯一写入口提交；不会遗漏 RequiredKeys 或错误重置胜利状态。
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	++NewState.ActivatedKeys;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

bool AmultiplayerGameMode::TryCompleteCoopGame(
	int32 CurrentPlayers,
	int32 RequiredPlayers)
{
	// 区域只提供当前观测人数，最终门槛在 GameMode 再检查；RequiredPlayers 至少按 1 处理。
	if (!HasAuthority() || CurrentPlayers < FMath::Max(1, RequiredPlayers))
	{
		return false;
	}

	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr
		|| CoopState->GetObjectiveState().bGameWon
		|| !CoopState->IsObjectiveComplete())
	{
		return false;
	}

	// bGameWon 只允许从 false 单向推进到 true；重复区域事件会在上面的既有状态检查中返回。
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	NewState.bGameWon = true;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

int32 AmultiplayerGameMode::ResolveRequiredKeys() const
{
	// (**) 优先按关卡实际摆放数量计算，避免配置值与插槽数量不一致导致永远无法胜利。
	int32 PlacedSocketCount = 0;
	for (TActorIterator<AmultiplayerKeySocket> SocketIt(GetWorld()); SocketIt; ++SocketIt)
	{
		++PlacedSocketCount;
	}

	return PlacedSocketCount > 0
		? PlacedSocketCount
		// 没有摆放插槽的测试地图仍可使用配置值初始化目标。
		: FMath::Max(1, RequiredKeys);
}
