#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "Core/multiplayerGameplayConfig.h"
#include "multiplayerGameInstance.generated.h"

/** 客户端连接生命周期。服务器不参与菜单状态，不因某名玩家退出而停止。 */
UENUM(BlueprintType)
enum class EMultiplayerConnectionState : uint8
{
	Idle, Connecting, Connected, ReconnectWaiting, Reconnecting, Leaving
};
DECLARE_MULTICAST_DELEGATE(FMultiplayerConnectionChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FMultiplayerLeaveFailed, const FText&);

/**
 * DS 地址连接、取消和有限重连；不承担房间发现、账号登录或匹配。
 * 状态跨地图保存在 GameInstance，进入客户端 PlayingState 才确认成功。
 * 先提交状态再通知；通知可能同步触发退出，Travel 前再次核对操作编号。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()
public:
	virtual void Init() override;
	virtual void Shutdown() override;
	const FmultiplayerGameplayConfig& GetGameplayConfig() const { return GameplayConfig; }

	// 仅接受 hostname/IPv4:port，不允许地图路径、URL 参数或 listen 选项混入。
	UFUNCTION(BlueprintCallable, Category="Network")
	bool ConnectToServer(const FString& Address);
	UFUNCTION(BlueprintCallable, Category="Network")
	void LeaveGame();
	void NotifyClientConnected();
	EMultiplayerConnectionState GetConnectionState() const { return ConnectionState; }
	const FText& GetConnectionMessage() const { return ConnectionMessage; }
	static bool NormalizeServerAddress(const FString& Input, FString& Output);
	FMultiplayerConnectionChanged OnConnectionChanged;
	FMultiplayerLeaveFailed OnLeaveFailed;

private:
	void SetConnectionState(EMultiplayerConnectionState State, const FText& Message);
	void ClearConnectionTimers();
	void StartTravel(bool bRetry);
	void HandleConnectTimeout(uint64 Revision);
	void ScheduleReconnect();
	void TryReconnect();
	void FailConnection(const FString& Reason);
	void HandlePostLoadMap(UWorld* World);
	void HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type Type, const FString& Error);
	bool OwnsFailure(const UWorld* World, const UNetDriver* Driver) const;

	FmultiplayerGameplayConfig GameplayConfig;
	EMultiplayerConnectionState ConnectionState = EMultiplayerConnectionState::Idle;
	FText ConnectionMessage;
	FString LastServerAddress;
	int32 ReconnectAttempt = 0;
	uint64 OperationRevision = 0;
	FTimerHandle ConnectTimeoutHandle, ReconnectTimerHandle;
	FDelegateHandle NetworkFailureHandle, TravelFailureHandle, PostLoadMapHandle;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopConnectionStateTest;
#endif
};
