// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "multiplayerGameInstance.generated.h"

UENUM(BlueprintType)
enum class EMultiplayerSessionOperation : uint8
{
	None,
	Hosting,
	Finding,
	Joining,
	Destroying
};

UENUM(BlueprintType)
enum class EMultiplayerSessionMap : uint8
{
	ThirdPersonMap UMETA(DisplayName = "Third Person Map"),
	DesertCityExample UMETA(DisplayName = "Desert City Example")
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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerSessionOperationEvent,
	bool,
	bWasSuccessful);

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

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FmultiplayerConnectionFailureEvent,
	FString,
	FailureSource,
	FString,
	FailureType,
	FString,
	ErrorMessage);

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

	/** Selects the gameplay map used by the next hosted session. */
	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void SelectSessionMap(EMultiplayerSessionMap NewMap);

	UFUNCTION(BlueprintPure, Category = "Network|Session")
	EMultiplayerSessionMap GetSelectedSessionMap() const
	{
		return SelectedSessionMap;
	}

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void FindGames(int32 MaxResults, bool bIsLanQuery);

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void JoinGame(int32 ResultIndex);

	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void DestroyGameSession();

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

	UFUNCTION(BlueprintPure, Category = "Network|Diagnostics")
	const FString& GetLastConnectionError() const
	{
		return LastConnectionError;
	}

	UFUNCTION(BlueprintCallable, Category = "Network|Diagnostics")
	void ClearLastConnectionError();

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationEvent OnHostComplete;

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionSearchEvent OnFindComplete;

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationEvent OnJoinComplete;

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationEvent OnDestroyComplete;

	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationChanged OnSessionOperationChanged;

	/** Network and map travel failures are surfaced to UI instead of failing silently. */
	UPROPERTY(BlueprintAssignable, Category = "Network|Diagnostics")
	FmultiplayerConnectionFailureEvent OnConnectionFailure;

protected:
	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Network|Session")
	FString SessionMapPath = TEXT("/Script/Engine.World'/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo.Stylized_Egypt_Demo'");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|Session|Maps")
	FString ThirdPersonMapPath = TEXT("/Game/ThirdPerson/Maps/ThirdPersonMap");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Network|Session|Maps")
	FString DesertCityMapPath = TEXT("/Script/Engine.World'/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo.Stylized_Egypt_Demo'");

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Network|Session|Maps")
	EMultiplayerSessionMap SelectedSessionMap = EMultiplayerSessionMap::DesertCityExample;

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

	IOnlineSessionPtr SessionInterface;
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;

	FString PendingServerName = TEXT("Coop Session");
	int32 PendingPublicConnections = 4;
	bool bPendingIsLanMatch = true;
	bool bCreateSessionAfterDestroy = false;
	EMultiplayerSessionOperation CurrentOperation = EMultiplayerSessionOperation::None;
	FString LastConnectionError;
};
