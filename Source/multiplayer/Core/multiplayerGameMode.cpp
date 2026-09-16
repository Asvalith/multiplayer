// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerGameMode.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "Core/multiplayerCoopGameState.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Core/multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"

/*
 * GameMode 只在服务器存在，这里指定共享 GameState、玩家控制器和默认角色类型。
 * C++ 负责规则，角色模型和动画由派生蓝图组合；这里仍通过固定资源路径指定默认角色蓝图。
 */
AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerControllerClass = AmultiplayerCoopPlayerController::StaticClass();

	// 只在这里选定角色蓝图，不在规则函数中查找网格、动画等具体资源。
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != nullptr)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}

/*
 * 服务器开局时根据关卡实际插槽生成第一份目标快照。初始化也走 GameState 的唯一写入口，
 * 使范围修正、Listen Server 通知和后续复制保持同一条路径。
 */
void AmultiplayerGameMode::BeginPlay()
{
	Super::BeginPlay();

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

/*
 * 插槽完成自身校验后调用的服务器登记入口；检查胜利状态和目标上限后，只推进一格进度。
 * (**) 本函数不接收插槽身份，不能自行识别同一插槽的重复登记；去重依赖插槽先设置 bActivated。
 */
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

/*
 * 胜利区域提交当前人数后，由服务器集中复核人数、钥匙目标和既有胜利状态。
 * 成功只把 GameState 从未胜利推进到胜利一次；UI、音效等表现不在 GameMode 中直接执行。
 */
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

/*
 * 接受玩家控制器发来的重开请求。调用者必须属于当前 World，且本局已经胜利；
 * 成功后先设置一次性标记，再让 AGameMode 重新加载当前 URL，防止两名玩家重复发起 Travel。
 */
bool AmultiplayerGameMode::RequestRestartCurrentRound(
	AController* RequestingController)
{
	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (!HasAuthority()
		|| RequestingController == nullptr
		|| RequestingController->GetWorld() != GetWorld()
		|| CoopState == nullptr
		|| !CoopState->GetObjectiveState().bGameWon
		|| bRestartRequested)
	{
		return false;
	}

	// 先锁定请求再触发 Travel，阻止两名玩家同帧点击导致重复重载。
	bRestartRequested = true;
	UE_LOG(LogMultiplayer, Log, TEXT("Restarting the current coop map after an authoritative victory."));
	RestartGame();
	return true;
}

/*
 * 服务器在开局扫描实际摆放的插槽数量，避免“关卡有三处目标、配置却写四”导致无法完成。
 * 没有插槽时回退到配置值；当前调用链只在开局扫描一次，不形成运行期轮询成本。
 */
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
		// 无插槽时保留非零目标，避免空关卡被直接判为目标完成。
		: FMath::Max(1, RequiredKeys);
}
