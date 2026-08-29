// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/TimerHandle.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "multiplayerGameInstance.generated.h"

UENUM(BlueprintType)
enum class EMultiplayerSessionOperation : uint8
{
	None,
	Hosting,
	Finding,
	Joining
};

UENUM(BlueprintType)
enum class EMultiplayerReconnectState : uint8
{
	Idle,
	Waiting,
	Connecting,
	Succeeded,
	Failed
};

USTRUCT(BlueprintType)
struct FmultiplayerSessionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 ResultIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	FString ServerName;

	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 PingInMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 CurrentPlayers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 MaxPlayers = 0;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerSessionSearchEvent,
	bool,
	bWasSuccessful,
	const TArray<FmultiplayerSessionInfo>&,
	Results);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerSessionOperationChanged,
	EMultiplayerSessionOperation,
	NewOperation);

/**
 * Owns the online session lifecycle across map travel.
 * Menus call the public functions and react to the BlueprintAssignable events.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void HostGame(const FString& ServerName, int32 PublicConnections, bool bIsLanMatch);

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void FindGames(int32 MaxResults, bool bIsLanQuery);

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void JoinGame(int32 ResultIndex);

	/** Called by the local PlayerController after a client connection becomes playable. */
	void NotifyClientConnected();

	/** Stops pending retries before an intentional leave or a new connection request. */
	UFUNCTION(BlueprintCallable, Category = "Network|Reconnect")
	void CancelAutomaticReconnect();

	UFUNCTION(BlueprintPure, Category = "Network|Reconnect")
	EMultiplayerReconnectState GetReconnectState() const
	{
		return ReconnectState;
	}

	UFUNCTION(BlueprintPure, Category = "Network|Session")
	bool IsSessionOperationInProgress() const
	{
		return CurrentOperation != EMultiplayerSessionOperation::None;
	}

	UFUNCTION(BlueprintPure, Category = "Network|Session")
	EMultiplayerSessionOperation GetCurrentSessionOperation() const
	{
		return CurrentOperation;
	}

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionSearchEvent OnFindComplete;

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationChanged OnSessionOperationChanged;

protected:
	/** Fixed gameplay map opened by a successfully hosted session. */
	UPROPERTY(EditDefaultsOnly, Category = "Network|Session")
	FString SessionMapPath = TEXT("/Script/Engine.World'/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo.Stylized_Egypt_Demo'");

private:
	void CreateSession();
	void BindDestroyDelegate();
	void ClearSessionDelegates();
	bool BeginSessionOperation(EMultiplayerSessionOperation NewOperation);
	void EndSessionOperation();

	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleNetworkFailure(
		UWorld* World,
		UNetDriver* NetDriver,
		ENetworkFailure::Type FailureType,
		const FString& ErrorString);
	void HandleTravelFailure(
		UWorld* World,
		ETravelFailure::Type FailureType,
		const FString& ErrorString);
	void RecordConnectionFailure(
		const TCHAR* FailureSource,
		const FString& FailureType,
		const FString& ErrorString);
	bool CanRetryNetworkFailure(
		UNetDriver* NetDriver,
		ENetworkFailure::Type FailureType) const;
	void ScheduleAutomaticReconnect();
	void TryAutomaticReconnect();

#if !UE_BUILD_SHIPPING
	void SimulateConnectionLossForTesting();
#endif

	IOnlineSessionPtr SessionInterface;
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;

	FString PendingServerName = TEXT("Coop Session");
	FString LastConnectString;
	int32 PendingPublicConnections = 4;
	bool bPendingIsLanMatch = true;
	EMultiplayerSessionOperation CurrentOperation = EMultiplayerSessionOperation::None;
	EMultiplayerReconnectState ReconnectState = EMultiplayerReconnectState::Idle;
	FTimerHandle ReconnectTimerHandle;
	int32 ReconnectAttempt = 0;

#if !UE_BUILD_SHIPPING
	FTimerHandle ReconnectTestTimerHandle;
	bool bReconnectTestTriggered = false;
#endif
};
