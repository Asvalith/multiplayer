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
#include "UObject/SoftObjectPath.h"

namespace MultiplayerSession
{
	const FName ServerNameKey(TEXT("SERVER_NAME"));
	constexpr int32 MaxReconnectAttempts = 3;
	constexpr float ReconnectDelays[] = {1.0f, 2.0f, 4.0f};
}

void UmultiplayerGameInstance::Init()
{
	Super::Init();

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

	PendingServerName = ServerName.IsEmpty() ? TEXT("Coop Session") : ServerName;
	PendingPublicConnections = FMath::Max(2, PublicConnections);
	bPendingIsLanMatch = bIsLanMatch;

	if (SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
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
	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = bPendingIsLanMatch;
	Settings.NumPublicConnections = PendingPublicConnections;
	Settings.NumPrivateConnections = 0;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bAllowJoinViaPresence = !bPendingIsLanMatch;
	Settings.bUsesPresence = !bPendingIsLanMatch;
	Settings.bUseLobbiesIfAvailable = !bPendingIsLanMatch;

	Settings.Set(
		SETTING_MAPNAME,
		SessionMapPath,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(
		MultiplayerSession::ServerNameKey,
		PendingServerName,
		EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

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

	SessionSearch = MakeShared<FOnlineSessionSearch>();
	SessionSearch->MaxSearchResults = FMath::Max(1, MaxResults);
	SessionSearch->bIsLanQuery = bIsLanQuery;

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
	const FString TravelMapPath = FSoftObjectPath(SessionMapPath).GetLongPackageName();
	const FString TravelUrl = (TravelMapPath.IsEmpty() ? SessionMapPath : TravelMapPath) + TEXT("?listen");

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

	FString ConnectString;
	const bool bResolved = Result == EOnJoinSessionCompleteResult::Success
		&& SessionInterface->GetResolvedConnectString(SessionName, ConnectString);

	UWorld* World = GetWorld();
	APlayerController* PlayerController =
		World != nullptr ? World->GetFirstPlayerController() : nullptr;

	const bool bCanTravel = bResolved && PlayerController != nullptr;
	EndSessionOperation();

	if (bCanTravel)
	{
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
	const float Delay = MultiplayerSession::ReconnectDelays[ReconnectAttempt];
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
	if (World == nullptr || World->GetNetMode() != NM_Client)
	{
		return;
	}

	if (LastConnectString.IsEmpty() && !World->URL.Host.IsEmpty())
	{
		LastConnectString = World->URL.Port > 0
			? FString::Printf(TEXT("%s:%d"), *World->URL.Host, World->URL.Port)
			: World->URL.Host;
	}

	GetTimerManager().ClearTimer(ReconnectTimerHandle);

	if (ReconnectState == EMultiplayerReconnectState::Connecting
		|| ReconnectState == EMultiplayerReconnectState::Waiting)
	{
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
