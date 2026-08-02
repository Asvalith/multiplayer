// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerGameInstance.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"

namespace MultiplayerSession
{
	const FName ServerNameKey(TEXT("SERVER_NAME"));
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
			LogTemp,
			Log,
			TEXT("Session subsystem initialized: %s"),
			*OnlineSubsystem->GetSubsystemName().ToString());
	}

	if (!SessionInterface.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Online session interface is unavailable."));
	}
}

void UmultiplayerGameInstance::Shutdown()
{
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

void UmultiplayerGameInstance::ClearLastConnectionError()
{
	LastConnectionError.Reset();
}

void UmultiplayerGameInstance::HostGame(
	const FString& ServerName,
	int32 PublicConnections,
	bool bIsLanMatch)
{
	if (!SessionInterface.IsValid()
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Hosting))
	{
		OnHostComplete.Broadcast(false);
		return;
	}

	PendingServerName = ServerName.IsEmpty() ? TEXT("Coop Session") : ServerName;
	PendingPublicConnections = FMath::Max(2, PublicConnections);
	bPendingIsLanMatch = bIsLanMatch;

	if (SessionInterface->GetNamedSession(NAME_GameSession) != nullptr)
	{
		bCreateSessionAfterDestroy = true;
		BindDestroyDelegate();

		if (!SessionInterface->DestroySession(NAME_GameSession))
		{
			SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
				DestroySessionCompleteHandle);
			DestroySessionCompleteHandle.Reset();
			bCreateSessionAfterDestroy = false;
			EndSessionOperation();
			OnHostComplete.Broadcast(false);
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
		OnHostComplete.Broadcast(false);
	}
}

void UmultiplayerGameInstance::FindGames(int32 MaxResults, bool bIsLanQuery)
{
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
	if (!SessionInterface.IsValid()
		|| !SessionSearch.IsValid()
		|| !SessionSearch->SearchResults.IsValidIndex(ResultIndex)
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Joining))
	{
		OnJoinComplete.Broadcast(false);
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
		OnJoinComplete.Broadcast(false);
	}
}

void UmultiplayerGameInstance::DestroyGameSession()
{
	if (!SessionInterface.IsValid()
		|| !BeginSessionOperation(EMultiplayerSessionOperation::Destroying))
	{
		OnDestroyComplete.Broadcast(false);
		return;
	}

	bCreateSessionAfterDestroy = false;

	if (SessionInterface->GetNamedSession(NAME_GameSession) == nullptr)
	{
		EndSessionOperation();
		OnDestroyComplete.Broadcast(true);
		return;
	}

	BindDestroyDelegate();
	if (!SessionInterface->DestroySession(NAME_GameSession))
	{
		SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
			DestroySessionCompleteHandle);
		DestroySessionCompleteHandle.Reset();
		EndSessionOperation();
		OnDestroyComplete.Broadcast(false);
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
	EndSessionOperation();

	OnHostComplete.Broadcast(bWasSuccessful);

	if (!bWasSuccessful)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (World == nullptr || !World->ServerTravel(SessionMapPath + TEXT("?listen")))
	{
		UE_LOG(LogTemp, Error, TEXT("Session created, but ServerTravel failed."));
	}
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
	OnJoinComplete.Broadcast(bCanTravel);

	if (bCanTravel)
	{
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
	}
}

void UmultiplayerGameInstance::HandleDestroySessionComplete(
	FName SessionName,
	bool bWasSuccessful)
{
	SessionInterface->ClearOnDestroySessionCompleteDelegate_Handle(
		DestroySessionCompleteHandle);
	DestroySessionCompleteHandle.Reset();

	const bool bShouldCreate = bCreateSessionAfterDestroy;
	bCreateSessionAfterDestroy = false;

	if (bShouldCreate)
	{
		if (bWasSuccessful)
		{
			CreateSession();
		}
		else
		{
			EndSessionOperation();
			OnHostComplete.Broadcast(false);
		}
		return;
	}

	EndSessionOperation();
	OnDestroyComplete.Broadcast(bWasSuccessful);
}

void UmultiplayerGameInstance::HandleNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
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
	bCreateSessionAfterDestroy = false;
	EndSessionOperation();

	LastConnectionError = FString::Printf(
		TEXT("%s [%s]: %s"),
		FailureSource,
		*FailureType,
		*ErrorString);

	UE_LOG(LogTemp, Error, TEXT("%s"), *LastConnectionError);
	OnConnectionFailure.Broadcast(FailureSource, FailureType, ErrorString);
}

bool UmultiplayerGameInstance::BeginSessionOperation(
	EMultiplayerSessionOperation NewOperation)
{
	if (CurrentOperation != EMultiplayerSessionOperation::None)
	{
		UE_LOG(
			LogTemp,
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
