// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/TimerHandle.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "multiplayerGameInstance.generated.h"

/**
 * 菜单层可观察的会话异步操作。
 *
 * WBPmainmenu 使用该状态禁用重复按钮请求。Hosting/Finding 等待 OnlineSubsystem 回调；
 * Joining 还会覆盖后续 ClientTravel，直到客户端真正进入服务器或收到失败事件。
 */
UENUM(BlueprintType)
enum class EMultiplayerSessionOperation : uint8
{
	// 当前没有建房、搜索或加入流程；退出和重连还会由各自状态单独限制请求。
	None,
	// 正在销毁旧会话或创建新会话；二者属于同一次建房流程。
	Hosting,
	// 正在等待局域网搜索结果。
	Finding,
	// 正在加入选中的搜索结果，直到客户端真正进入服务器。
	Joining
};

/**
 * 仅供 C++ 自动重连流程使用的内部状态。
 *
 * 它没有暴露给蓝图，因为当前菜单没有对应的重连交互；保留显式状态是为了区分定时等待、
 * 已经发起 ClientTravel 和真正进入 PlayingState，避免把“发出连接请求”误当成“重连成功”。
 */
enum class EMultiplayerReconnectState : uint8
{
	// 没有重连任务。
	Idle,
	// 已安排有限退避定时器，尚未发起下一次连接。
	Waiting,
	// 已调用 ClientTravel，等待本地 PlayerController 进入 PlayingState。
	Connecting
};

/** 同一个 DestroySession 回调可能服务于重新建房或主动退出，必须显式区分后续动作。 */
enum class EMultiplayerDestroyPurpose : uint8
{
	None,
	RecreateSession,
	LeaveGame
};

/** 搜索结果的蓝图安全摘要；真正的 FOnlineSessionSearchResult 仍由 GameInstance 持有。 */
USTRUCT(BlueprintType)
struct FmultiplayerSessionInfo
{
	GENERATED_BODY()

	// 对应当前 SessionSearch->SearchResults 的下标，只能用于本轮搜索结果。
	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 ResultIndex = INDEX_NONE;

	// 主机写入会话设置的显示名称；缺失时使用 OnlineSubsystem 提供的拥有者名称。
	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	FString ServerName;

	// OnlineSubsystem 测得的往返延迟估计，用于菜单展示，不参与连接成功判定。
	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 PingInMs = 0;

	// 由最大连接数减去剩余公开槽位得到的近似在线人数。
	UPROPERTY(BlueprintReadOnly, Category = "Network|Session")
	int32 CurrentPlayers = 0;

	// 会话声明的公开连接上限；当前代码并未强制固定为两人。
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
 * 管理跨地图存在的联机会话和自动重连流程。
 * 菜单调用公开接口，并通过可绑定事件接收异步结果。
 *
 * (*) 选择 GameInstance 是因为它不会随地图切换销毁，适合保存会话接口、连接地址和重试状态。
 * (*) OnlineSession 负责发现和加入会话，真正建立游戏网络连接还需要解析 ConnectString 并执行
 * ClientTravel；“JoinSession 成功”和“玩家已经进入 PlayingState”是两个不同阶段。
 * (**) OnlineSubsystem 使用完成回调报告结果，Null 的部分路径会在请求函数返回前同步回调。
 * 因此先绑定再请求，回调先清自己的句柄；返回失败且句柄仍有效时，入口才补做失败清理。
 * 同一时间只允许一条菜单会话流程，避免旧回调覆盖新操作；这属于流程限制，不是多线程锁。
 *
 * 当前默认使用 OnlineSubsystemNull，面向同一局域网的发现与连接；它不是带账号、邀请、匹配
 * 和云端大厅的完整线上服务。自动重连也只尝试回到仍存活且地址未变化的原服务器。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	// 获取会话接口并绑定全局网络/Travel 失败事件；任何一步缺失都保留明确日志而不继续空调用。
	virtual void Init() override;
	// 清理计时器、会话回调和全局失败事件，避免 GameInstance 退出后仍收到异步通知。
	virtual void Shutdown() override;

	/**
	 * 发起主机流程。参数先保存为 Pending 配置；若已有同名会话则先异步销毁，再创建新会话。
	 * 当前 World 若尚未监听则原地开启 Listen Server；创建成功不会自动切换地图。
	 */
	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void HostGame(const FString& ServerName, int32 PublicConnections, bool bIsLanMatch);

	/**
	 * 发起一次会话搜索。完成后将蓝图需要的字段转换为 FmultiplayerSessionInfo 并广播。
	 * 新搜索会替换旧 SessionSearch，因此旧的 ResultIndex 不能跨搜索继续使用。
	 */
	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void FindGames(int32 MaxResults, bool bIsLanQuery);

	/**
	 * 加入当前搜索结果中的指定条目。
	 * 函数会检查下标和异步状态；加入回调成功后仍需 ResolveConnectString + ClientTravel。
	 */
	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void JoinGame(int32 ResultIndex);

	/**
	 * 主动退出房间。先停止重连、销毁本地 Session，再回到项目默认主菜单。
	 * Listen Server 主机退出时会先通知远端客户端走相同清理路径。
	 */
	UFUNCTION(BlueprintCallable, Category = "Network|Session")
	void LeaveGame();

	/**
	 * 本地 PlayerController 进入可操作状态后调用。
	 * (**) 本项目用客户端进入 PlayingState 结束 Joining；这不保证所有 Actor 属性和 UI 都已就绪。
	 */
	void NotifyClientConnected();

	// 搜索完成事件。bWasSuccessful 表示搜索调用是否成功，空 Results 不等于接口调用失败。
	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionSearchEvent OnFindComplete;

	// 菜单利用该事件锁定/恢复按钮，避免建房、搜索和加入互相覆盖。
	UPROPERTY(BlueprintAssignable, Category = "Network|Session")
	FmultiplayerSessionOperationChanged OnSessionOperationChanged;

private:
	// 使用 Pending 配置真正调用 OnlineSubsystem::CreateSession。
	void CreateSession();
	// 确保当前 World 具备监听连接的 NetDriver；只原地监听，不执行地图切换。
	bool EnsureCurrentWorldIsListening();
	// 为销毁旧会话注册一次回调，并记录完成后是重新建房还是退出。
	void BindDestroyDelegate(EMultiplayerDestroyPurpose Purpose);
	// Session 销毁完成或无需销毁时，统一返回默认主菜单。
	void FinishLeaveGame();
	// 统一解绑会话回调并重置句柄及销毁用途。
	void ClearSessionDelegates();
	// 获取异步操作占用权；已有操作时拒绝新请求并保持原回调链不变。
	bool BeginSessionOperation(EMultiplayerSessionOperation NewOperation);
	// 释放异步操作占用权并通知菜单恢复交互。
	void EndSessionOperation();
	// 连接失败时移除客户端本地会话记录，避免后续 JoinSession 被同名记录阻塞。
	void RemoveFailedLocalSession();
	// 发起新连接前停止旧重试，并清空旧地址，避免过期定时器把玩家带回旧服务器。
	void CancelAutomaticReconnect();

	// 完成回调先释放自己的 DelegateHandle；Create 只公布当前地图的房间，不再发起地图迁移。
	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	// 转换结果后先结束 Finding 再广播，允许菜单在结果回调里直接发起加入。
	void HandleFindSessionsComplete(bool bWasSuccessful);
	// 成功后解析地址并发起 ClientTravel；Joining 保持到 NotifyClientConnected 或连接失败。
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	// 按 DestroyPurpose 分别续接重新建房或返回菜单，不将两种后续行为混用。
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful);
	// 退出后核对默认菜单已加载并解除限制；开发构建还用此通知启动命令行自动化。
	void HandlePostLoadMap(UWorld* LoadedWorld);
	// 引擎全局事件必须先核对 World/Driver 归属，再决定有限重连还是清理失败状态。
	void HandleNetworkFailure(
		UWorld* World,
		UNetDriver* NetDriver,
		ENetworkFailure::Type FailureType,
		const FString& ErrorString);
	// 加载失败分别处理退出、重连和普通加入；这是错误回收入口，不代表实现了自动选关。
	void HandleTravelFailure(
		UWorld* World,
		ETravelFailure::Type FailureType,
		const FString& ErrorString);
	// 统一记录可检索的失败来源、类型和错误文本，便于双实例测试时对照日志。
	void RecordConnectionFailure(
		const TCHAR* FailureSource,
		const FString& FailureType,
		const FString& ErrorString);
	// 只允许客户端的短暂断线/超时进入重连；永久错误和主机端错误立即失败。
	bool CanRetryNetworkFailure(
		UNetDriver* NetDriver,
		ENetworkFailure::Type FailureType) const;
	// 按 1/2/4 秒有限退避安排下一次尝试，避免断线后无上限高频重连。
	void ScheduleAutomaticReconnect();
	// 使用最近一次 ConnectString 再次 ClientTravel；成功由 NotifyClientConnected 最终确认。
	void TryAutomaticReconnect();

#if !UE_BUILD_SHIPPING
	// 注入引擎失败事件以覆盖重连分支；不会真正中断物理网络。
	void InjectNetworkFailureForTesting();
	// 按命令行 Host/Join 角色启动一次开发测试，正常游玩不触发。
	void StartSessionAutomationIfRequested();
	// 提交一轮测试搜索，是否重试由搜索完成回调判断。
	void RunSessionAutomationFind();
#endif

	// OnlineSubsystem 的会话接口与最近一次搜索对象；搜索结果的真实生命周期由后者决定。
	IOnlineSessionPtr SessionInterface;
	TSharedPtr<FOnlineSessionSearch> SessionSearch;

	// 句柄是订阅凭据，不拥有 GameInstance；按类型精确解绑，避免影响同一接口的其他订阅者。
	FDelegateHandle CreateSessionCompleteHandle;
	FDelegateHandle FindSessionsCompleteHandle;
	FDelegateHandle JoinSessionCompleteHandle;
	FDelegateHandle DestroySessionCompleteHandle;
	FDelegateHandle NetworkFailureHandle;
	FDelegateHandle TravelFailureHandle;
	FDelegateHandle PostLoadMapHandle;

	// HostGame 先写入以下 Pending 配置，销毁旧会话完成后 CreateSession 再读取。
	FString PendingServerName = TEXT("Coop Session");
	/**
	 * 最近一次可用的直连地址。
	 * (**) 当前重连只适用于服务器仍存活且地址未改变的短暂掉线；它不会恢复旧 Pawn，
	 * 也不是跨服务器的账号级断线续玩，共享目标状态由服务器现有 GameState 继续提供。
	 */
	FString LastConnectString;
	int32 PendingPublicConnections = 4;
	bool bPendingIsLanMatch = true;
	// 菜单异步操作与网络重连使用两套状态，避免把会话回调和连接恢复混为一谈。
	EMultiplayerSessionOperation CurrentOperation = EMultiplayerSessionOperation::None;
	EMultiplayerDestroyPurpose DestroyPurpose = EMultiplayerDestroyPurpose::None;
	EMultiplayerReconnectState ReconnectState = EMultiplayerReconnectState::Idle;
	FTimerHandle ReconnectTimerHandle;
	int32 ReconnectAttempt = 0;
	// 会话操作 None 不代表菜单已经可用：退出还需等默认地图加载完成。
	bool bLeaveInProgress = false;

#if !UE_BUILD_SHIPPING
	// 开发包专用的失败事件注入入口；它验证重连逻辑，不等同于真实断网测试。
	FTimerHandle ReconnectTestTimerHandle;
	FTimerHandle SessionAutomationTimerHandle;
	bool bReconnectTestTriggered = false;
	bool bSessionAutomationStarted = false;
	bool bSessionAutomationTravelStarted = false;
	bool bSessionAutomationJoinConfirmed = false;
	int32 SessionAutomationFindAttempt = 0;
	// 每次双进程测试独有；客户端只加入同一 token 的主机会话，避免并发测试串房。
	FString SessionAutomationToken;
#endif
};
