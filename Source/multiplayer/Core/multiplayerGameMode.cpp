// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerGameMode.h"

#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameState.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Core/multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

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
 * 父类先创建 GameSession，再写入 JSON 容量；原地建房、直连和重开使用同一入场上限。
 */
void AmultiplayerGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	if (GameSession != nullptr)
	{
		GameSession->MaxPlayers = FmultiplayerGameplayConfig::Get(this).SessionMaxPlayers;
		UE_LOG(LogMultiplayer, Log, TEXT("Server player capacity configured: %d"), GameSession->MaxPlayers);
	}
}

/** 目标数量仍按实际插槽统计，并走 GameState 的统一写入口生成初始快照。 */
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
	UWorld* World = GetWorld();
	AmultiplayerCoopGameState* CoopState =
		GetGameState<AmultiplayerCoopGameState>();
	if (!HasAuthority()
		|| World == nullptr || World->bIsTearingDown || GameSession == nullptr
		|| RequestingController == nullptr
		|| RequestingController->GetWorld() != GetWorld()
		|| CoopState == nullptr
		|| !CoopState->GetObjectiveState().bGameWon
		|| bRestartRequested
		|| GetMatchState() == MatchState::LeavingMap
		|| !World->NextURL.IsEmpty() || World->IsInSeamlessTravel())
	{
		return false;
	}

	// 先锁定请求再触发 Travel，阻止两名玩家同帧点击导致重复重载。
	bRestartRequested = true;
	MatchStateBeforeRestart = GetMatchState();
	NextSwitchCountdownBeforeRestart = World->NextSwitchCountdown;
	PendingRestartURL.Reset();
#if !UE_BUILD_SHIPPING
	static bool bRestartFailureInjected = false;
	if (!bRestartFailureInjected && FParse::Param(FCommandLine::Get(), TEXT("CoopTestFailRestartOnce")))
	{
		bRestartFailureInjected = true;
		// 模拟已进入离图状态但旧 World 尚存活，精确验证与真实失败共用的恢复分支。
		// 不广播引擎全局错误，避免引擎默认处理器另行断线/切回菜单干扰本分支。
		StartToLeaveMap();
		World->NextURL = TEXT("?Restart");
		PendingRestartURL = World->NextURL;
		RecoverFailedRestart(TEXT("CoopTest SyntheticTravelFailure injected once; no map load attempted."));
		return false;
	}
#endif
	// 原地 World::Listen 不会更新引擎的 LastURL。RestartGame 的 ?Restart 会重新读取它，
	// 若遗漏 listen，新世界就会退回单机，客户端无法重连。这里只保留当前关卡的监听模式。
	if (World->GetNetMode() == NM_ListenServer && GEngine != nullptr)
	{
		if (FWorldContext* Context = GEngine->GetWorldContextFromWorld(World))
		{
			Context->LastURL.AddOption(TEXT("listen"));
		}
	}
	UE_LOG(LogMultiplayer, Log, TEXT("Restarting the current coop map after an authoritative victory."));
	RestartGame();
	if (!bRestartRequested)
	{
		return false;
	}
	PendingRestartURL = World->NextURL;
	// RestartGame 不返回结果；请求被 GameSession/CanServerTravel 拒绝时不会留下待跳转任务。
	if (World->NextURL.IsEmpty() && !World->IsInSeamlessTravel())
	{
		RecoverFailedRestart(TEXT("RestartGame did not schedule server travel."));
		return false;
	}
	return true;
}

bool AmultiplayerGameMode::RecoverFailedRestart(const FString& FailureReason)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !bRestartRequested || World == nullptr || World->bIsTearingDown
		|| World->GetAuthGameMode() != this)
	{
		return false;
	}

	// 引擎已中止比赛或其他跳转接管时，只释放本次锁，不覆盖引擎自己的错误恢复。
	if (GetMatchState() == MatchState::Aborted
		|| (!World->NextURL.IsEmpty() && World->NextURL != PendingRestartURL))
	{
		bRestartRequested = false;
		MatchStateBeforeRestart = NAME_None;
		NextSwitchCountdownBeforeRestart = 0.f;
		PendingRestartURL.Reset();
		return false;
	}
	if (World->NextURL == PendingRestartURL)
	{
		World->NextURL.Empty();
	}
	// 上面已排除其他跳转接管；引擎也可能先清空本次失败 URL，因此两种情况都恢复本次快照。
	// 不能清零：Listen Server 重试不会自动重置此倒计时，需要保留客户端接收 Travel RPC 的时间。
	World->NextSwitchCountdown = NextSwitchCountdownBeforeRestart;
	if (GetMatchState() == MatchState::LeavingMap && !MatchStateBeforeRestart.IsNone())
	{
		// 恢复已有比赛而非再次开赛，避免 SetMatchState 重复执行 HandleMatchHasStarted。
		MatchState = MatchStateBeforeRestart;
		if (AGameState* FullGameState = GetGameState<AGameState>())
		{
			FullGameState->SetMatchState(MatchState);
		}
	}
	bRestartRequested = false;
	MatchStateBeforeRestart = NAME_None;
	NextSwitchCountdownBeforeRestart = 0.f;
	PendingRestartURL.Reset();
	UE_LOG(LogMultiplayer, Warning, TEXT("Restart failure recovered in the existing world; retry unlocked. Reason=%s"),
		*FailureReason);
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
