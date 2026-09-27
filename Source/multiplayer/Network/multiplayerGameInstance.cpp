// Copyright Epic Games, Inc. All Rights Reserved.

#include "Network/multiplayerGameInstance.h"

#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "GameMapsSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameSession.h"
#include "Misc/Paths.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "Core/multiplayerLog.h"
#include "Core/multiplayerGameMode.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#endif

/**
 * 会话流程分成两层：OnlineSession 负责公布和查找房间，ClientTravel 负责连接游戏服务器。
 * 建房使用当前地图的监听服务器；加入和退出仍会涉及客户端加载 World，但这里不负责自动选关。
 * 本文件把入口、完成回调、失败清理和有限重试集中在 GameInstance，便于沿一条调用链排查问题。
 */
namespace MultiplayerSession
{
	// 房间名使用固定键，主机写入与客户端搜索读取必须一致。
	const FName ServerNameKey(TEXT("SERVER_NAME"));

	/**
	 * 判断全局失败事件是否属于本实例的游戏连接。
	 * 优先核对 World；连接尚在建立时再查 PendingNetGame。无法确认归属时忽略，避免串扰其他 PIE 窗口。
	 */
	bool IsFailureForGameInstance(
		const UGameInstance* GameInstance,
		const UWorld* FailureWorld,
		const UNetDriver* FailureDriver)
	{
		if (GameInstance == nullptr)
		{
			return false;
		}

		if (FailureWorld != nullptr)
		{
			if (FailureWorld->GetGameInstance() != GameInstance)
			{
				return false;
			}

			// NetDriverCreateFailure 可能在驱动尚未创建时传入 nullptr；只要 World
			// 明确属于本实例，就仍应让该实例结束 Connecting/会话忙碌状态。
			if (FailureDriver == nullptr
				|| FailureWorld->GetNetDriver() == FailureDriver)
			{
				return true;
			}
		}

		// PendingConnectionFailure 是 UE5.5 中明确会以空 World 广播的路径。
		// 其余情况通过本 GameInstance 的 WorldContext 找回归属，并且只接受 PendingNetGame
		// 或 GameNetDriver；Beacon、Demo 等辅助驱动失败不能清理主游戏会话。
		// World 和 Driver 都为空时无法安全区分同进程中的多个 GameInstance。
		if (FailureDriver == nullptr || GEngine == nullptr)
		{
			return false;
		}

		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.OwningGameInstance != GameInstance)
			{
				continue;
			}
			if (FailureWorld != nullptr && Context.World() != FailureWorld)
			{
				continue;
			}

			if (Context.PendingNetGame != nullptr
				&& Context.PendingNetGame->GetNetDriver() == FailureDriver)
			{
				return true;
			}

			for (const FNamedNetDriver& NamedDriver : Context.ActiveNetDrivers)
			{
				if (NamedDriver.NetDriver == FailureDriver
					&& NamedDriver.NetDriverDef != nullptr
					&& NamedDriver.NetDriverDef->DefName == NAME_GameNetDriver)
				{
					return true;
				}
			}
		}

		return false;
	}
}

/** 初始化共享会话接口和引擎事件订阅；接口不可用时记录日志，后续公开入口各自检查可用性。 */
void UmultiplayerGameInstance::Init()
{
	Super::Init();

	// 不在 Actor 构造或 Tick 中读磁盘；跨关卡复用同一份启动配置，改表后需重启游戏实例。
	GameplayConfig.LoadFromFile(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Config/Gameplay.json")));

	// 网络失败和 Travel 失败是引擎级事件，不属于某个临时 World；绑定在跨地图存活的 GameInstance 上。
	if (GEngine != nullptr)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(
			this,
			&UmultiplayerGameInstance::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(
			this,
			&UmultiplayerGameInstance::HandleTravelFailure);
	}

	IOnlineSubsystem* OnlineSubsystem = IOnlineSubsystem::Get();
	if (OnlineSubsystem != nullptr)
	{
		SessionInterface = OnlineSubsystem->GetSessionInterface();
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Session subsystem initialized: %s"),
			*OnlineSubsystem->GetSubsystemName().ToString());
	}

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Online session interface is unavailable."));
	}

	// 退出互斥必须等新 World 就绪后释放。
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this,
		&UmultiplayerGameInstance::HandlePostLoadMap);
}

/** 结束本实例持有的定时任务和订阅，再释放搜索结果与接口；这里只做对象退出清理，不发起新的 Travel。 */
void UmultiplayerGameInstance::Shutdown()
{
	// 先停计时器再解绑回调，避免 Shutdown 过程中新的重连尝试重新进入本对象。
	GetTimerManager().ClearTimer(ReconnectTimerHandle);

	if (GEngine != nullptr)
	{
		if (NetworkFailureHandle.IsValid())
		{
			GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		}
		if (TravelFailureHandle.IsValid())
		{
			GEngine->OnTravelFailure().Remove(TravelFailureHandle);
		}
	}

	NetworkFailureHandle.Reset();
	TravelFailureHandle.Reset();
	if (PostLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
		PostLoadMapHandle.Reset();
	}
	ClearSessionDelegates();
	SessionSearch.Reset();
	SessionInterface.Reset();

	Super::Shutdown();
}

/**
 * 接受建房请求后锁住会话操作，保存参数，并在当前 World 开启监听。
 * 若已有同名 Session，销毁完成回调负责继续 CreateSession；任一提交失败分支负责释放操作状态。
 * 监听驱动和 Session 是两份状态；失败时只回滚本次新建的监听，不拆除已有主机连接。
 */
void UmultiplayerGameInstance::HostGame(
	const FString& ServerName,
	int32 PublicConnections,
	bool bIsLanMatch)
{
	if (!SessionInterface.IsValid()
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Hosting))
	{
		return;
	}

	// 只有请求通过会话互斥检查后，才允许它终止旧重连。
	// 被退出流程拒绝的按钮点击不能反向撤销 bLeaveInProgress，也不能清掉可重连地址。
	CancelAutomaticReconnect();

	HostStartedListeningWorld.Reset();
	HostStartedNetDriver.Reset();
	if (!EnsureCurrentWorldIsListening())
	{
		EndSessionOperation();
		return;
	}

	// 参数要跨越“销毁旧会话”的异步间隔，因此先保存到 GameInstance，而不是捕获临时局部变量。
	PendingServerName = ServerName.IsEmpty() ? TEXT("Coop Session") : ServerName;
	if (PublicConnections != GameplayConfig.SessionMaxPlayers)
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Session capacity uses Gameplay.json: %d (legacy menu value: %d)."),
			GameplayConfig.SessionMaxPlayers, PublicConnections);
	}
	// 当前地图已经开始运行，建房是原地开启监听；同步设置实际入场检查，不能只改会话广告人数。
	if (AGameModeBase* GameMode = GetWorld()->GetAuthGameMode())
	{
		if (GameMode->GameSession != nullptr)
		{
			GameMode->GameSession->MaxPlayers = GameplayConfig.SessionMaxPlayers;
		}
	}
	bPendingIsLanMatch = bIsLanMatch;

	if (SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
		// (**) 同名会话未销毁时直接 CreateSession 通常会失败，先异步销毁再继续创建。
		BindDestroyDelegate(EMultiplayerDestroyPurpose::RecreateSession);

		const bool bRequestAccepted = SessionInterface->DestroySession(NAME_GameSession);
		if (!bRequestAccepted && DestroySessionCompleteHandle.IsValid())
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
				DestroySessionCompleteHandle);
			DestroySessionCompleteHandle.Reset();
			DestroyPurpose = EMultiplayerDestroyPurpose::None;
			RollbackHostListener();
			EndSessionOperation();
			UE_LOG(LogMultiplayer, Error, TEXT("Existing session could not be destroyed before hosting."));
		}
		return;
	}

	CreateSession();
}

/**
 * 使用 HostGame 保存的参数公布房间；调用前由建房流程保证 SessionInterface 有效且占用 Hosting。
 * (**) 返回值表示请求是否受理，完成事件表示结果；Null 可能同步触发事件，必须防止失败收尾做两遍。
 */
void UmultiplayerGameInstance::CreateSession()
{
	// SessionSettings 描述的是会话如何被发现和加入，不会替代真正的 NetDriver 连接与地图 Travel。
	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = bPendingIsLanMatch;
	Settings.NumPublicConnections = GameplayConfig.SessionMaxPlayers;
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	// LAN 使用局域网广播；在线模式才启用 Presence/Lobby，避免 Null 子系统下的无效配置。
	Settings.bAllowJoinViaPresence = !bPendingIsLanMatch;
	Settings.bUsesPresence = !bPendingIsLanMatch;
	Settings.bUseLobbiesIfAvailable = !bPendingIsLanMatch;

	// 地图名和服务器名作为可广播的会话元数据，搜索列表无需连接服务器就能展示摘要。
	const UWorld* World = GetWorld();
	const FString CurrentMap = World != nullptr
		? World->GetOutermost()->GetName()
		: TEXT("Unknown");
	Settings.Set(
		SETTING_MAPNAME,
		CurrentMap,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(
		MultiplayerSession::ServerNameKey,
		PendingServerName,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

	// 先绑定完成回调再发请求，兼容可能很快完成的 OnlineSubsystem 实现。
	CreateSessionCompleteHandle =
		SessionInterface->AddOnCreateSessionCompleteDelegate_Handle(
			FOnCreateSessionCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleCreateSessionComplete));

#if !UE_BUILD_SHIPPING
	// 仅注入一次完成失败，走与真实创建失败相同的回滚入口；自动驾驶由独立测试驱动负责。
	if (!bHostFailureInjected && FParse::Param(FCommandLine::Get(), TEXT("CoopTestFailHostSessionOnce")))
	{
		bHostFailureInjected = true;
		UE_LOG(LogMultiplayer, Warning, TEXT("CoopTest SyntheticSessionCreateFailure injected once."));
		HandleCreateSessionComplete(NAME_GameSession, false);
		return;
	}
#endif

	const bool bRequestAccepted = SessionInterface->CreateSession(
		0,
		NAME_GameSession,
		Settings);
	// Null 子系统可能在函数返回前同步执行完成回调。句柄失效表示回调已经收口，不能再处理一次。
	if (!bRequestAccepted && CreateSessionCompleteHandle.IsValid())
	{
		SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(
			CreateSessionCompleteHandle);
		CreateSessionCompleteHandle.Reset();
		RollbackHostListener();
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("CreateSession request was rejected."));
	}
}

/**
 * 创建本轮查询对象并启动搜索。成功取得操作权才替换旧结果、取消旧重连，拒绝的请求不修改原流程。
 * 菜单拿到的是结果摘要；真正加入仍使用 SessionSearch 保存的原始结果。
 */
void UmultiplayerGameInstance::FindGames(int32 MaxResults, bool bIsLanQuery)
{
	if (!SessionInterface.IsValid())
	{
		OnFindComplete.Broadcast(false, {});
		return;
	}
	if (!BeginSessionOperation(EMultiplayerSessionOperation::Finding))
	{
		// 互斥拒绝不代表本轮搜索失败；原操作仍会通过自己的回调结束。
		return;
	}

	// 与 Host/Join 保持相同顺序：先取得操作权，再修改重连状态。
	CancelAutomaticReconnect();

	// 新对象同时承载查询参数和真实搜索结果；菜单中的 ResultIndex 只在这份对象存活期间有效。
	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = FMath::Max(1, MaxResults);
	SessionSearch->bIsLanQuery = bIsLanQuery;

	// Null/LAN 使用广播发现；非 LAN 查询才增加 Lobby 条件，避免过滤掉 Null 子系统结果。
	if (!bIsLanQuery)
	{
		SessionSearch->QuerySettings.Set(
			SEARCH_LOBBIES,
			true,
			EOnlineComparisonOp::Equals);
	}

	FindSessionsCompleteHandle =
		SessionInterface->AddOnFindSessionsCompleteDelegate_Handle(
			FOnFindSessionsCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleFindSessionsComplete));

	const bool bRequestAccepted = SessionInterface->FindSessions(
		0,
		SessionSearch.ToSharedRef());
	if (!bRequestAccepted && FindSessionsCompleteHandle.IsValid())
	{
		SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(
			FindSessionsCompleteHandle);
		FindSessionsCompleteHandle.Reset();
		SessionSearch.Reset();
		EndSessionOperation();
		OnFindComplete.Broadcast(false, {});
	}
}

/**
 * 按当前结果下标提交加入请求；有效请求会清除旧连接地址，等待回调提供新地址。
 * 下标检查只能防止越界，不能识别“旧列表中的同一个数字”；菜单必须使用最近一次搜索提供的结果。
 */
void UmultiplayerGameInstance::JoinGame(int32 ResultIndex)
{
	// 先验证当前搜索对象和下标，避免无效请求取消原重连；结果是否来自最新列表由调用方保证。
	if (!SessionInterface.IsValid()
		|| !SessionSearch.IsValid()
		|| !SessionSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		return;
	}

	if (!BeginSessionOperation(EMultiplayerSessionOperation::Joining))
	{
		return;
	}

	// 非法下标或重叠请求在上面已经返回，不会破坏正在等待的自动重连。
	CancelAutomaticReconnect();

	JoinSessionCompleteHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
			FOnJoinSessionCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleJoinSessionComplete));

	const bool bRequestAccepted = SessionInterface->JoinSession(
		0,
		NAME_GameSession,
		SessionSearch->SearchResults[ResultIndex]);
	if (!bRequestAccepted && JoinSessionCompleteHandle.IsValid())
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(
			JoinSessionCompleteHandle);
		JoinSessionCompleteHandle.Reset();
		RemoveFailedLocalSession();
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("JoinSession request was rejected."));
	}
}

/** 当前 World 已监听则复用；独立游戏尝试原地启动 NetDriver，客户端和空 World 直接拒绝。 */
bool UmultiplayerGameInstance::EnsureCurrentWorldIsListening()
{
	UWorld* World = GetWorld();
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Current world cannot become a listen server."));
		return false;
	}

	if (World->GetNetMode() == NM_ListenServer)
	{
		return true;
	}

	// 原地建立监听 NetDriver，不进行 ServerTravel，因此创建房间不会隐式改变当前地图。
	FURL ListenUrl = World->URL;
	ListenUrl.AddOption(TEXT("listen"));
	if (!World->Listen(ListenUrl))
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Listen server could not start on the current map."));
		return false;
	}

	HostStartedListeningWorld = World;
	HostStartedNetDriver = World->GetNetDriver();
	UE_LOG(
		LogMultiplayer,
		Log,
		TEXT("Listen server started on current map: %s"),
		*World->GetOutermost()->GetName());
	return true;
}

void UmultiplayerGameInstance::RollbackHostListener()
{
	UWorld* StartedWorld = HostStartedListeningWorld.Get();
	UNetDriver* StartedDriver = HostStartedNetDriver.Get();
	// 先释放所有权记录，避免驱动销毁引起的嵌套通知再次执行回滚。
	HostStartedListeningWorld.Reset();
	HostStartedNetDriver.Reset();
	if (GEngine == nullptr || StartedWorld == nullptr || StartedDriver == nullptr
		|| StartedWorld != GetWorld() || StartedWorld->GetGameInstance() != this
		|| StartedWorld->GetNetDriver() != StartedDriver)
	{
		return;
	}

	// 精确销毁这一条驱动；引擎会同时清理 World 与 LevelCollection 中的引用。
	GEngine->DestroyNamedNetDriver(StartedWorld, StartedDriver->NetDriverName);
	UE_LOG(LogMultiplayer, Log, TEXT("Host rollback removed the newly created listen driver; NetMode=%d."),
		static_cast<int32>(StartedWorld->GetNetMode()));
}

/** 销毁前先记录用途并替换旧订阅；即使 DestroySession 同步完成，回调也能决定建房还是返回菜单。 */
void UmultiplayerGameInstance::BindDestroyDelegate(
	EMultiplayerDestroyPurpose Purpose)
{
	if (DestroySessionCompleteHandle.IsValid())
	{
		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
			DestroySessionCompleteHandle);
	}
	DestroyPurpose = Purpose;

	DestroySessionCompleteHandle =
		SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
			FOnDestroySessionCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleDestroySessionComplete));
}

/** 创建结果只结束会话公布阶段并解锁菜单；主机此前已经监听当前地图，此处无需额外切图。 */
void UmultiplayerGameInstance::HandleCreateSessionComplete(
	FName SessionName,
	bool bWasSuccessful)
{
	// 回调一进入就解绑自己，确保后续失败分支、Travel 失败或再次建房都不会重复收到旧完成事件。
	SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(
		CreateSessionCompleteHandle);
	CreateSessionCompleteHandle.Reset();

	if (!bWasSuccessful)
	{
		RollbackHostListener();
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("Session creation failed."));
		return;
	}
	// 创建成功后，监听成为房间长期资源，不再属于本次失败回滚。
	HostStartedListeningWorld.Reset();
	HostStartedNetDriver.Reset();
	EndSessionOperation();
	UE_LOG(LogMultiplayer, Log, TEXT("Session created on the current map; no automatic map travel was requested."));
}

/**
 * 先释放本次搜索订阅，再把原始结果转换成蓝图可读值。
 * 广播结果前结束 Finding，使菜单能在回调中直接调用 JoinGame。
 */
void UmultiplayerGameInstance::HandleFindSessionsComplete(bool bWasSuccessful)
{
	SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(
		FindSessionsCompleteHandle);
	FindSessionsCompleteHandle.Reset();

	// 只把菜单需要的稳定值复制到蓝图结构体；原始搜索结果仍保留给 JoinSession 使用。
	TArray<FmultiplayerSessionInfo> Results;
	if (bWasSuccessful && SessionSearch.IsValid())
	{
		Results.Reserve(SessionSearch->SearchResults.Num());

		for (int32 Index = 0; Index < SessionSearch->SearchResults.Num(); ++Index)
		{
			const FOnlineSessionSearchResult& SearchResult =
				SessionSearch->SearchResults[Index];

			FmultiplayerSessionInfo Info;
			Info.ResultIndex = Index;
			Info.ServerName = SearchResult.Session.OwningUserName;
			SearchResult.Session.SessionSettings.Get(
				MultiplayerSession::ServerNameKey,
				Info.ServerName);
			Info.PingInMs = SearchResult.PingInMs;
			Info.MaxPlayers = SearchResult.Session.SessionSettings.NumPublicConnections;
			Info.CurrentPlayers = FMath::Max(
				0,
				Info.MaxPlayers - SearchResult.Session.NumOpenPublicConnections);
			// 摘要之后不再使用，移动进数组即可；其中的 ResultIndex 始终指向原始 SearchResults。
			Results.Add(MoveTemp(Info));
		}
	}

	EndSessionOperation();
	OnFindComplete.Broadcast(bWasSuccessful, Results);
}

/**
 * 会话加入成功后解析地址并发起 ClientTravel，Joining 继续保持到本地控制器进入 PlayingState。
 * 无可用地址或控制器时清掉本地同名 Session 再解锁，避免下一次加入被残留记录阻塞。
 */
void UmultiplayerGameInstance::HandleJoinSessionComplete(
	FName SessionName,
	EOnJoinSessionCompleteResult::Type Result)
{
	SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(
		JoinSessionCompleteHandle);
	JoinSessionCompleteHandle.Reset();

	// JoinSession 成功只表示会话层接受加入，还必须从子系统解析 NetDriver 能使用的实际地址。
	FString ConnectString;
	const bool bResolved = Result == EOnJoinSessionCompleteResult::Success
		&& SessionInterface->GetResolvedConnectString(SessionName, ConnectString);

	UWorld* World = GetWorld();
	APlayerController* PlayerController =
		World != nullptr ? World->GetFirstPlayerController() : nullptr;

	// ClientTravel 必须由本地 PlayerController 发起；没有本地控制器时不能假装加入成功。
	const bool bCanTravel = bResolved && PlayerController != nullptr;

	if (bCanTravel)
	{
		// (*) JoinSession 只加入在线会话；还要解析真实地址并由本地控制器 ClientTravel。
		LastConnectString = ConnectString;
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
		return;
	}

	// 先移除失败的本地会话，再开放菜单操作，避免紧接着加入时撞上同名残留。
	RemoveFailedLocalSession();
	EndSessionOperation();
	UE_LOG(LogMultiplayer, Error, TEXT("Session join completed without a usable connection address."));
}

/**
 * 先取出并清空本次销毁用途，再进入后续流程，避免嵌套调用继续使用上一次用途。
 * 重新建房要求销毁成功；主动退出即使销毁失败也继续返回菜单，失败信息保留在日志中。
 */
void UmultiplayerGameInstance::HandleDestroySessionComplete(
	FName SessionName,
	bool bWasSuccessful)
{
	SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
		DestroySessionCompleteHandle);
	DestroySessionCompleteHandle.Reset();

	const EMultiplayerDestroyPurpose CompletedPurpose = DestroyPurpose;
	DestroyPurpose = EMultiplayerDestroyPurpose::None;

	if (CompletedPurpose == EMultiplayerDestroyPurpose::LeaveGame)
	{
		if (!bWasSuccessful)
		{
			UE_LOG(LogMultiplayer, Warning, TEXT("Session destroy failed during leave; returning to menu anyway."));
		}
		FinishLeaveGame();
		return;
	}

	if (bWasSuccessful
		&& CompletedPurpose == EMultiplayerDestroyPurpose::RecreateSession)
	{
		CreateSession();
		return;
	}

	RollbackHostListener();
	EndSessionOperation();
	UE_LOG(LogMultiplayer, Error, TEXT("Existing session could not be destroyed before hosting."));
}

/**
 * 退出优先于当前菜单操作：取消连接与重试，解绑完成事件，再尝试销毁本地 Session。
 * bLeaveInProgress 跨越销毁和返回菜单两个阶段；重复退出直接返回，直到新 World 就绪才接受新操作。
 */
void UmultiplayerGameInstance::LeaveGame()
{
	if (bLeaveInProgress)
	{
		return;
	}

	bLeaveInProgress = true;
	CancelAutomaticReconnect();

	UWorld* World = GetWorld();
	if (GEngine != nullptr && World != nullptr)
	{
		// Leave 明确取代正在进行的连接；先关闭 PendingNetGame，避免旧 ClientTravel 稍后覆盖回菜单请求。
		GEngine->CancelPending(World);
	}
	if (World != nullptr && World->GetNetMode() == NM_ListenServer)
	{
		// 先让每个远端客户端主动清理自己的 Session；本地主机在下面完成相同清理。
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* PlayerController = It->Get();
			if (PlayerController != nullptr && !PlayerController->IsLocalController())
			{
				PlayerController->ClientReturnToMainMenuWithTextReason(
					NSLOCTEXT("Multiplayer", "HostLeftSession", "主机已退出房间"));
			}
		}
	}

	const bool bWasFinding =
		CurrentOperation == EMultiplayerSessionOperation::Finding;
	ClearSessionDelegates();
	if (bWasFinding && SessionInterface.IsValid())
	{
		// 解绑完成回调后再取消底层搜索，避免 CancelFindSessions 的同步完成通知重新进入离开流程。
		SessionInterface->CancelFindSessions();
	}
	SessionSearch.Reset();
	EndSessionOperation();

	if (SessionInterface.IsValid()
		&& SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
		BindDestroyDelegate(EMultiplayerDestroyPurpose::LeaveGame);
		const bool bRequestAccepted = SessionInterface->DestroySession(NAME_GameSession);
		if (bRequestAccepted || !DestroySessionCompleteHandle.IsValid())
		{
			return;
		}

		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
			DestroySessionCompleteHandle);
		DestroySessionCompleteHandle.Reset();
		DestroyPurpose = EMultiplayerDestroyPurpose::None;
		UE_LOG(LogMultiplayer, Warning, TEXT("DestroySession request was rejected during leave."));
	}

	FinishLeaveGame();
}

/** 清空房间搜索与重连资料并调用引擎返回菜单；保留退出标记，交给加载完成或加载失败事件释放。 */
void UmultiplayerGameInstance::FinishLeaveGame()
{
	DestroyPurpose = EMultiplayerDestroyPurpose::None;
	SessionSearch.Reset();
	CancelAutomaticReconnect();

	UE_LOG(LogMultiplayer, Log, TEXT("Local session cleaned; returning to the main menu."));
	// Session 清理完成后仍会经历 NetDriver 拆除和主菜单加载；退出互斥保持到新 World 就绪。
	ReturnToMainMenu();
}

/**
 * 只处理本实例的游戏网络错误；主动退出引起的断开无需重试。
 * 服务器的单个远端断开不销毁房间；允许重试的客户端错误安排重连，其余错误统一清理。
 */
void UmultiplayerGameInstance::HandleNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
	// GEngine 的失败事件会广播给同进程中的所有 GameInstance（例如多窗口 PIE）。
	// 只处理属于本实例 WorldContext 的错误，不能让另一个客户端清理本实例的会话或重连状态。
	if (!MultiplayerSession::IsFailureForGameInstance(this, World, NetDriver))
	{
		return;
	}

	if (bLeaveInProgress)
	{
		// 主动退出本来就会拆除 GameNetDriver；等待新 World 的 PostLoadMap 再释放退出互斥。
		return;
	}

	if (NetDriver != nullptr
		&& (FailureType == ENetworkFailure::ConnectionLost || FailureType == ENetworkFailure::ConnectionTimeout)
		&& (NetDriver->GetNetMode() == NM_ListenServer || NetDriver->GetNetMode() == NM_DedicatedServer))
	{
		// (**) 引擎也会向服务器报告单个客户端超时；监听仍正常，不能走全局失败清理删除房间广告。
		// 保留主机会话和正在进行的菜单操作，远端连接与 Pawn 的移除继续交给引擎处理。
		UE_LOG(LogMultiplayer, Warning, TEXT("Remote connection [%s]: %s. Host session retained."),
			ENetworkFailure::ToString(FailureType), *ErrorString);
		return;
	}

	if (CanRetryNetworkFailure(NetDriver, FailureType))
	{
		// 清掉中断中的会话回调，避免旧异步结果在重连期间继续改变状态。
		ClearSessionDelegates();
		SessionSearch.Reset();
		EndSessionOperation();
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Network [%s]: %s. Automatic reconnect will be attempted."),
			ENetworkFailure::ToString(FailureType),
			*ErrorString);
		ScheduleAutomaticReconnect();
		return;
	}

	RecordConnectionFailure(
		TEXT("Network"),
		ENetworkFailure::ToString(FailureType),
		ErrorString);
}

/**
 * 区分返回菜单失败、重连加载失败和普通加入失败。
 * 返回菜单失败需解除退出限制；重连阶段沿用剩余次数；其余情况清空连接资料并恢复菜单操作。
 */
void UmultiplayerGameInstance::HandleTravelFailure(
	UWorld* World,
	ETravelFailure::Type FailureType,
	const FString& ErrorString)
{
	// TravelFailure 同样是引擎全局事件，必须先按 World/GameInstance 隔离多 PIE 实例。
	if (World == nullptr || World->GetGameInstance() != this)
	{
		return;
	}

	if (bLeaveInProgress)
	{
		// 主菜单 Travel 失败时必须释放退出互斥，否则当前 World 中所有后续会话请求都会永久被拒。
		bLeaveInProgress = false;
		RecordConnectionFailure(
			TEXT("LeaveTravel"),
			ETravelFailure::ToString(FailureType),
			ErrorString);
		return;
	}

	if (AmultiplayerGameMode* CoopGameMode = World->GetAuthGameMode<AmultiplayerGameMode>())
	{
		// 旧 World 仍存活时，重开失败只恢复本次请求，不清除仍有效的主机会话。
		if (CoopGameMode->RecoverFailedRestart(ErrorString))
		{
			return;
		}
	}

	if (ReconnectState == EMultiplayerReconnectState::Connecting
		&& !LastConnectString.IsEmpty())
	{
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Reconnect travel [%s]: %s"),
			ETravelFailure::ToString(FailureType),
			*ErrorString);
		ScheduleAutomaticReconnect();
		return;
	}

	RecordConnectionFailure(
		TEXT("Travel"),
		ETravelFailure::ToString(FailureType),
		ErrorString);
}

/** 结束本地会话请求与重试并记录错误；此函数不恢复旧 Pawn，也不负责弹出蓝图错误窗口。 */
void UmultiplayerGameInstance::RecordConnectionFailure(
	const TCHAR* FailureSource,
	const FString& FailureType,
	const FString& ErrorString)
{
	// 不可恢复失败统一收口所有异步状态，避免菜单仍显示忙碌或旧回调在稍后覆盖失败结果。
	ClearSessionDelegates();
	SessionSearch.Reset();
	CancelAutomaticReconnect();
	RollbackHostListener();
	RemoveFailedLocalSession();
	EndSessionOperation();

	UE_LOG(
		LogMultiplayer,
		Error,
		TEXT("%s [%s]: %s"),
		FailureSource,
		*FailureType,
		*ErrorString);
}

/**
 * 只分类，不修改状态：客户端有已知地址时可重试断开和超时。
 * PendingConnectionFailure 仅在已开始的重连中继续重试，初次 Join 失败不会在此自动开启新一轮重连。
 */
bool UmultiplayerGameInstance::CanRetryNetworkFailure(
	UNetDriver* NetDriver,
	ENetworkFailure::Type FailureType) const
{
	if (GameplayConfig.ReconnectDelaysSeconds.IsEmpty()
		|| LastConnectString.IsEmpty()
		|| NetDriver == nullptr
		|| NetDriver->GetNetMode() != NM_Client)
	{
		return false;
	}

	if (FailureType == ENetworkFailure::ConnectionLost
		|| FailureType == ENetworkFailure::ConnectionTimeout)
	{
		// 只重试可能短暂恢复的断线和超时；版本不匹配、封禁等永久错误应立即失败。
		return true;
	}

	return ReconnectState == EMultiplayerReconnectState::Connecting
		&& FailureType == ENetworkFailure::PendingConnectionFailure;
}

/**
 * 安排一次性等待，Waiting 状态下的重复错误不会叠加定时器。
 * 配置的连接尝试耗尽后清除地址与本地会话记录；等待时间之外，连接本身仍遵循引擎的超时设置。
 */
void UmultiplayerGameInstance::ScheduleAutomaticReconnect()
{
	if (ReconnectState == EMultiplayerReconnectState::Waiting)
	{
		return;
	}

	if (ReconnectAttempt >= GameplayConfig.ReconnectDelaysSeconds.Num())
	{
		ReconnectState = EMultiplayerReconnectState::Idle;
		RemoveFailedLocalSession();
		LastConnectString.Reset();
		UE_LOG(
			LogMultiplayer,
			Error,
			TEXT("Automatic reconnect failed after %d attempts."),
			ReconnectAttempt);
		ReconnectAttempt = 0;
		return;
	}

	ReconnectState = EMultiplayerReconnectState::Waiting;
	// (*) 尝试次数直接等于间隔数量，避免“次数”和“数组长度”两份配置不一致导致越界。
	const float Delay = GameplayConfig.ReconnectDelaysSeconds[ReconnectAttempt];
	// 使用 GameInstance 的定时器，因为断线和地图切换期间旧 World 可能被销毁。
	GetTimerManager().SetTimer(
		ReconnectTimerHandle,
		this,
		&UmultiplayerGameInstance::TryAutomaticReconnect,
		Delay,
		false);

	UE_LOG(
		LogMultiplayer,
		Log,
		TEXT("Automatic reconnect attempt %d/%d scheduled in %.0f second(s)."),
		ReconnectAttempt + 1,
		GameplayConfig.ReconnectDelaysSeconds.Num(),
		Delay);
}

/**
 * 等待到期后按缓存地址直连原主机，不重新搜索房间，也不恢复上次的私人游戏进度。
 * 每次进入都计作一次尝试，包括暂时找不到本地控制器的情况，使失败路径也能有限结束。
 */
void UmultiplayerGameInstance::TryAutomaticReconnect()
{
	UWorld* World = GetWorld();
	APlayerController* PlayerController =
		World != nullptr ? World->GetFirstPlayerController() : nullptr;

	// 先推进状态和次数，再发起 Travel；同步失败回调也能看到一致的“第几次连接中”状态。
	ReconnectState = EMultiplayerReconnectState::Connecting;
	++ReconnectAttempt;
	if (PlayerController == nullptr)
	{
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Automatic reconnect attempt %d could not find a local player."),
			ReconnectAttempt);
		ScheduleAutomaticReconnect();
		return;
	}

	UE_LOG(
		LogMultiplayer,
		Log,
		TEXT("Automatic reconnect attempt %d/%d started."),
		ReconnectAttempt,
		GameplayConfig.ReconnectDelaysSeconds.Num());
	PlayerController->ClientTravel(LastConnectString, TRAVEL_Absolute);
}

/**
 * 由本地 PlayerController::BeginPlayingState 通知，作为本项目结束 Joining/重连等待的时机。
 * 该时机确认控制器进入游玩状态，不保证所有 Actor 的复制数据和界面绑定都已准备好。
 */
void UmultiplayerGameInstance::NotifyClientConnected()
{
	UWorld* World = GetWorld();

	// Listen Server 主机也会进入 PlayingState，但它不是需要重连的远端客户端，不能污染客户端地址状态。
	if (World == nullptr || World->GetNetMode() != NM_Client)
	{
		return;
	}

	if (LastConnectString.IsEmpty() && !World->URL.Host.IsEmpty())
	{
		// 直连进入的客户端没有 JoinSession 回调，从当前 World URL 补记可重连地址。
		LastConnectString = World->URL.Port > 0
			? FString::Printf(TEXT("%s:%d"), *World->URL.Host, World->URL.Port)
			: World->URL.Host;
	}

	GetTimerManager().ClearTimer(ReconnectTimerHandle);
	if (CurrentOperation == EMultiplayerSessionOperation::Joining)
	{
		// JoinSession 回调只完成会话层；进入 PlayingState 后才真正结束 Joining。
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Log, TEXT("Session connection established."));
	}

	if (ReconnectState == EMultiplayerReconnectState::Connecting
		|| ReconnectState == EMultiplayerReconnectState::Waiting)
	{
		// (*) 由本地控制器进入 PlayingState 确认这次连接；其他复制对象的就绪由各自组件处理。
		const int32 SuccessfulAttempt = ReconnectAttempt;
		ReconnectState = EMultiplayerReconnectState::Idle;
		ReconnectAttempt = 0;
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Automatic reconnect succeeded after %d attempt(s)."),
			SuccessfulAttempt);
	}
}

/** 清除等待定时器、次数和地址；用于接受新的菜单请求或退出，不负责取消已发出的 PendingNetGame。 */
void UmultiplayerGameInstance::CancelAutomaticReconnect()
{
	GetTimerManager().ClearTimer(ReconnectTimerHandle);
	ReconnectState = EMultiplayerReconnectState::Idle;
	ReconnectAttempt = 0;
	LastConnectString.Reset();
}

/**
 * 核对加载事件属于本实例；退出时再核对实际目的地确为默认菜单，随后释放退出限制。
 */
void UmultiplayerGameInstance::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (LoadedWorld == nullptr || LoadedWorld->GetGameInstance() != this)
	{
		return;
	}

	if (bLeaveInProgress)
	{
		const FString LoadedPackage = LoadedWorld->GetOutermost()->GetName();
		const FString ExpectedMenuPackage = FPackageName::ObjectPathToPackageName(
			UGameMapsSettings::GetGameDefaultMap());
		if (LoadedPackage != ExpectedMenuPackage)
		{
			bLeaveInProgress = false;
			RecordConnectionFailure(
				TEXT("LeaveTravel"),
				TEXT("UnexpectedDestination"),
				FString::Printf(
					TEXT("Expected %s but loaded %s."),
					*ExpectedMenuPackage,
					*LoadedPackage));
			return;
		}

		// 到这里旧 NetDriver 已经拆除、新 World 已可用，后续菜单请求才可以重新取得操作权。
		bLeaveInProgress = false;
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Leave transition completed in the main menu: %s"),
			*LoadedPackage);
	}
}

/**
 * 在游戏线程上的入口之间限制重复操作，不是跨线程锁。
 * 退出、重连正在连接或已有菜单请求时拒绝；重连仍在 Waiting 时允许新的有效菜单请求接管并取消等待。
 */
bool UmultiplayerGameInstance::BeginSessionOperation(
	EMultiplayerSessionOperation NewOperation)
{
	if (bLeaveInProgress)
	{
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Ignored session operation while leaving. Requested=%s"),
			*UEnum::GetValueAsString(NewOperation));
		return false;
	}

	if (ReconnectState == EMultiplayerReconnectState::Connecting)
	{
		// ClientTravel 已经发出时不能再开启会话操作，否则旧 PendingNetGame 仍可能稍后完成并覆盖新请求。
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Ignored session operation while reconnect travel is in flight. Requested=%s"),
			*UEnum::GetValueAsString(NewOperation));
		return false;
	}

	// (**) 异步状态机拒绝建房、搜索、加入互相覆盖，保证每个回调只结束自己的操作。
	if (CurrentOperation != EMultiplayerSessionOperation::None)
	{
		UE_LOG(
			LogMultiplayer,
			Warning,
			TEXT("Ignored overlapping session operation. Current=%s Requested=%s"),
			*UEnum::GetValueAsString(CurrentOperation),
			*UEnum::GetValueAsString(NewOperation));
		return false;
	}

	CurrentOperation = NewOperation;
	OnSessionOperationChanged.Broadcast(CurrentOperation);
	return true;
}

/** 仅在状态实际变化时广播 None，减少重复解锁通知；退出和重连的限制仍由各自状态决定。 */
void UmultiplayerGameInstance::EndSessionOperation()
{
	if (CurrentOperation == EMultiplayerSessionOperation::None)
	{
		return;
	}

	CurrentOperation = EMultiplayerSessionOperation::None;
	OnSessionOperationChanged.Broadcast(CurrentOperation);
}

/**
 * 当前 Null/LAN 失败路径只移除本进程保存的同名 Session，避免额外增加一次异步销毁流程。
 * RemoveNamedSession 不等于通知远端退出或关闭 NetDriver；替换在线服务时需要重新核对其清理要求。
 */
void UmultiplayerGameInstance::RemoveFailedLocalSession()
{
	if (SessionInterface.IsValid()
		&& SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
		// 当前项目使用 Null/LAN。连接已经失败时只需移除客户端本地记录，
		// 不再引入一套额外的异步销毁状态。
		SessionInterface->RemoveNamedSession(NAME_GameSession);
	}
}

/**
 * 失败、退出和 Shutdown 共用的订阅清理；接口存在时解绑，接口失效也要重置本地句柄。
 * 这里只取消接收回调，不自动取消子系统里的原始操作；例如搜索还需由 LeaveGame 调用 CancelFindSessions。
 */
void UmultiplayerGameInstance::ClearSessionDelegates()
{
	if (SessionInterface.IsValid())
	{
		if (CreateSessionCompleteHandle.IsValid())
		{
			// (**) CreateUObject 能避免对已销毁对象直接调用，但活着的 GameInstance 仍需主动解绑过期订阅。
			SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(
				CreateSessionCompleteHandle);
		}
		if (FindSessionsCompleteHandle.IsValid())
		{
			SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(
				FindSessionsCompleteHandle);
		}
		if (JoinSessionCompleteHandle.IsValid())
		{
			SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(
				JoinSessionCompleteHandle);
		}
		if (DestroySessionCompleteHandle.IsValid())
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
				DestroySessionCompleteHandle);
		}
	}

	CreateSessionCompleteHandle.Reset();
	FindSessionsCompleteHandle.Reset();
	JoinSessionCompleteHandle.Reset();
	DestroySessionCompleteHandle.Reset();
	DestroyPurpose = EMultiplayerDestroyPurpose::None;
}
