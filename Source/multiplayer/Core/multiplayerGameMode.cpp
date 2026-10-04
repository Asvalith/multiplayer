// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerGameMode.h"

#include "Engine/World.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameState.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UI/multiplayerVictoryWidget.h"
#include "Core/multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

//获取类
AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerControllerClass = AmultiplayerCoopPlayerController::StaticClass();
	HUDClass = AmultiplayerCoopHUD::StaticClass();

	//static 只在构造函数第一次执行时查找资源，避免每次创建 GameMode 都重复查找。
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != nullptr)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}

//获取已加载的配置，初始化规则；JSON 已由 GameInstance 读取，这里不再读文件。
void AmultiplayerGameMode::InitGameState()
{
	Super::InitGameState();
	const FmultiplayerGameplayConfig& Config = FmultiplayerGameplayConfig::Get(this);

	if (GameSession != nullptr)
	{
		GameSession->MaxPlayers = Config.SessionMaxPlayers;
	}

	//游戏开始前：初始化共享目标状态。
	if (AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>())
	{
		FmultiplayerCoopObjectiveState InitialState;
		// 从 Config 读取目标数量；加载失败时已由 Config 回退，这里不再遍历插槽或另设回退目标。
		InitialState.RequiredKeys = Config.RequiredKeys;
		CoopState->ApplyAuthoritativeState(InitialState);
	}
	UE_LOG(LogMultiplayer, Log, TEXT("Game rules configured: MaxPlayers=%d, RequiredKeys=%d"),
		Config.SessionMaxPlayers, Config.RequiredKeys);
}

//装钥匙
bool AmultiplayerGameMode::RegisterActivatedKey(TFunctionRef<bool()> CommitKey)
{
	//防重入与状态校验
	if (bRegisteringKey)
	{
		return false;
	}
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || CoopState->GetObjectiveState().bGameWon || CoopState->IsObjectiveComplete())
	{
		return false;
	}
	//标记安装
	TGuardValue<bool> RegisterGuard(bRegisteringKey, true);
	if (!CommitKey())
	{
		return false;
	}
	// 保留其余快照字段，只推进钥匙进度。
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	++NewState.ActivatedKeys;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

//判胜利
bool AmultiplayerGameMode::TryCompleteCoopGame(int32 CurrentPlayers, int32 RequiredPlayers)
{
	if (CurrentPlayers < FMath::Max(1, RequiredPlayers))
	{
		return false;
	}
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || CoopState->GetObjectiveState().bGameWon || !CoopState->IsObjectiveComplete())
	{
		return false;
	}
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	NewState.bGameWon = true;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

//重新开始
bool AmultiplayerGameMode::RequestRestartCurrentRound(AController* RequestingController)
{
	//前置校验
	UWorld* World = GetWorld();
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (World == nullptr || World->bIsTearingDown || GameSession == nullptr
		|| RequestingController == nullptr
		|| RequestingController->GetWorld() != World
		|| CoopState == nullptr
		|| !CoopState->GetObjectiveState().bGameWon
		|| PendingRestart.bRequested
		|| GetMatchState() == MatchState::LeavingMap
		|| !World->NextURL.IsEmpty() || World->IsInSeamlessTravel())
	{
		return false;
	}

	// 先锁定请求再触发 Travel，阻止两名玩家同帧点击导致重复重载。
	PendingRestart.bRequested = true;
	PendingRestart.PreviousMatchState = GetMatchState();
	PendingRestart.PreviousSwitchCountdown = World->NextSwitchCountdown;
	PendingRestart.Requester = RequestingController;
#if !UE_BUILD_SHIPPING
	static bool bRestartFailureInjected = false;
	if (!bRestartFailureInjected && FParse::Param(FCommandLine::Get(), TEXT("CoopTestFailRestartOnce")))
	{
		bRestartFailureInjected = true;
		// 模拟已进入离图状态但旧 World 尚存活，精确验证与真实失败共用的恢复分支。
		// 不广播引擎全局错误，避免引擎默认处理器另行断线/切回菜单干扰本分支。
		StartToLeaveMap();
		World->NextURL = TEXT("?Restart");
		PendingRestart.TravelURL = World->NextURL;
		RecoverFailedRestart(TEXT("CoopTest SyntheticTravelFailure injected once; no map load attempted."));
		return false;
	}
#endif
	// DS 的服务端身份来自进程；重开当前地图不需要追加 listen 或创建本地主机玩家。
	UE_LOG(LogMultiplayer, Log, TEXT("Restarting the current coop map after an authoritative victory."));
	RestartGame();
	if (!PendingRestart.bRequested)
	{
		return false;
	}
	PendingRestart.TravelURL = World->NextURL;
	// RestartGame 不返回结果；请求被 GameSession/CanServerTravel 拒绝时不会留下待跳转任务。
	if (World->NextURL.IsEmpty() && !World->IsInSeamlessTravel())
	{
		RecoverFailedRestart(TEXT("RestartGame did not schedule server travel."));
		return false;
	}
	return true;
}

//重开失败恢复
bool AmultiplayerGameMode::RecoverFailedRestart(const FString& FailureReason)
{
	UWorld* World = GetWorld();
	if (!PendingRestart.bRequested || World == nullptr || World->bIsTearingDown || World->GetAuthGameMode() != this)
	{
		return false;
	}

	// 先取出快照并释放旧请求，再恢复状态和通知外部，避免通知期间重入旧请求。
	const FPendingRestart FailedRestart = MoveTemp(PendingRestart);
	PendingRestart = FPendingRestart{};
	// 引擎已中止比赛或其他跳转接管时，不覆盖引擎自己的错误恢复。
	const bool bCanRestoreWorld = GetMatchState() != MatchState::Aborted
		&& (World->NextURL.IsEmpty() || World->NextURL == FailedRestart.TravelURL);
	if (bCanRestoreWorld)
	{
		// 上面已排除其他跳转接管；引擎也可能先清空本次失败 URL，因此两种情况都恢复本次快照。
		World->NextURL.Empty();
		// 重试仍需保留客户端接收 Travel RPC 的时间，不能把切图倒计时清零。
		World->NextSwitchCountdown = FailedRestart.PreviousSwitchCountdown;
		if (GetMatchState() == MatchState::LeavingMap && !FailedRestart.PreviousMatchState.IsNone())
		{
			// 恢复已有比赛而非再次开赛，避免 SetMatchState 重复执行 HandleMatchHasStarted。
			MatchState = FailedRestart.PreviousMatchState;
			if (AGameState* FullGameState = GetGameState<AGameState>())
			{
				FullGameState->SetMatchState(MatchState);
			}
		}
		UE_LOG(LogMultiplayer, Warning, TEXT("Restart failure recovered in the existing world; retry unlocked. Reason=%s"),
			*FailureReason);
	}

	// 无论是否能恢复旧 World，本次请求都已结束，只向请求者发送一次失败回执。
	if (AmultiplayerCoopPlayerController* Requester = Cast<AmultiplayerCoopPlayerController>(FailedRestart.Requester.Get()))
	{
		Requester->NotifyRestartFailed(NSLOCTEXT("Multiplayer", "RestartTravelFailed", "重开失败，可以重试或退出房间。"));
	}
	return bCanRestoreWorld;
}
