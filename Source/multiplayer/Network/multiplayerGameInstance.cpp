#include "Network/multiplayerGameInstance.h"
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

void UmultiplayerGameInstance::Init()
{
	Super::Init();
	//加载Content/Config/Gameplay.json
	GameplayConfig.LoadFromFile(FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Config/Gameplay.json")));
	if (GEngine)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &ThisClass::HandleTravelFailure);
	}
	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &ThisClass::HandlePostLoadMap);
}

void UmultiplayerGameInstance::Shutdown()
{
	ClearConnectionTimers();
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
	Super::Shutdown();
}

bool UmultiplayerGameInstance::NormalizeServerAddress(const FString& Input, FString& Output)
{
	Output.Reset();
	const FString Address = Input.TrimStartAndEnd();
	FString Host, PortText;
	if (Address.Len() > 260 || !Address.Split(TEXT(":"), &Host, &PortText) || Host.IsEmpty()
		|| Host.Len() > 253 || PortText.IsEmpty() || PortText.Len() > 5) return false;
	for (const TCHAR Ch : PortText) if (Ch < '0' || Ch > '9') return false;
	const int32 Port = FCString::Atoi(*PortText);
	if (Port < 1 || Port > 65535) return false;
	TArray<FString> Labels;
	Host.ParseIntoArray(Labels, TEXT("."), false);
	for (const FString& Label : Labels)
	{
		if (Label.IsEmpty() || Label.Len() > 63 || Label.StartsWith(TEXT("-")) || Label.EndsWith(TEXT("-"))) return false;
		for (const TCHAR Ch : Label)
			if (!((Ch >= 'a' && Ch <= 'z') || (Ch >= 'A' && Ch <= 'Z') || (Ch >= '0' && Ch <= '9') || Ch == '-')) return false;
	}
	Output = FString::Printf(TEXT("%s:%d"), *Host.ToLower(), Port);
	return true;
}

bool UmultiplayerGameInstance::ConnectToServer(const FString& Address)
{
	UWorld* World = GetWorld();
	FString Normalized;
	// 拒绝请求不能修改当前地址或取消正在等待的重连。
	if (!World || World->GetNetMode() != NM_Standalone || ConnectionState != EMultiplayerConnectionState::Idle) return false;
	if (!NormalizeServerAddress(Address, Normalized))
	{
		SetConnectionState(EMultiplayerConnectionState::Idle,
			NSLOCTEXT("Multiplayer", "InvalidAddress", "请输入服务器地址，例如 127.0.0.1:7777"));
		return false;
	}
	LastServerAddress = MoveTemp(Normalized);
	ReconnectAttempt = 0;
	StartTravel(false);
	return ConnectionState == EMultiplayerConnectionState::Connecting;
}

uint64 UmultiplayerGameInstance::SetConnectionState(EMultiplayerConnectionState State, const FText& Message)
{
	ConnectionState = State;
	ConnectionMessage = Message;
	const uint64 Revision = ++OperationRevision;
	OnConnectionChanged.Broadcast();
	return Revision;
}

void UmultiplayerGameInstance::ClearConnectionTimers()
{
	GetTimerManager().ClearTimer(ConnectTimeoutHandle);
	GetTimerManager().ClearTimer(ReconnectTimerHandle);
}

void UmultiplayerGameInstance::StartTravel(bool bRetry)
{
	ClearConnectionTimers();
	const uint64 Revision = SetConnectionState(
		bRetry ? EMultiplayerConnectionState::Reconnecting : EMultiplayerConnectionState::Connecting,
		NSLOCTEXT("Multiplayer", "ConnectingDS", "正在连接服务器……"));
	// 通知可以同步触发取消；编号未变，才继续本次连接。
	if (Revision != OperationRevision) return;
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC)
	{
		if (bRetry) ScheduleReconnect(); else FailConnection(TEXT("Local player is not ready"));
		return;
	}
	// 覆盖地址不可达、已发起 Travel 却未进入 PlayingState 的路径；GI Timer 跨 World 存活。
	GetTimerManager().SetTimer(ConnectTimeoutHandle,
		FTimerDelegate::CreateUObject(this, &ThisClass::HandleConnectTimeout, Revision), 20.f, false);
	PC->ClientTravel(LastServerAddress, TRAVEL_Absolute);
}

void UmultiplayerGameInstance::HandleConnectTimeout(uint64 Revision)
{
	if (Revision != OperationRevision) return;
	const bool bRetry = ConnectionState == EMultiplayerConnectionState::Reconnecting;
	if (GEngine && GetWorld()) GEngine->CancelPending(GetWorld());
	if (bRetry) ScheduleReconnect(); else FailConnection(TEXT("Connection timed out"));
}

void UmultiplayerGameInstance::NotifyClientConnected()
{
	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() != NM_Client || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	// 控制台/命令行直连没有菜单请求；从实际 URL 补记地址，重开后也重新确认。
	FString Address;
	if (NormalizeServerAddress(FString::Printf(TEXT("%s:%d"), *World->URL.Host, World->URL.Port), Address)) LastServerAddress = Address;
	ClearConnectionTimers();
	ReconnectAttempt = 0;
	SetConnectionState(EMultiplayerConnectionState::Connected, NSLOCTEXT("Multiplayer", "ConnectedDS", "已连接服务器"));
}

void UmultiplayerGameInstance::LeaveGame()
{
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_DedicatedServer || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	ClearConnectionTimers();
	LastServerAddress.Reset();
	ReconnectAttempt = 0;
	const uint64 Revision = SetConnectionState(EMultiplayerConnectionState::Leaving,
		NSLOCTEXT("Multiplayer", "LeavingDS", "正在返回连接菜单……"));
	if (Revision != OperationRevision) return;
	if (GEngine) GEngine->CancelPending(GetWorld());
	ReturnToMainMenu();
}

void UmultiplayerGameInstance::HandlePostLoadMap(UWorld* World)
{
	if (World && World->GetGameInstance() == this && World->GetNetMode() == NM_Standalone
		&& ConnectionState == EMultiplayerConnectionState::Leaving)
	{
		SetConnectionState(EMultiplayerConnectionState::Idle, FText::GetEmpty());
	}
}

bool UmultiplayerGameInstance::OwnsFailure(const UWorld* World, const UNetDriver* Driver) const
{
	if (World && World->GetGameInstance() != this) return false;
	if (World && (!Driver || World->GetNetDriver() == Driver)) return true;
	if (!Driver || !GEngine) return false;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.OwningGameInstance != this) continue;
		if (Context.PendingNetGame && Context.PendingNetGame->GetNetDriver() == Driver) return true;
		if (Context.World() && Context.World()->GetNetDriver() == Driver) return true;
	}
	return false;
}

void UmultiplayerGameInstance::HandleNetworkFailure(UWorld* World, UNetDriver* Driver, ENetworkFailure::Type Type, const FString& Error)
{
	if (!OwnsFailure(World, Driver) || ConnectionState == EMultiplayerConnectionState::Leaving) return;
	if ((World && World->GetNetMode() == NM_DedicatedServer) || (Driver && Driver->GetNetMode() == NM_DedicatedServer))
	{
		UE_LOG(LogMultiplayer, Warning, TEXT("DS network event %s: %s; server continues independently."), ENetworkFailure::ToString(Type), *Error);
		return;
	}
	const bool bRetryable = Type == ENetworkFailure::ConnectionLost || Type == ENetworkFailure::ConnectionTimeout
		|| (ConnectionState == EMultiplayerConnectionState::Reconnecting && Type == ENetworkFailure::PendingConnectionFailure);
	if (bRetryable && !LastServerAddress.IsEmpty() && (ConnectionState == EMultiplayerConnectionState::Connected
		|| ConnectionState == EMultiplayerConnectionState::Reconnecting || ConnectionState == EMultiplayerConnectionState::ReconnectWaiting))
	{
		ScheduleReconnect();
		return;
	}
	FailConnection(Error);
}

void UmultiplayerGameInstance::HandleTravelFailure(UWorld* World, ETravelFailure::Type, const FString& Error)
{
	if (!World || World->GetGameInstance() != this) return;
	if (auto* GM = World->GetAuthGameMode<AmultiplayerGameMode>())
		if (GM->RecoverFailedRestart(Error)) return;
	if (World->GetNetMode() == NM_DedicatedServer) { UE_LOG(LogMultiplayer, Error, TEXT("DS travel failed: %s"), *Error); return; }
	if (ConnectionState == EMultiplayerConnectionState::Reconnecting)
	{
		ScheduleReconnect();
		return;
	}
	// 失败清理会改状态并通知外部；先保存本次是否退出，再共用清理出口。
	const bool bWasLeaving = ConnectionState == EMultiplayerConnectionState::Leaving;
	FailConnection(Error);
	if (bWasLeaving)
	{
		OnLeaveFailed.Broadcast(NSLOCTEXT("Multiplayer", "LeaveFailed", "返回菜单失败，请重试退出。"));
	}
}

void UmultiplayerGameInstance::FailConnection(const FString& Reason)
{
	ClearConnectionTimers();
	LastServerAddress.Reset();
	ReconnectAttempt = 0;
	UE_LOG(LogMultiplayer, Warning, TEXT("DS connection failed: %s"), *Reason);
	SetConnectionState(EMultiplayerConnectionState::Idle, FText::FromString(Reason));
}

void UmultiplayerGameInstance::ScheduleReconnect()
{
	if (ConnectionState == EMultiplayerConnectionState::ReconnectWaiting) return;
	if (LastServerAddress.IsEmpty() || ReconnectAttempt >= GameplayConfig.ReconnectDelaysSeconds.Num())
	{
		FailConnection(TEXT("Reconnect attempts exhausted; connect manually to retry"));
		return;
	}
	ClearConnectionTimers();
	const float Delay = GameplayConfig.ReconnectDelaysSeconds[ReconnectAttempt];
	const uint64 Revision = SetConnectionState(EMultiplayerConnectionState::ReconnectWaiting,
		NSLOCTEXT("Multiplayer", "ReconnectWaiting", "连接中断，等待重试……"));
	if (Revision != OperationRevision) return;
	GetTimerManager().SetTimer(ReconnectTimerHandle, this, &ThisClass::TryReconnect, Delay, false);
}

void UmultiplayerGameInstance::TryReconnect()
{
	if (ConnectionState != EMultiplayerConnectionState::ReconnectWaiting) return;
	++ReconnectAttempt;
	StartTravel(true);
}
