#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "Core/multiplayerGameplayConfig.h"
#include "multiplayerGameInstance.generated.h"

class AmultiplayerCoopPlayerController;
class UNetConnection;

// 业务等待的有界估计值；采样来自 UE 已有 ACK RTT，不接管可靠包重传或服务器接纳期限。
struct MULTIPLAYER_API FmultiplayerConnectionTiming
{
	// 新 ACK 批次均值的平滑结果与变化量，单位为秒；不是加载或整个握手的耗时。
	double SmoothedRTT = 0.0;
	double RTTVariation = 0.0;
	int32 SampleCount = 0;

	// 忽略无效测量；变化量按旧平滑值计算，再更新本次平滑 RTT。
	void ObserveRTT(double RTTSeconds);
	// 无样本使用 1 秒；已测量估计限制在 0.25～5 秒。
	double GetRTOSeconds() const;
	// 20 秒加载/业务预算之外增加退避余量，总等待上限 60 秒。
	double GetConnectTimeoutSeconds(int32 Attempt) const;
	// 配置间隔与 RTO 指数退避取较大值，不再把配置间隔重复乘指数。
	double GetReconnectDelaySeconds(double ConfiguredDelay, int32 Attempt) const;
};

/** [职责] 本机连接业务状态；DS 不执行玩家菜单退出与自动重连。 */
UENUM(BlueprintType)
enum class EMultiplayerConnectionState : uint8
{
	// 无进行中的连接操作；接受新请求还要求当前 World 为 Standalone。
	Idle,
	// 首次连接已发起，尚未收到服务器接纳及个人状态恢复完成的确认。
	Connecting,
	// 已进入客户端 PlayingState，不表示所有复制对象和界面都已就绪。
	// 还必须已收到服务器会话接纳/恢复完成回执。
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
 * [职责] 跨 World 保留本实例的 DS 连接意图、有限重试和端点票据。
 * [边界] 不承担房间发现、账号登录或匹配，也不持有服务器个人战斗快照。
 * 连接状态在进入游戏、重开和返回菜单期间保留；PlayingState 发起握手，服务器接纳后才确认成功。
 * 状态提交统一更新操作编号，通知后只检查本次操作是否被取消或替换。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
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

	// 本地控制器收到服务器接纳后提交连接成功；Travel 发出或进入 PlayingState 本身不算成功。
	void NotifyClientConnected();
	// Owned Controller 在 PlayingState 准备握手，只有同一操作与同一控制器的服务器结果可以提交成功。
	bool PrepareCombatHandshake(AmultiplayerCoopPlayerController* Controller, FGuid& OutTicket, uint64& OutOperation);
	// 消费当前握手回执；成功只记录票据，由控制器接着提交 Connected 状态。
	bool CompleteCombatHandshake(AmultiplayerCoopPlayerController* Controller, uint64 Operation,
		const FGuid& Ticket, const FText& Error, bool bRetryable);
	// 仅接受当前已接纳的所属控制器更新端点票据，不持久化个人战斗数值。
	void SetReconnectTicket(AmultiplayerCoopPlayerController* Controller, const FGuid& Ticket);

	// 只读状态供菜单展示，配置供玩法规则读取；地址校验不发起连接。
	// 返回本实例的业务状态，不直接等同于底层 NetConnection 状态。
	UFUNCTION(BlueprintPure, Category = "Network")
	EMultiplayerConnectionState GetConnectionState() const { return ConnectionState; }

	// 返回当前连接提示或失败原因，供本地界面显示。
	UFUNCTION(BlueprintPure, Category = "Network")
	FText GetConnectionMessage() const { return ConnectionMessage; }

	// 返回本实例已加载的配置，调用者不能通过此引用修改规则。
	const FmultiplayerGameplayConfig& GetGameplayConfig() const { return GameplayConfig; }
	// 只读诊断估计，不开放外部改写网络样本或连接预算。
	const FmultiplayerConnectionTiming& GetConnectionTiming() const { return ConnectionTiming; }
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
	// 停止等待和 RTT 采样，不直接关闭底层连接；同端点重连保留已有估计。
	void ClearConnectionTimers();
	// Connected 后低频读取当前连接的新 ACK 批次，统计周期或连接变化时重置采样游标。
	void SampleConnectionTiming();
	// 更换端点、主动退出、最终失败或 Shutdown 时放弃上一端点的估计。
	void ResetConnectionTiming();

	// 本实例读取的配置；服务器玩法规则以服务器自己加载的配置为准。
	FmultiplayerGameplayConfig GameplayConfig;
	// 本地连接业务状态与显示文本，不复制给其他客户端。
	EMultiplayerConnectionState ConnectionState = EMultiplayerConnectionState::Idle;
	FText ConnectionMessage;
	// 首次请求或实际客户端 URL 记下的地址；主动退出和最终失败会清空。
	FString LastServerAddress;
	// 本地内存按规范端点保存不透明票据，不拼入URL、不写日志，也不保存客户端战斗数值。
	// [边界] 票据只是恢复凭据；恢复时限、资格和实际快照由服务器 GameMode 裁决。
	TMap<FString, FGuid> ReconnectTickets;
	TWeakObjectPtr<AmultiplayerCoopPlayerController> HandshakeController;
	// 已经开始的重连次数，用作下一次等待间隔的下标。
	int32 ReconnectAttempt = 0;
	// 状态提交或 Shutdown 使编号递增；超时、重试 Timer、握手回执都绑定本次编号。
	// 旧 World/旧 Controller 的通知不能接管新连接，广播后也必须复核是否被同步回调替换。
	uint64 OperationRevision = 0;
	// 一次连接尝试的等待期限，与重试前的等待分别管理。
	FTimerHandle ConnectTimeoutHandle;
	FTimerHandle ReconnectTimerHandle;
	FTimerHandle ConnectionTimingTimer;
	FmultiplayerConnectionTiming ConnectionTiming;
	TWeakObjectPtr<UNetConnection> TimingConnection;
	double TimingStatUpdateTime = -1.0;
	double TimingLagAcc = 0.0;
	int32 TimingLagCount = 0;
	// 引擎全局委托的订阅句柄，Shutdown 时用于对称解绑。
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadMapHandle;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopConnectionStateTest;
#endif
};
