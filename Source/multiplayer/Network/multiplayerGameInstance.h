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
	// 无进行中的连接操作；接受新请求还要求当前 World 为 Standalone。
	Idle,
	// 首次连接已发起，尚未收到本地 PlayingState 确认。
	Connecting,
	// 已进入客户端 PlayingState，不表示所有复制对象和界面都已就绪。
	Connected,
	// 断线后保留地址，等待下一次有限重试。
	ReconnectWaiting,
	// 正在执行一次重连，仍受连接超时和剩余次数限制。
	Reconnecting,
	// 主动取消或退出，停止自动重连并等待返回本地世界。
	Leaving
};
// 本地连接状态通知；监听者通过只读接口获取最新状态与提示，不经过网络。
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FMultiplayerConnectionChanged);
// 本地退出失败通知，用于恢复 Controller 的操作状态，不恢复旧网络连接。
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
	// 生命周期：读取配置、绑定全局事件；结束时清理计时器与订阅。
	// 实例初始化时加载配置并订阅引擎通知，不在这里发起玩家连接。
	virtual void Init() override;
	// 实例结束时取消等待并解除订阅，与 Init 对称。
	virtual void Shutdown() override;

	// 连接请求与成功确认。拒绝请求不覆盖现有连接数据。
	// 仅接受 hostname/IPv4:port，不允许地图路径、URL 参数或 listen 选项混入。
	UFUNCTION(BlueprintCallable, Category = "Network")
	bool ConnectToServer(const FString& Address);

	// 取消当前连接意图或退出游戏连接，由引擎返回本地菜单。
	UFUNCTION(BlueprintCallable, Category = "Network")
	void LeaveGame();

	// 由本地 PlayerController 进入 PlayingState 时调用，不以 Travel 发出作为成功依据。
	void NotifyClientConnected();

	// 只读状态供菜单展示，配置供玩法规则读取；地址校验不发起连接。
	// 返回本实例的业务状态，不直接等同于底层 NetConnection 状态。
	UFUNCTION(BlueprintPure, Category = "Network")
	EMultiplayerConnectionState GetConnectionState() const { return ConnectionState; }

	// 返回当前连接提示或失败原因，供本地界面显示。
	UFUNCTION(BlueprintPure, Category = "Network")
	FText GetConnectionMessage() const { return ConnectionMessage; }

	// 返回本进程已加载的配置，调用者不能通过此引用修改规则。
	const FmultiplayerGameplayConfig& GetGameplayConfig() const { return GameplayConfig; }
	// 独立校验地址格式并生成规范输出，不尝试连接服务器。
	static bool NormalizeServerAddress(const FString& Input, FString& Output);

	// 蓝图可订阅的本地通知；状态与提示已在广播前写入。
	UPROPERTY(BlueprintAssignable, Category = "Network")
	FMultiplayerConnectionChanged OnConnectionChanged;
	// 返回菜单失败时通知本地 Controller 解锁退出操作。
	FMultiplayerLeaveFailed OnLeaveFailed;

private:
	// 连接执行与重试流程。
	// 执行一次 Travel；bRetry 区分首次请求和重试时的失败处理。
	void StartTravel(bool bRetry);
	// 根据地址、次数和配置安排下一次等待，预算耗尽则结束请求。
	void ScheduleReconnect();
	// 等待 Timer 到期时累计一次尝试，并调用 StartTravel。
	void TryReconnect();
	// 只处理编号仍匹配的连接超时，避免旧尝试影响新状态。
	void HandleConnectTimeout(uint64 Revision);
	// 清理业务连接数据并回到 Idle；底层连接和地图仍由 UE 清理。
	void FailConnection(const FString& Reason);

	// 引擎事件入口：先核对事件归属，再推进本实例状态。
	// 自己的 Standalone World 加载完成时结束 Leaving，不用于确认联网成功。
	void HandlePostLoadMap(UWorld* World);
	// 根据错误归属、运行端和当前阶段，决定有限重连或结束请求。
	void HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error);
	// 处理重开恢复、重连跳转失败以及退出失败的本地反馈。
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type Type, const FString& Error);
	// 同时检查当前 World 和 PendingNetGame，避免全局失败事件影响其他实例。
	bool OwnsFailure(const UWorld* World, const UNetDriver* Driver) const;

	// 状态提交与公共清理。
	// 先提交状态和编号，再同步广播；返回本次编号供调用者检查是否被接管。
	uint64 SetConnectionState(EMultiplayerConnectionState State, const FText& Message);
	// 取消两种等待计时器，不直接关闭已经建立的网络连接。
	void ClearConnectionTimers();

	// 本进程读取的配置；服务器玩法规则以服务器自己加载的配置为准。
	FmultiplayerGameplayConfig GameplayConfig;
	// 本地连接业务状态与显示文本，不复制给其他客户端。
	EMultiplayerConnectionState ConnectionState = EMultiplayerConnectionState::Idle;
	FText ConnectionMessage;
	// 首次请求或实际客户端 URL 记下的地址；主动退出和最终失败会清空。
	FString LastServerAddress;
	// 已经开始的重连次数，用作下一次等待间隔的下标。
	int32 ReconnectAttempt = 0;
	// 仅由 SetConnectionState 递增，使旧回调和通知中被替换的操作失效。
	// 超时和状态通知后显式校验编号；重试 Timer 则检查当前等待状态。
	uint64 OperationRevision = 0;
	// 一次连接尝试的等待期限，与重试前的等待分别管理。
	FTimerHandle ConnectTimeoutHandle;
	FTimerHandle ReconnectTimerHandle;
	// 引擎全局委托的订阅句柄，Shutdown 时用于对称解绑。
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadMapHandle;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopConnectionStateTest;
#endif
};
