#include "Network/multiplayerGameInstance.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Core/multiplayerGameMode.h"
#include "Core/multiplayerLog.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

// 本地连接流程层：跨 World 维护连接、有限重试、票据和取消意图。
// 服务器只通过所属 Controller 校验票据与恢复状态；本实例不接受客户端上传战斗快照。

// 进程初始化：加载本进程配置，并订阅网络失败、跳转失败和地图加载通知。
void UmultiplayerGameInstance::Init()
{
	Super::Init();
	//加载Content/Config/Gameplay.json
	GameplayConfig.LoadFromFile(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Config/Gameplay.json")));
	// 引擎事件可能涉及多个 World；这里只订阅，回调中再判断是否属于当前实例。
	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &ThisClass::HandleTravelFailure);
	}
	// 返回本地菜单后，通过地图加载通知结束 Leaving 状态。
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);
}

// 进程结束：停止本实例的等待，并解除 Init 中建立的事件订阅。
void UmultiplayerGameInstance::Shutdown()
{
	// 不让连接超时或重试等待在实例结束后继续执行。
	ClearConnectionTimers();
	++OperationRevision;
	HandshakeController.Reset();
	ReconnectTickets.Reset();
	// 与初始化时的引擎事件订阅对称清理。
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	Super::Shutdown();
}

// 校验并规范化主机名和端口；只检查输入格式，不验证服务器可达性或玩家身份。
bool UmultiplayerGameInstance::NormalizeServerAddress(const FString& Input, FString& Output)
{
	// 清空旧输出、去掉首尾空格，再检查地址能否拆成主机名与端口。
	Output.Reset();
	const FString Address = Input.TrimStartAndEnd();
	FString Host, PortText;
	if (Address.Len() > 260 || !Address.Split(TEXT(":"), &Host, &PortText) || Host.IsEmpty()
		|| Host.Len() > 253 || PortText.IsEmpty() || PortText.Len() > 5) return false;
	// 端口必须是十进制数字，且处于有效范围内。
	for (const TCHAR Ch : PortText) if (Ch < '0' || Ch > '9') return false;
	const int32 Port = FCString::Atoi(*PortText);
	if (Port < 1 || Port > 65535) return false;
	// 按点分隔检查主机名，拒绝地图路径、URL 参数和其他不支持的字符。
	TArray<FString> Labels;
	Host.ParseIntoArray(Labels, TEXT("."), false);
	for (const FString& Label : Labels)
	{
		if (Label.IsEmpty() || Label.Len() > 63 || Label.StartsWith(TEXT("-")) || Label.EndsWith(TEXT("-"))) return false;
		for (const TCHAR Ch : Label)
			if (!((Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '-')) return false;
	}
	// 统一主机名大小写和端口写法，供后续连接及重连保存使用。
	Output = FString::Printf(TEXT("%s:%d"), *Host.ToLower(), Port);
	return true;
}

// 首次连接入口：仅接受本地世界的空闲请求；返回 true 只表示请求仍处于 Connecting。
bool UmultiplayerGameInstance::ConnectToServer(const FString& Address)
{
	UWorld* World = GetWorld();
	FString Normalized;
	// 拒绝请求不能修改当前地址或取消正在等待的重连。
	if (!World || World->GetNetMode() != NM_Standalone || ConnectionState != EMultiplayerConnectionState::Idle) return false;
	// 格式不合法时只更新提示，不发起网络连接。
	if (!NormalizeServerAddress(Address, Normalized))
	{
		SetConnectionState(EMultiplayerConnectionState::Idle,
			NSLOCTEXT("Multiplayer", "InvalidAddress", "请输入服务器地址，例如 127.0.0.1:7777"));
		return false;
	}
	// 通过校验后才保存地址、重置次数，并复用统一的连接执行流程。
	LastServerAddress = MoveTemp(Normalized);
	ReconnectAttempt = 0;
	StartTravel(false);
	return ConnectionState == EMultiplayerConnectionState::Connecting;
}

// 统一提交本地状态与提示，再通知监听者；返回本次提交的编号，不是广播后的最新编号。
// 设计：OperationRevision 标识本机连接操作，过滤旧 Timer/回执，也隔离广播中的同步取消。
// 它不对底层网络包排序，不替代服务器的票据和接纳校验。
uint64 UmultiplayerGameInstance::SetConnectionState(EMultiplayerConnectionState State, const FText& Message)
{
	ConnectionState = State;
	ConnectionMessage = Message;
	// 先保存自己的编号；同步监听者可能再次修改状态，让当前操作失效。
	const uint64 Revision = ++OperationRevision;
	OnConnectionChanged.Broadcast();
	return Revision;
}

// 同时取消连接期限和重连等待；这里只清理计时器，不关闭底层网络连接。
void UmultiplayerGameInstance::ClearConnectionTimers()
{
	GetTimerManager().ClearTimer(ConnectTimeoutHandle);
	GetTimerManager().ClearTimer(ReconnectTimerHandle);
}

// 执行一次连接尝试：首次连接和重连共用 Travel 与超时处理，区别只在状态和失败去向。
void UmultiplayerGameInstance::StartTravel(bool bRetry)
{
	// 新尝试接管旧等待；先提交状态，供菜单显示连接中的提示。
	ClearConnectionTimers();
	HandshakeController.Reset();
	const uint64 Revision = SetConnectionState(
		bRetry ? EMultiplayerConnectionState::Reconnecting : EMultiplayerConnectionState::Connecting,
		NSLOCTEXT("Multiplayer", "ConnectingDS", "正在连接服务器……"));
	// 通知可以同步触发取消；编号未变，才继续本次连接。
	if (Revision != OperationRevision) return;
	// Travel 由本地控制器发起；控制器尚未就绪时，首次请求失败，重试则继续按预算安排。
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC)
	{
		if (bRetry) ScheduleReconnect(); else FailConnection(TEXT("Local player is not ready"));
		return;
	}
	// 覆盖地址不可达、已发起 Travel 却未进入 PlayingState 的路径；GI Timer 跨 World 存活。
	GetTimerManager().SetTimer(ConnectTimeoutHandle,
		FTimerDelegate::CreateUObject(this, &ThisClass::HandleConnectTimeout, Revision), 20.f, false);
	// 将建立连接和加载服务器地图交给 UE；调用返回不代表已连接成功。
	PC->ClientTravel(LastServerAddress, TRAVEL_Absolute);
}

// 本次连接等待到期：忽略旧编号，取消尚未完成的连接，再按首次请求或重试分别收尾。
void UmultiplayerGameInstance::HandleConnectTimeout(uint64 Revision)
{
	// 成功、退出或另一轮尝试已经更新编号时，旧超时不能影响当前状态。
	if (Revision != OperationRevision) return;
	// 先保留本次是否重试，再取消 Pending；后续收尾会改变状态。
	const bool bRetry = ConnectionState == EMultiplayerConnectionState::Reconnecting;
	if (GEngine && GetWorld()) GEngine->CancelPending(GetWorld());
	if (Revision != OperationRevision) return;
	// 首次连接到期直接报错；重连到期使用剩余次数安排下一次。
	if (bRetry) ScheduleReconnect(); else FailConnection(TEXT("Connection timed out"));
	if (GetWorld() && GetWorld()->GetNetMode() == NM_Client) ReturnToMainMenu();
}

// 所属客户端在 PlayingState 准备票据与操作编号，重置握手期限但不提前宣告连接成功。
bool UmultiplayerGameInstance::PrepareCombatHandshake(AmultiplayerCoopPlayerController* Controller,
	FGuid& OutTicket, uint64& OutOperation)
{
	UWorld* World = GetWorld();
	if (!Controller || Controller != GetFirstLocalPlayerController() || Controller->GetWorld() != World
		|| !World || World->GetNetMode() != NM_Client || ConnectionState == EMultiplayerConnectionState::Leaving) return false;
	FString Address;
	if (!NormalizeServerAddress(FString::Printf(TEXT("%s:%d"), *World->URL.Host, World->URL.Port), Address)) return false;
	LastServerAddress = MoveTemp(Address);
	// 控制台直连和整体回合重开也必须经过相同握手；重连则保留原来的有限重试预算。
	if (ConnectionState != EMultiplayerConnectionState::Connecting && ConnectionState != EMultiplayerConnectionState::Reconnecting)
	{
		const uint64 Revision = SetConnectionState(EMultiplayerConnectionState::Connecting,
			NSLOCTEXT("Multiplayer", "ConfirmCombatSession", "正在确认服务器会话……"));
		if (Revision != OperationRevision) return false;
	}
	ClearConnectionTimers();
	HandshakeController = Controller;
	OutOperation = OperationRevision;
	OutTicket = ReconnectTickets.FindRef(LastServerAddress);
	GetTimerManager().SetTimer(ConnectTimeoutHandle,
		FTimerDelegate::CreateUObject(this, &ThisClass::HandleConnectTimeout, OutOperation), 20.f, false);
	return true;
}

// 只消费当前 Controller/World/操作的回执；成功记住轮换票据，失败按服务器原因终止或有限重试。
bool UmultiplayerGameInstance::CompleteCombatHandshake(AmultiplayerCoopPlayerController* Controller,
	uint64 Operation, const FGuid& Ticket, const FText& Error, bool bRetryable)
{
	if (!Controller || Controller != HandshakeController.Get() || Controller != GetFirstLocalPlayerController()
		|| Controller->GetWorld() != GetWorld() || Operation != OperationRevision
		|| (ConnectionState != EMultiplayerConnectionState::Connecting && ConnectionState != EMultiplayerConnectionState::Reconnecting)) return false;
	HandshakeController.Reset();
	if (Ticket.IsValid() && Error.IsEmpty())
	{
		ReconnectTickets.Add(LastServerAddress, Ticket);
		return true;
	}
	// 只有旧连接尚未被服务器清理可以继续有限重试；失效票据明确终止，不能静默换成新人物。
	if (!bRetryable) ReconnectTickets.Remove(LastServerAddress);
	if (bRetryable && ConnectionState == EMultiplayerConnectionState::Reconnecting) ScheduleReconnect();
	else FailConnection(Error.IsEmpty() ? TEXT("Server rejected the combat session") : Error.ToString());
	ReturnToMainMenu();
	return false;
}

// 接收所属控制器的票据更新：正常切图清除，旧 World 重开失败时可恢复原票据。
void UmultiplayerGameInstance::SetReconnectTicket(AmultiplayerCoopPlayerController* Controller, const FGuid& Ticket)
{
	if (!Controller || Controller != GetFirstLocalPlayerController() || Controller->GetWorld() != GetWorld()
		|| !Controller->HasAcceptedCombatSession() || LastServerAddress.IsEmpty()) return;
	if (Ticket.IsValid()) ReconnectTickets.Add(LastServerAddress, Ticket);
	else ReconnectTickets.Remove(LastServerAddress);
}

// 本地 Controller 收到服务器接纳并完成个人恢复后确认连接；不保证所有机关复制和 UI 已就绪。
void UmultiplayerGameInstance::NotifyClientConnected()
{
	UWorld* World = GetWorld();
	// 只接受客户端游戏世界的确认，不能把菜单、服务器或主动退出改成已连接。
	if (!World || World->GetNetMode() != NM_Client || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	const auto* Controller = Cast<AmultiplayerCoopPlayerController>(GetFirstLocalPlayerController());
	if (!Controller || !Controller->HasAcceptedCombatSession()) return;
	// 控制台/命令行直连没有菜单请求；从实际 URL 补记地址，重开后也重新确认。
	FString Address;
	if (NormalizeServerAddress(FString::Printf(TEXT("%s:%d"), *World->URL.Host, World->URL.Port), Address)) LastServerAddress = Address;
	// 成功后清掉旧等待并重置重试预算，提交 Connected 也会使旧超时编号失效。
	ClearConnectionTimers();
	ReconnectAttempt = 0;
	SetConnectionState(EMultiplayerConnectionState::Connected, NSLOCTEXT("Multiplayer", "ConnectedDS", "已连接服务器"));
}

// 主动退出或取消连接：先关闭自动重连意图，再交给引擎返回本地菜单。
void UmultiplayerGameInstance::LeaveGame()
{
	// DS 不执行玩家菜单退出，已经退出中的客户端也不重复启动流程。
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_DedicatedServer || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	// 清掉等待、地址和次数，避免主动关闭连接后又被断线通知自动连回。
	ClearConnectionTimers();
	ReconnectTickets.Remove(LastServerAddress);
	HandshakeController.Reset();
	LastServerAddress.Reset();
	ReconnectAttempt = 0;
	// 先提交 Leaving，使旧超时失效；监听者可以同步更新退出界面。
	const uint64 Revision = SetConnectionState(EMultiplayerConnectionState::Leaving,
		NSLOCTEXT("Multiplayer", "LeavingDS", "正在返回连接菜单……"));
	// 广播期间若被其他操作接管，就不继续执行这次旧退出。
	if (Revision != OperationRevision) return;
	// 取消 Pending；正式连接与旧 World 的清理由引擎返回菜单流程处理。
	if (GEngine) GEngine->CancelPending(GetWorld());
	ReturnToMainMenu();
}

// 自己的本地世界加载完成后结束退出；地图加载本身不作为服务器连接成功的判据。
void UmultiplayerGameInstance::HandlePostLoadMap(UWorld* World)
{
	if (World && World->GetGameInstance() == this && World->GetNetMode() == NM_Standalone
		&& ConnectionState == EMultiplayerConnectionState::Leaving)
	{
		SetConnectionState(EMultiplayerConnectionState::Idle, FText::GetEmpty());
	}
}

// 判断全局网络错误是否属于当前实例，同时覆盖正式连接和 PendingNetGame 中的连接。
bool UmultiplayerGameInstance::OwnsFailure(const UWorld* World, const UNetDriver* Driver) const
{
	// 已知 World 属于其他实例时直接忽略，避免编辑器多实例之间串台。
	if (World && (World->GetGameInstance() != this || World != GetWorld())) return false;
	// 当前实例的 World 与正式驱动能够对应时，直接确认归属。
	if (World && (!Driver || World->GetNetDriver() == Driver)) return true;
	// World 信息不足时，必须有驱动与引擎上下文才能继续查找。
	if (!Driver || !GEngine) return false;
	// 正在连接的驱动可能还挂在 PendingNetGame 上，不能只查当前 World 的正式驱动。
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.OwningGameInstance != this) continue;
		if (Context.PendingNetGame && Context.PendingNetGame->GetNetDriver() == Driver) return true;
		if (Context.World() && Context.World()->GetNetDriver() == Driver) return true;
	}
	return false;
}

// 引擎网络失败入口：先过滤归属，再区分 DS、主动退出、有限重连和终止请求。
void UmultiplayerGameInstance::HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error)
{
	// 别的实例不处理；主动退出中的断开也不能再次启动重连。
	if (!OwnsFailure(World, Driver) || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	// DS 不参与客户端菜单与重连；底层连接清理仍由 UE 负责。
	if ((World && World->GetNetMode() == NM_DedicatedServer) || (Driver && Driver->GetNetMode() == NM_DedicatedServer))
	{
		UE_LOG(LogMultiplayer, Warning, TEXT("DS network event %s: %s; server continues independently."), ENetworkFailure::ToString(Type), *Error);
		return;
	}
	// 失败回执已经结束业务操作时，底层随后清连接的通知不能覆盖具体原因。
	if (ConnectionState == EMultiplayerConnectionState::Idle && LastServerAddress.IsEmpty()) return;
	HandshakeController.Reset();
	// 自动恢复仅面向已连接后的丢线或重试过程中的连接失败；首次连接失败不循环重试。
	const bool bRetryable = Type == ENetworkFailure::ConnectionLost || Type == ENetworkFailure::ConnectionTimeout
		|| (ConnectionState == EMultiplayerConnectionState::Reconnecting && Type == ENetworkFailure::PendingConnectionFailure);
	if (bRetryable && !LastServerAddress.IsEmpty() && (ConnectionState == EMultiplayerConnectionState::Connected
		|| ConnectionState == EMultiplayerConnectionState::Reconnecting || ConnectionState == EMultiplayerConnectionState::ReconnectWaiting))
	{
		ScheduleReconnect();
		return;
	}
	// 无地址、不处于可重连阶段或错误不允许重试时，统一结束本地请求。
	FailConnection(Error);
}

// 引擎跳转失败入口：优先处理服务器重开恢复，其余按重连或普通失败收尾。
void UmultiplayerGameInstance::HandleTravelFailure(UWorld* World, ETravelFailure::Type, const FString& Error)
{
	// Travel 通知同样是全局事件，只处理当前实例的 World。
	if (!World || World->GetGameInstance() != this || World != GetWorld()) return;
	// 服务器重开失败可能仍能恢复旧世界，先让 GameMode 处理自己的请求快照。
	if (auto* GM = World->GetAuthGameMode<AmultiplayerGameMode>())
		if (GM->RecoverFailedRestart(Error)) return;
	// DS 不走玩家返回菜单与重试逻辑。
	if (World->GetNetMode() == NM_DedicatedServer) { UE_LOG(LogMultiplayer, Error, TEXT("DS travel failed: %s"), *Error); return; }
	// 重连中的跳转失败消耗同一份重试预算，不重新开启无限连接。
	if (ConnectionState == EMultiplayerConnectionState::Reconnecting)
	{
		ScheduleReconnect();
		return;
	}
	// 失败清理会改状态并通知外部；先保存本次是否退出，再共用清理出口。
	const bool bWasLeaving = ConnectionState == EMultiplayerConnectionState::Leaving;
	FailConnection(Error);
	// 退出失败只通知本地 Controller 恢复操作能力，不恢复旧连接或之前的地址。
	if (bWasLeaving)
	{
		OnLeaveFailed.Broadcast(NSLOCTEXT("Multiplayer", "LeaveFailed", "返回菜单失败，请重试退出。"));
	}
}

// 项目层的统一失败出口：清连接数据并显示错误；不独立承担全部引擎连接和 World 清理。
void UmultiplayerGameInstance::FailConnection(const FString& Reason)
{
	// 结束等待并放弃原地址，之后需要玩家重新提交连接请求。
	ClearConnectionTimers();
	HandshakeController.Reset();
	LastServerAddress.Reset();
	ReconnectAttempt = 0;
	// Idle 表示业务操作结束，不表示引擎已经同步完成返回菜单。
	UE_LOG(LogMultiplayer, Warning, TEXT("DS connection failed: %s"), *Reason);
	SetConnectionState(EMultiplayerConnectionState::Idle, FText::FromString(Reason));
}

// 安排下一次重连等待：按配置间隔有限尝试，重复断线通知不重复创建等待。
// 取舍：只为允许重试的掉线保留有限预算；主动退出不连回，不用无限后台重试掩盖接纳失败。
void UmultiplayerGameInstance::ScheduleReconnect()
{
	// 已经安排等待时保持原计时器，避免重复通知推迟或叠加尝试。
	if (ConnectionState == EMultiplayerConnectionState::ReconnectWaiting) return;
	// 地址已被清除或预算耗尽时结束流程，不再自动连接。
	if (LastServerAddress.IsEmpty() || ReconnectAttempt >= GameplayConfig.ReconnectDelaysSeconds.Num())
	{
		FailConnection(TEXT("Reconnect attempts exhausted; connect manually to retry"));
		return;
	}
	// 用已开始的尝试次数选择下一次间隔；这里安排等待，还没有增加尝试次数。
	ClearConnectionTimers();
	HandshakeController.Reset();
	const float Delay = GameplayConfig.ReconnectDelaysSeconds[ReconnectAttempt];
	const uint64 Revision = SetConnectionState(EMultiplayerConnectionState::ReconnectWaiting,
		NSLOCTEXT("Multiplayer", "ReconnectWaiting", "连接中断，等待重试……"));
	// 等待通知也可能同步触发退出，操作未被替换才真正注册计时器。
	if (Revision != OperationRevision) return;
	// 弱绑定实例并检查操作编号，主动退出或新的连接意图会使旧等待失效。
	GetTimerManager().SetTimer(ReconnectTimerHandle, FTimerDelegate::CreateWeakLambda(this, [this, Revision]
	{
		if (Revision == OperationRevision) TryReconnect();
	}), Delay, false);
}

// 重连等待 Timer 到期后开始一次尝试；已取消等待时不执行，也不增加次数。
void UmultiplayerGameInstance::TryReconnect()
{
	if (ConnectionState != EMultiplayerConnectionState::ReconnectWaiting) return;
	// 只有真正开始连接才累计次数，连接执行仍复用 StartTravel 的超时与成功确认。
	++ReconnectAttempt;
	StartTravel(true);
}
