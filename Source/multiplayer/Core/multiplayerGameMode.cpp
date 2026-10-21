// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/multiplayerGameMode.h"

#include "Engine/World.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/GameState.h"
#include "GameFramework/HUD.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Core/multiplayerLog.h"
#include "UObject/ConstructorHelpers.h"
#include "TimerManager.h"
#include "Combat/multiplayerCombatCharacter.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

// 服务器规则层：集中处理接纳/短线恢复、目标结算、终局与回合重开。
// 机关提供事实，战斗角色提供个人快照；GameState 复制共享结果，本类不驱动客户端界面。

// 设置原生默认类并优先采用现有角色、GameState、HUD 蓝图，保留关卡配置入口。
AmultiplayerGameMode::AmultiplayerGameMode()
{
	GameStateClass = AmultiplayerCoopGameState::StaticClass();
	PlayerControllerClass = AmultiplayerCoopPlayerController::StaticClass();
	// 规则和复制仍由 C++ 负责；蓝图 GameState 通知本地 HUD 就绪，HUD 管理界面生命周期。
	static ConstructorHelpers::FClassFinder<AGameStateBase> CoopStateBPClass(TEXT("/Game/UI/BP_CoopGameState"));
	static ConstructorHelpers::FClassFinder<AHUD> CoopHUDBPClass(TEXT("/Game/UI/BP_CoopHUD"));
	if (CoopStateBPClass.Class != nullptr)
	{
		GameStateClass = CoopStateBPClass.Class;
	}
	if (CoopHUDBPClass.Class != nullptr)
	{
		HUDClass = CoopHUDBPClass.Class;
	}

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
		InitialState.RequiredKeys = Config.RequiredKeys;
		CoopState->ApplyAuthoritativeState(InitialState);
	}
}

// 引擎先创建/接管 Pawn，但服务器接纳前保持战斗与移动关闭；不新增逐帧握手轮询。
void AmultiplayerGameMode::RestartPlayer(AController* NewPlayer)
{
	AmultiplayerCoopPlayerController* Controller = Cast<AmultiplayerCoopPlayerController>(NewPlayer);
	// 本 Demo 没有个人复活规则；已接纳玩家不能借内置 RestartPlayer 刷新血量、资源或冷却。
	if (Controller && Controller->HasAcceptedCombatSession()) return;
	Super::RestartPlayer(NewPlayer);
}

// 在 Possess 前后关闭未接纳 Pawn 的行动资格，避免引擎 Restart 恢复移动绕过握手。
void AmultiplayerGameMode::FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation)
{
	// Pawn 已创建、Possess/PlayingState 尚未通知；本地权威玩家的同步握手也不会被随后重新关门。
	if (Cast<AmultiplayerCoopPlayerController>(NewPlayer))
		if (auto* Character = Cast<AmultiplayerCombatCharacter>(NewPlayer->GetPawn())) Character->SetCombatSessionReady(false);
	Super::FinishRestartPlayer(NewPlayer, StartRotation);
	// ACharacter::Restart 在 Possess 中恢复默认移动模式；未确认的远端玩家仍须禁行。
	if (auto* Controller = Cast<AmultiplayerCoopPlayerController>(NewPlayer); Controller && !Controller->HasAcceptedCombatSession())
		if (auto* Character = Cast<AmultiplayerCombatCharacter>(Controller->GetPawn())) Character->SetCombatSessionReady(false);
}

// 新连接登录后启动一次握手期限；尚未收到接纳请求的玩家不能无限占用未就绪 Pawn。
void AmultiplayerGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	AmultiplayerCoopPlayerController* Controller = Cast<AmultiplayerCoopPlayerController>(NewPlayer);
	if (!Controller || Controller->HasAcceptedCombatSession()) return;
	// 弱绑定 Controller 生命周期；超时只拒绝仍未接纳的连接，不影响已经完成恢复的玩家。
	GetWorldTimerManager().SetTimer(Controller->CombatHandshakeTimer, FTimerDelegate::CreateWeakLambda(Controller, [Controller]
	{
		if (!Controller->HasAcceptedCombatSession())
		{
			Controller->bCombatHandshakeSubmitted = true;
			Controller->ClientCombatSessionResult(FGuid(), 0, NSLOCTEXT("Multiplayer", "CombatHandshakeTimeout", "会话确认超时，请重新连接服务器。"), false);
			Controller->SetLifeSpan(1.0f);
		}
	}), 15.0f, false);
}

// 验证、消费票据和恢复集中在同一权威入口；票据只在内存与所属 Controller RPC 中传递。
// 设计：票据定位本局快照，在线 Controller 占用阻止同一状态被重复接入；不是账号认证系统。
// 轮换兼容保留客户端实际持有的旧票据别名，但别名仍共享同一个在线占用。
bool AmultiplayerGameMode::ClaimCombatSession(AmultiplayerCoopPlayerController* Controller, const FGuid& Ticket,
	FGuid& OutTicket, FText& OutError, bool& bRetryable)
{
	OutTicket.Invalidate(); OutError = FText::GetEmpty(); bRetryable = false;
	AmultiplayerCombatCharacter* Character = Controller ? Cast<AmultiplayerCombatCharacter>(Controller->GetPawn()) : nullptr;
	if (!Character || !Controller->HasAuthority() || Controller->GetWorld() != GetWorld()
		|| Controller->HasAcceptedCombatSession() || Character->IsCombatReady())
	{
		OutError = NSLOCTEXT("Multiplayer", "CombatSessionNotReady", "玩家尚未就绪，无法确认会话。");
		return false;
	}
	const double Now = FPlatformTime::Seconds();
	for (auto It = CombatSessions.CreateIterator(); It; ++It)
		if (!It.Value().Controller.IsValid() && It.Value().ExpiresAt <= Now) It.RemoveCurrent();
	FCombatSession Session;
	FGuid PreviousKey;
	if (Ticket.IsValid())
	{
		PreviousKey = Ticket;
		FCombatSession* Previous = CombatSessions.Find(Ticket);
		if (!Previous)
			for (auto& Entry : CombatSessions)
				if (Entry.Value.PreviousTicket == Ticket) { Previous = &Entry.Value; PreviousKey = Entry.Key; break; }
		if (!Previous)
		{
			OutError = NSLOCTEXT("Multiplayer", "CombatSessionExpired", "重连凭据已失效或服务器已经重启，请重新加入。");
			return false;
		}
		if (Previous->Controller.IsValid())
		{
			bRetryable = true;
			OutError = NSLOCTEXT("Multiplayer", "CombatSessionStillOnline", "原会话仍在线，等待断线清理后重试。");
			return false;
		}
		Session = MoveTemp(*Previous);
		CombatSessions.Remove(PreviousKey);
		// 保留客户端真正持有的那张票据；连续丢失轮换回执时仍可使用，但不能绕过同一个在线锁。
		Session.PreviousTicket = Ticket;
	}
	else if (CombatSessions.Num() >= 32)
	{
		OutError = NSLOCTEXT("Multiplayer", "CombatSessionCacheFull", "服务器暂时无法接纳新会话，请稍后重试。");
		return false;
	}
	const FmultiplayerCombatResumeState SavedState = Session.State;
	const FCombatSession RecoverySession = Session;
	const float OfflineSeconds = static_cast<float>(FMath::Max(0.0, Now - Session.SavedAt));
	FGuid IssuedTicket;
	do { IssuedTicket = FGuid::NewGuid(); } while (CombatSessions.Contains(IssuedTicket));
	Session.Controller = Controller;
	CombatSessions.Add(IssuedTicket, MoveTemp(Session));
	Controller->ServerCombatTicket = IssuedTicket;
	if (Ticket.IsValid()) Character->RestoreResumeState(SavedState, OfflineSeconds);
	if (!IsValid(Character) || !IsValid(Controller) || Controller->GetPawn() != Character)
	{
		// 恢复通知允许同步销毁/换Pawn；失败不能消费用户仅有的旧票据。
		CombatSessions.Remove(IssuedTicket);
		if (PreviousKey.IsValid()) CombatSessions.Add(PreviousKey, RecoverySession);
		if (IsValid(Controller)) Controller->ServerCombatTicket.Invalidate();
		bRetryable = true;
		OutError = NSLOCTEXT("Multiplayer", "CombatRestoreFailed", "个人状态恢复未完成，请重新连接。");
		return false;
	}
	Controller->bCombatSessionAccepted = true;
	Character->SetCombatSessionReady(true);
	OutTicket = IssuedTicket;
	UE_LOG(LogMultiplayer, Display, TEXT("Combat session accepted: restored=%d; shared round state retained."), Ticket.IsValid());
	OnCombatSessionAccepted.Broadcast();
	return true;
}

// 断线前保存服务器个人状态并释放在线锁，快照只在本 GameMode 内保留 120 秒。
// 取舍：使用单调实时时钟扣除离线时间；缓存随本局 GameMode 消失，不承担服务器重启持久化。
void AmultiplayerGameMode::SaveCombatSession(AmultiplayerCoopPlayerController* Controller)
{
	if (!Controller || !Controller->HasAcceptedCombatSession()) return;
	FCombatSession* Session = CombatSessions.Find(Controller->ServerCombatTicket);
	AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(Controller->GetPawn());
	if (!Session || Session->Controller.Get() != Controller || !Character) return;
	Session->State = Character->CaptureResumeState();
	Session->SavedAt = FPlatformTime::Seconds();
	Session->ExpiresAt = Session->SavedAt + 120.0;
	Session->Controller.Reset();
	Character->SetCombatSessionReady(false);
}

// 补足普通退出路径的保存；Controller 提前保存后，此处不会重复覆盖已释放的会话。
void AmultiplayerGameMode::Logout(AController* Exiting)
{
	SaveCombatSession(Cast<AmultiplayerCoopPlayerController>(Exiting));
	Super::Logout(Exiting);
}

// 整局切图前清除各所属客户端的旧回合票据；新 GameMode 不继承旧会话缓存。
APlayerController* AmultiplayerGameMode::ProcessClientTravel(FString& URL, bool bSeamless, bool bAbsolute)
{
	// 新回合的 GameMode 没有旧票据缓存；清理 RPC 必须先于引擎发送 ClientTravel。
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		if (auto* Controller = Cast<AmultiplayerCoopPlayerController>(It->Get())) Controller->ClientSetReconnectTicket(FGuid());
	return Super::ProcessClientTravel(URL, bSeamless, bAbsolute);
}

// 战斗终局只由服务器复核；尚有可行动队友时仍允许其救援和撤离。
void AmultiplayerGameMode::NotifyCombatPlayerDeath()
{
	auto* State = GetGameState<AmultiplayerCoopGameState>();
	if (!State || State->HasGameWon() || State->HasGameLost()) return;
	bool bHasCombatPlayers = false;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		const auto* PC = It->Get();
		const auto* Character = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetPawn()) : nullptr;
		if (!Character) continue;
		bHasCombatPlayers = true;
		if (Character->IsCombatReady()) return;
	}
	if (bHasCombatPlayers)
	{
		auto Result = State->GetObjectiveState();
		Result.bGameLost = true;
		State->ApplyAuthoritativeState(Result);
	}
}

// 安装成功才发布共享进度；避免附件安装失败却已增加目标数量，两份事实必须按此顺序提交。
bool AmultiplayerGameMode::RegisterActivatedKey(TFunctionRef<bool()> CommitKey)
{
	//防重入与状态校验
	if (bRegisteringKey)
	{
		return false;
	}
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || CoopState->HasGameLost() || CoopState->GetObjectiveState().bGameWon || CoopState->IsObjectiveComplete())
	{
		return false;
	}
	//标记安装
	TGuardValue<bool> RegisterGuard(bRegisteringKey, true);
	if (!CommitKey())
	{
		return false;
	}
	//更新状态
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	++NewState.ActivatedKeys;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

//判胜利
bool AmultiplayerGameMode::TryCompleteCoopGame(int32 CurrentPlayers, int32 RequiredPlayers)
{
	//细节：合法性兜底
	if (CurrentPlayers < FMath::Max(1, RequiredPlayers))
	{
		return false;
	}
	//状态校验
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (CoopState == nullptr || CoopState->HasGameLost() || CoopState->GetObjectiveState().bGameWon || !CoopState->IsObjectiveComplete())
	{
		return false;
	}
	//更新
	FmultiplayerCoopObjectiveState NewState = CoopState->GetObjectiveState();
	NewState.bGameWon = true;
	CoopState->ApplyAuthoritativeState(NewState);
	return true;
}

//重新开始（传入发起者）
bool AmultiplayerGameMode::RequestRestartCurrentRound(AController* RequestingController)
{
	//前置校验
	UWorld* World = GetWorld();
	AmultiplayerCoopGameState* CoopState = GetGameState<AmultiplayerCoopGameState>();
	if (World == nullptr || World->bIsTearingDown || GameSession == nullptr
		|| RequestingController == nullptr
		|| RequestingController->GetWorld() != World
		|| CoopState == nullptr
		|| (!CoopState->HasGameWon() && !CoopState->HasGameLost())
		|| PendingRestart.bRequested
		|| GetMatchState() == MatchState::LeavingMap
		|| !World->NextURL.IsEmpty() || World->IsInSeamlessTravel())
	{
		return false;
	}

	//****先锁定请求再触发 Travel，阻止两名玩家同帧点击导致重复重载
	PendingRestart.bRequested = true;
	PendingRestart.PreviousMatchState = GetMatchState();
	PendingRestart.PreviousSwitchCountdown = World->NextSwitchCountdown;
	PendingRestart.Requester = RequestingController;

//测试分支：模拟重开失败（只注入一次）
#if !UE_BUILD_SHIPPING
	static bool bRestartFailureInjected = false;
	if (!bRestartFailureInjected && FParse::Param(FCommandLine::Get(), TEXT("CoopTestFailRestartOnce")))
	{
		bRestartFailureInjected = true;
		StartToLeaveMap();
		World->NextURL = TEXT("?Restart");
		PendingRestart.TravelURL = World->NextURL;
		RecoverFailedRestart(TEXT("CoopTest SyntheticTravelFailure injected once; no map load attempted."));
		return false;
	}
#endif


	RestartGame();
	if (!PendingRestart.bRequested)
	{
		return false;
	}
	PendingRestart.TravelURL = World->NextURL;
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
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			if (auto* Controller = Cast<AmultiplayerCoopPlayerController>(It->Get())) Controller->ClientSetReconnectTicket(Controller->ServerCombatTicket);
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
