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
	Idle,
	Connecting,
	Connected,
	ReconnectWaiting,
	Reconnecting,
	Leaving
};
DECLARE_MULTICAST_DELEGATE(FMultiplayerConnectionChanged);
DECLARE_MULTICAST_DELEGATE_OneParam(FMultiplayerLeaveFailed, const FText&);

/**
 * DS 地址连接、取消和有限重连；不承担房间发现、账号登录或匹配。
 * 连接状态在进入游戏、重开和返回菜单期间保留；进入客户端 PlayingState 才确认成功。
 * 状态提交统一更新操作编号，通知后只检查本次操作是否被取消或替换。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// 连接请求与成功确认。拒绝请求不覆盖现有连接数据。
	// 仅接受 hostname/IPv4:port，不允许地图路径、URL 参数或 listen 选项混入。
	UFUNCTION(BlueprintCallable, Category = "Network")
	bool ConnectToServer(const FString& Address);

	UFUNCTION(BlueprintCallable, Category = "Network")
	void LeaveGame();

	// 由本地 PlayerController 进入 PlayingState 时调用，不以 Travel 发出作为成功依据。
	void NotifyClientConnected();

	// 只读状态供菜单展示，配置供玩法规则读取；地址校验不发起连接。
	EMultiplayerConnectionState GetConnectionState() const { return ConnectionState; }
	const FText& GetConnectionMessage() const { return ConnectionMessage; }
	const FmultiplayerGameplayConfig& GetGameplayConfig() const { return GameplayConfig; }
	static bool NormalizeServerAddress(const FString& Input, FString& Output);

	FMultiplayerConnectionChanged OnConnectionChanged;
	FMultiplayerLeaveFailed OnLeaveFailed;

	// 生命周期：读取配置、绑定全局事件；结束时清理计时器与订阅。
	virtual void Init() override;
	virtual void Shutdown() override;

private:
	// 连接执行与重试流程。
	void StartTravel(bool bRetry);
	void ScheduleReconnect();
	void TryReconnect();
	void HandleConnectTimeout(uint64 Revision);
	void FailConnection(const FString& Reason);

	// 引擎事件入口：先核对事件归属，再推进本实例状态。
	void HandlePostLoadMap(UWorld* World);
	void HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type Type, const FString& Error);
	bool OwnsFailure(const UWorld* World, const UNetDriver* Driver) const;

	// 状态提交与公共清理。
	uint64 SetConnectionState(EMultiplayerConnectionState State, const FText& Message);
	void ClearConnectionTimers();

	FmultiplayerGameplayConfig GameplayConfig;
	EMultiplayerConnectionState ConnectionState = EMultiplayerConnectionState::Idle;
	FText ConnectionMessage;
	FString LastServerAddress;
	int32 ReconnectAttempt = 0;
	// 仅由 SetConnectionState 递增，使旧回调和通知中被替换的操作失效。
	uint64 OperationRevision = 0;
	FTimerHandle ConnectTimeoutHandle;
	FTimerHandle ReconnectTimerHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadMapHandle;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopConnectionStateTest;
#endif
};
