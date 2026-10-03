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
 * 父类先创建引擎 GameSession，再写入 JSON 容量；直连和重开使用同一入场上限。
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
 * 先校验进度，再同步安装钥匙，最后发布进度；拒绝的请求不能先改变钥匙状态。
 * 插槽负责自身去重，本函数阻止提交期间的嵌套登记；这不是跨网络的原子事务。
 */
bool AmultiplayerGameMode::RegisterActivatedKey(TFunctionRef<bool()> CommitKey)
{
	// 即使当前函数通常由服务器插槽调用，仍保留 Authority 检查，防止以后新增入口时破坏写权限边界。
	if (!HasAuthority() || bRegisteringKey)
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

	// 借用的回调仅在本栈帧执行；拒绝或安装失败都不发布进度。
	TGuardValue<bool> RegisterGuard(bRegisteringKey, true);
	if (!CommitKey())
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

bool AmultiplayerGameMode::RecoverFailedRestart(const FString& FailureReason)
{
	UWorld* World = GetWorld();
	if (!HasAuthority() || !PendingRestart.bRequested || World == nullptr || World->bIsTearingDown
		|| World->GetAuthGameMode() != this)
	{
		return false;
	}

	// 先取出快照并释放旧请求，再恢复状态和通知外部，避免通知期间重入旧请求。
	const FPendingRestart FailedRestart = MoveTemp(PendingRestart);
	PendingRestart = FPendingRestart{};
	AmultiplayerCoopPlayerController* Requester =
		Cast<AmultiplayerCoopPlayerController>(FailedRestart.Requester.Get());
	const FText Message = NSLOCTEXT("Multiplayer", "RestartTravelFailed", "重开失败，可以重试或退出房间。");

	// 引擎已中止比赛或其他跳转接管时，不覆盖引擎自己的错误恢复。
	if (GetMatchState() == MatchState::Aborted
		|| (!World->NextURL.IsEmpty() && World->NextURL != FailedRestart.TravelURL))
	{
		if (Requester != nullptr)
		{
			Requester->NotifyRestartFailed(Message);
		}
		return false;
	}
	if (World->NextURL == FailedRestart.TravelURL)
	{
		World->NextURL.Empty();
	}
	// 上面已排除其他跳转接管；引擎也可能先清空本次失败 URL，因此两种情况都恢复本次快照。
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
	if (Requester != nullptr)
	{
		Requester->NotifyRestartFailed(Message);
	}
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
