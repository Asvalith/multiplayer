// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameInstance.h"

#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "TimerManager.h"
#include "multiplayerLog.h"

namespace MultiplayerSession
{
	const FName ServerNameKey(TEXT("SERVER_NAME"));
	const FString GameplayMapPath(TEXT("/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo"));
	constexpr int32 MaxReconnectAttempts = 3;
	constexpr float ReconnectDelays[] = {1.0f, 2.0f, 4.0f};
}

void UmultiplayerGameInstance::Init()
{
	Super::Init();

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
}

void UmultiplayerGameInstance::Shutdown()
{
	// 先停计时器再解绑回调，避免 Shutdown 过程中新的重连尝试或测试事件重新进入本对象。
	GetTimerManager().ClearTimer(ReconnectTimerHandle);
#if !UE_BUILD_SHIPPING
	GetTimerManager().ClearTimer(ReconnectTestTimerHandle);
#endif

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
	ClearSessionDelegates();
	SessionSearch.Reset();
	SessionInterface.Reset();

	Super::Shutdown();
}

void UmultiplayerGameInstance::HostGame(
	const FString& ServerName,
	int32 PublicConnections,
	bool bIsLanMatch)
{
	CancelAutomaticReconnect();

	if (!SessionInterface.IsValid()
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Hosting))
	{
		return;
	}

	// 参数要跨越“销毁旧会话”的异步间隔，因此先保存到 GameInstance，而不是捕获临时局部变量。
	PendingServerName = ServerName.IsEmpty() ? TEXT("Coop Session") : ServerName;
	PendingPublicConnections = FMath::Max(2, PublicConnections);
	bPendingIsLanMatch = bIsLanMatch;

	if (SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
		// (**) 同名会话未销毁时直接 CreateSession 通常会失败，先异步销毁再继续创建。
		BindDestroyDelegate();

		if (!SessionInterface->DestroySession(NAME_GameSession))
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
				DestroySessionCompleteHandle);
			DestroySessionCompleteHandle.Reset();
			EndSessionOperation();
			UE_LOG(LogMultiplayer, Error, TEXT("Existing session could not be destroyed before hosting."));
		}
		return;
	}

	CreateSession();
}

void UmultiplayerGameInstance::CreateSession()
{
	// SessionSettings 描述的是会话如何被发现和加入，不会替代真正的 NetDriver 连接与地图 Travel。
	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = bPendingIsLanMatch;
	Settings.NumPublicConnections = PendingPublicConnections;
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	// LAN 使用局域网广播；在线模式才启用 Presence/Lobby，避免 Null 子系统下的无效配置。
	Settings.bAllowJoinViaPresence = !bPendingIsLanMatch;
	Settings.bUsesPresence = !bPendingIsLanMatch;
	Settings.bUseLobbiesIfAvailable = !bPendingIsLanMatch;

	// 地图名和服务器名作为可广播的会话元数据，搜索列表无需连接服务器就能展示摘要。
	Settings.Set(
		SETTING_MAPNAME,
		MultiplayerSession::GameplayMapPath,
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

	if (!SessionInterface->CreateSession(0, NAME_GameSession, Settings))
	{
		SessionInterface->ClearOnCreateSessionCompleteDelegate_Handle(
			CreateSessionCompleteHandle);
		CreateSessionCompleteHandle.Reset();
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("CreateSession request was rejected."));
	}
}

void UmultiplayerGameInstance::FindGames(int32 MaxResults, bool bIsLanQuery)
{
	CancelAutomaticReconnect();

	if (!SessionInterface.IsValid()
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Finding))
	{
		OnFindComplete.Broadcast(false, {});
		return;
	}

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

	if (!SessionInterface->FindSessions(0, SessionSearch.ToSharedRef()))
	{
		SessionInterface->ClearOnFindSessionsCompleteDelegate_Handle(
			FindSessionsCompleteHandle);
		FindSessionsCompleteHandle.Reset();
		SessionSearch.Reset();
		EndSessionOperation();
		OnFindComplete.Broadcast(false, {});
	}
}

void UmultiplayerGameInstance::JoinGame(int32 ResultIndex)
{
	CancelAutomaticReconnect();

	// 在发异步请求前验证搜索对象和下标，防止 UI 使用上一轮搜索留下的过期 ResultIndex。
	if (!SessionInterface.IsValid()
		|| !SessionSearch.IsValid()
		|| !SessionSearch->SearchResults.IsValidIndex(ResultIndex)
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Joining))
	{
		return;
	}

	JoinSessionCompleteHandle =
		SessionInterface->AddOnJoinSessionCompleteDelegate_Handle(
			FOnJoinSessionCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleJoinSessionComplete));

	if (!SessionInterface->JoinSession(
		0,
		NAME_GameSession,
		SessionSearch->SearchResults[ResultIndex]))
	{
		SessionInterface->ClearOnJoinSessionCompleteDelegate_Handle(
			JoinSessionCompleteHandle);
		JoinSessionCompleteHandle.Reset();
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("JoinSession request was rejected."));
	}
}

void UmultiplayerGameInstance::BindDestroyDelegate()
{
	if (DestroySessionCompleteHandle.IsValid())
	{
		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
			DestroySessionCompleteHandle);
	}

	DestroySessionCompleteHandle =
		SessionInterface->AddOnDestroySessionCompleteDelegate_Handle(
			FOnDestroySessionCompleteDelegate::CreateUObject(
				this,
				&UmultiplayerGameInstance::HandleDestroySessionComplete));
}

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
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("Session creation failed."));
		return;
	}

	UWorld* World = GetWorld();
	// 项目没有 GameInstance 蓝图配置层，玩法地图作为当前项目的固定入口集中放在本文件。
	// (*) ?listen 让主机切图后的 World 启动监听服务器，否则客户端无法连接该地图。
	const FString TravelUrl = MultiplayerSession::GameplayMapPath + TEXT("?listen");

	if (World == nullptr || !World->ServerTravel(TravelUrl))
	{
		EndSessionOperation();
		UE_LOG(LogMultiplayer, Error, TEXT("Session created, but ServerTravel failed."));
		return;
	}

	EndSessionOperation();
}

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
			Results.Add(MoveTemp(Info));
		}
	}

	EndSessionOperation();
	OnFindComplete.Broadcast(bWasSuccessful, Results);
}

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
	EndSessionOperation();

	if (bCanTravel)
	{
		// (*) JoinSession 只加入在线会话；还要解析真实地址并由本地控制器 ClientTravel。
		LastConnectString = ConnectString;
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
		return;
	}

	UE_LOG(LogMultiplayer, Error, TEXT("Session join completed without a usable connection address."));
}

void UmultiplayerGameInstance::HandleDestroySessionComplete(
	FName SessionName,
	bool bWasSuccessful)
{
	SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
		DestroySessionCompleteHandle);
	DestroySessionCompleteHandle.Reset();

	if (bWasSuccessful)
	{
		CreateSession();
		return;
	}

	EndSessionOperation();
	UE_LOG(LogMultiplayer, Error, TEXT("Existing session could not be destroyed before hosting."));
}

void UmultiplayerGameInstance::HandleNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
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

void UmultiplayerGameInstance::HandleTravelFailure(
	UWorld* World,
	ETravelFailure::Type FailureType,
	const FString& ErrorString)
{
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

void UmultiplayerGameInstance::RecordConnectionFailure(
	const TCHAR* FailureSource,
	const FString& FailureType,
	const FString& ErrorString)
{
	// 不可恢复失败统一收口所有异步状态，避免菜单仍显示忙碌或旧回调在稍后覆盖失败结果。
	ClearSessionDelegates();
	SessionSearch.Reset();
	EndSessionOperation();
	GetTimerManager().ClearTimer(ReconnectTimerHandle);

	if (ReconnectState == EMultiplayerReconnectState::Waiting
		|| ReconnectState == EMultiplayerReconnectState::Connecting)
	{
		ReconnectState = EMultiplayerReconnectState::Failed;
	}
	LastConnectString.Reset();

	UE_LOG(
		LogMultiplayer,
		Error,
		TEXT("%s [%s]: %s"),
		FailureSource,
		*FailureType,
		*ErrorString);
}

bool UmultiplayerGameInstance::CanRetryNetworkFailure(
	UNetDriver* NetDriver,
	ENetworkFailure::Type FailureType) const
{
	if (ReconnectState == EMultiplayerReconnectState::Failed
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

void UmultiplayerGameInstance::ScheduleAutomaticReconnect()
{
	if (ReconnectState == EMultiplayerReconnectState::Waiting)
	{
		return;
	}

	if (ReconnectAttempt >= MultiplayerSession::MaxReconnectAttempts)
	{
		ReconnectState = EMultiplayerReconnectState::Failed;
		LastConnectString.Reset();
		UE_LOG(
			LogMultiplayer,
			Error,
			TEXT("Automatic reconnect failed after %d attempts."),
			ReconnectAttempt);
		return;
	}

	ReconnectState = EMultiplayerReconnectState::Waiting;
	// (*) 采用 1/2/4 秒的有限退避，既给网络恢复时间，也避免无上限高频请求服务器。
	const float Delay = MultiplayerSession::ReconnectDelays[ReconnectAttempt];
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
		MultiplayerSession::MaxReconnectAttempts,
		Delay);
}

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
		MultiplayerSession::MaxReconnectAttempts);
	PlayerController->ClientTravel(LastConnectString, TRAVEL_Absolute);
}

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

	if (ReconnectState == EMultiplayerReconnectState::Connecting
		|| ReconnectState == EMultiplayerReconnectState::Waiting)
	{
		// (*) PlayingState 表示服务器已经接受玩家并创建控制器，比“发出连接请求”更可靠。
		const int32 SuccessfulAttempt = ReconnectAttempt;
		ReconnectState = EMultiplayerReconnectState::Succeeded;
		ReconnectAttempt = 0;
		UE_LOG(
			LogMultiplayer,
			Log,
			TEXT("Automatic reconnect succeeded after %d attempt(s)."),
			SuccessfulAttempt);
	}

#if !UE_BUILD_SHIPPING
	if (!bReconnectTestTriggered
		&& FParse::Param(FCommandLine::Get(), TEXT("CoopTestReconnect")))
	{
		bReconnectTestTriggered = true;
		GetTimerManager().SetTimer(
			ReconnectTestTimerHandle,
			this,
			&UmultiplayerGameInstance::SimulateConnectionLossForTesting,
			2.0f,
			false);
	}
#endif
}

void UmultiplayerGameInstance::CancelAutomaticReconnect()
{
	GetTimerManager().ClearTimer(ReconnectTimerHandle);
	ReconnectState = EMultiplayerReconnectState::Idle;
	ReconnectAttempt = 0;
	LastConnectString.Reset();
}

#if !UE_BUILD_SHIPPING
void UmultiplayerGameInstance::SimulateConnectionLossForTesting()
{
	UWorld* World = GetWorld();
	UNetDriver* NetDriver = World != nullptr ? World->GetNetDriver() : nullptr;
	if (GEngine == nullptr
		|| NetDriver == nullptr
		|| NetDriver->GetNetMode() != NM_Client)
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Reconnect test could not find the client net driver."));
		return;
	}

	UE_LOG(LogMultiplayer, Log, TEXT("Reconnect test: simulating connection loss."));
	GEngine->BroadcastNetworkFailure(
		World,
		NetDriver,
		ENetworkFailure::ConnectionLost,
		TEXT("Development reconnect test"));
}
#endif

bool UmultiplayerGameInstance::BeginSessionOperation(
	EMultiplayerSessionOperation NewOperation)
{
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

void UmultiplayerGameInstance::EndSessionOperation()
{
	if (CurrentOperation == EMultiplayerSessionOperation::None)
	{
		return;
	}

	CurrentOperation = EMultiplayerSessionOperation::None;
	OnSessionOperationChanged.Broadcast(CurrentOperation);
}

void UmultiplayerGameInstance::ClearSessionDelegates()
{
	if (!SessionInterface.IsValid())
	{
		return;
	}

	if (CreateSessionCompleteHandle.IsValid())
	{
		// (**) OnlineSubsystem 不会替对象管理这些句柄；退出或失败时必须逐个解绑。
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

	CreateSessionCompleteHandle.Reset();
	FindSessionsCompleteHandle.Reset();
	JoinSessionCompleteHandle.Reset();
	DestroySessionCompleteHandle.Reset();
}
