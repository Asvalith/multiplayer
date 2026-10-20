#include "Testing/CoopNetTestDriver.h"
#include "Testing/CoopPlatformRideProbe.h"
#include "Testing/CoopSkillPresentationReview.h"

#include "Core/multiplayerCoopGameState.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	UCoopNetTestDriver* Driver = nullptr;
	double Now() { return FPlatformTime::Seconds(); }
}

ACoopNetTestProbe::ACoopNetTestProbe()
{
#if !UE_BUILD_SHIPPING
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(10);
#endif
}

void ACoopNetTestProbe::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACoopNetTestProbe, Stage);
	DOREPLIFETIME(ACoopNetTestProbe, Subject);
	DOREPLIFETIME(ACoopNetTestProbe, CombatTarget);
	DOREPLIFETIME(ACoopNetTestProbe, EscapeRoute);
}

void ACoopNetTestProbe::ServerReceipt_Implementation(const FString& Name, bool bPassed, const FString& Detail)
{
#if !UE_BUILD_SHIPPING
	if (Driver) Driver->Receipt(Name, bPassed, Detail, this);
#endif
}

void StartCoopNetTests()
{
    StartCoopSkillPresentationReview();
#if !UE_BUILD_SHIPPING
	FString RequestedMode;
	if (!Driver && FParse::Value(FCommandLine::Get(), TEXT("CoopNetTest="), RequestedMode))
	{
		Driver = NewObject<UCoopNetTestDriver>();
		Driver->AddToRoot();
		Driver->Start();
	}
#endif
}

void StopCoopNetTests()
{
    StopCoopSkillPresentationReview();
#if !UE_BUILD_SHIPPING
	if (Driver)
	{
		Driver->Stop();
		Driver->RemoveFromRoot();
		Driver = nullptr;
	}
#endif
}

void UCoopNetTestDriver::Start()
{
#if !UE_BUILD_SHIPPING
	FParse::Value(FCommandLine::Get(), TEXT("CoopNetTest="), Mode);
	FParse::Value(FCommandLine::Get(), TEXT("CoopTestRole="), Role);
	FParse::Value(FCommandLine::Get(), TEXT("CoopTestToken="), Token);
	FParse::Value(FCommandLine::Get(), TEXT("CoopServerAddress="), ServerAddress);
	FParse::Value(FCommandLine::Get(), TEXT("CoopTestTimeout="), TimeoutSeconds);
	bServer = Role == TEXT("Server");
	bPartner = Role == TEXT("Partner");
	bVisualAcceptance = FParse::Param(FCommandLine::Get(),TEXT("VisualAcceptance"));
	bEscapeMovementDiagnostic = FParse::Param(FCommandLine::Get(),TEXT("EscapeMovementDiagnostic"));
	StartedAt = Now();
	SetPhase(TEXT("Boot"));
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ThisClass::Tick), .1f);

	if ((Role != TEXT("Server") && Role != TEXT("Client") && Role != TEXT("Partner")) || Token.IsEmpty())
		Finish(false, TEXT("Requires CoopTestRole=Server|Client|Partner and a unique CoopTestToken"));
	if (Mode != TEXT("RideMotion") && Mode != TEXT("Combat") && Mode != TEXT("CombatTraffic") && Mode != TEXT("CombatReconnect") && Mode != TEXT("CombatLifecycle") && Mode != TEXT("CombatExpansion") && Mode != TEXT("CoopEscape"))
		Finish(false, TEXT("Supported scenarios: RideMotion, Combat, CombatTraffic, CombatReconnect, CombatLifecycle, CombatExpansion and CoopEscape"));
	if (bVisualAcceptance)
	{
		if ((Mode != TEXT("Combat") && Mode != TEXT("CombatLifecycle") && Mode != TEXT("CombatExpansion"))
			|| (!bServer && FParse::Param(FCommandLine::Get(),TEXT("NullRHI"))))
			Finish(false,TEXT("VisualAcceptance requires one existing combat fixture and two rendered clients"));
		Emit(TEXT("FIXTURE_VISUAL"),TEXT("Controlled GAS fixture with original state/damage assertions; transient setup and observation timings are not a normal escape playthrough or a performance measurement"));
	}
	if (bEscapeMovementDiagnostic)
	{
		if (Mode != TEXT("CoopEscape")) { Finish(false,TEXT("EscapeMovementDiagnostic requires CoopEscape")); return; }
		Emit(TEXT("DIAGNOSTIC_ONLY"),TEXT("Direct physical walking to Key4 to diagnose blocking hits; this is not escape-loop, skill, visual or performance acceptance"));
	}

	FString SyncMode = TEXT("PlatformInertia");
	FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformSyncMode="), SyncMode);
	if (Mode == TEXT("RideMotion") && SyncMode != TEXT("OrderedVelocity") && SyncMode != TEXT("PlatformInertia"))
		Finish(false, TEXT("RideMotion requires CoopPlatformSyncMode=OrderedVelocity|PlatformInertia"));
#endif
}

void UCoopNetTestDriver::Stop()
{
	ClearCombatSelfHealFixture();
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	ClearCombatStateReplicationHolds();
	ClearCombatAimFixture();
	FWorldDelegates::OnWorldPostActorTick.Remove(CombatPreviewFixtureTick);
	CombatPreviewFixtureTick.Reset();
	CombatPreviewFixtureWorldTicks = 0;
	FWorldDelegates::OnWorldPostActorTick.Remove(CombatAreaInputTick);
	CombatAreaInputTick.Reset();
	if (Mode == TEXT("CombatLifecycle"))
		if (UWorld* World = FindWorld())
			if (APlayerController* PC = World->GetFirstPlayerController())
				PC->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.0));
	CombatAreaEffectsBefore.Reset();
	ClearEscapeInput();
	if (bCombatMovementInputHeld)
		if (UWorld* World = FindWorld())
			if (APlayerController* PC = World->GetFirstPlayerController())
				PC->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.0));
	bCombatMovementInputHeld = false;
}

UWorld* UCoopNetTestDriver::FindWorld() const
{
	if (GEngine) for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* World = Context.World();
		if (World && Context.WorldType == EWorldType::Game && World->HasBegunPlay()) return World;
	}
	return nullptr;
}

void UCoopNetTestDriver::Emit(const FString& Status, const FString& Detail, TSharedPtr<FJsonObject> Extra) const
{
	TSharedRef<FJsonObject> Event = Extra.IsValid() ? Extra.ToSharedRef() : MakeShared<FJsonObject>();
	Event->SetStringField(TEXT("mode"), Mode);
	Event->SetStringField(TEXT("role"), Role);
	Event->SetStringField(TEXT("token"), Token);
	Event->SetStringField(TEXT("status"), Status);
	Event->SetStringField(TEXT("phase"), Phase);
	Event->SetStringField(TEXT("detail"), Detail);
	Event->SetNumberField(TEXT("elapsed"), Now() - StartedAt);
	FString Json;
	FJsonSerializer::Serialize(Event, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
	UE_LOG(LogTemp, Display, TEXT("COOP_TEST %s"), *Json);
}

void UCoopNetTestDriver::SetPhase(const FString& Value)
{
	Phase = Value;
	PhaseAt = Now();
	Emit(TEXT("STAGE"));
}

void UCoopNetTestDriver::SetCommand(const FString& Command)
{
	SetPhase(Command);
	if (Probe.IsValid()) { Probe->Stage = Command; Probe->ForceNetUpdate(); }
	if (PartnerProbe.IsValid()) { PartnerProbe->Stage = Command; PartnerProbe->ForceNetUpdate(); }
}

void UCoopNetTestDriver::Assert(const FString& Name, bool bPassed, const FString& Detail)
{
	TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetStringField(TEXT("assertion"), Name);
	Extra->SetBoolField(TEXT("passed"), bPassed);
	Emit(TEXT("ASSERT"), Detail, Extra);
	if (!bPassed) Finish(false, Name + TEXT(": ") + Detail);
}

void UCoopNetTestDriver::SendReceipt(const FString& Name, bool bPassed, const FString& Detail)
{
	if (!Probe.IsValid() || SentReceipts.Contains(Name)) return;
	SentReceipts.Add(Name);
	Assert(Name, bPassed, Detail);
	Probe->ServerReceipt(Name, bPassed, Detail);
}

void UCoopNetTestDriver::Receipt(const FString& Name, bool bPassed, const FString& Detail, ACoopNetTestProbe* Source)
{
	if (!bServer || bDone) return;
	// RPC 所属 Probe 区分两条连接，不能把同一客户端的重复回执算成两人成功。
	const ACoopNetTestProbe* Expected = Name.StartsWith(TEXT("Partner")) ? PartnerProbe.Get() : Probe.Get();
	if (!Source || Source != Expected || Receipts.Contains(Name)) return;
	Receipts.Add(Name);
	Emit(TEXT("RECEIPT"), Name + TEXT(": ") + Detail);
	if (!bPassed) Assert(Name, false, Detail);
}

void UCoopNetTestDriver::Finish(bool bPassed, const FString& Detail)
{
	if (bDone) return;
	ClearCombatSelfHealFixture();
	if (CombatAreaInputTick.IsValid())
	{
		FWorldDelegates::OnWorldPostActorTick.Remove(CombatAreaInputTick);
		CombatAreaInputTick.Reset();
		if (UWorld* World = FindWorld())
			if (auto* PC = World->GetFirstPlayerController())
			{
				if (auto* Character = Cast<AmultiplayerCombatCharacter>(PC->GetPawn())) Character->CancelSkillTargeting();
				PC->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.0));
			}
	}
	ClearCombatAimFixture();
	ClearEscapeInput();
	// 断言失败也释放测试按键，不能让角色在延迟退出窗口继续收到 W。
	if (bCombatMovementInputHeld)
		if (UWorld* World = FindWorld())
			if (APlayerController* PC = World->GetFirstPlayerController())
				PC->InputKey(FInputKeyParams(EKeys::W, IE_Released, 0.0));
	bCombatMovementInputHeld = false;
	bDone = true;
	PhaseAt = Now();
	TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetBoolField(TEXT("passed"), bPassed);
	if (!bPassed) Emit(TEXT("FAIL"), Detail);
	Emit(TEXT("DONE"), Detail, Extra);
}

bool UCoopNetTestDriver::Tick(float)
{
#if !UE_BUILD_SHIPPING
	if (bDone)
	{
		if (Now() - PhaseAt > (bServer ? 5 : 2)) FPlatformMisc::RequestExit(false);
		return true;
	}
	if (Now() - StartedAt > TimeoutSeconds) { Finish(false, TEXT("Test timeout")); return true; }
	if (bVisualAcceptance && Phase.StartsWith(TEXT("Combat")) && Now()-PhaseAt>30.f)
	{ Finish(false,TEXT("Visual fixture state/image observation window expired: ")+Phase); return true; }
	UWorld* World = FindWorld();
	if (!World) return true;
	GameInstance = Cast<UmultiplayerGameInstance>(World->GetGameInstance());
	if (!GameInstance.IsValid()) { Finish(false, TEXT("Wrong GameInstance")); return true; }
	if (bServer) ServerTick(World);
	else if (bPartner) PartnerTick(World);
	else ClientTick(World);
#endif
	return true;
}

APlayerController* UCoopNetTestDriver::PartnerPlayer(UWorld* World) const
{
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		if (It->Get() && It->Get()->PlayerState
			&& It->Get()->PlayerState->GetUniqueId().ToString() == PartnerPlayerId) return It->Get();
	return nullptr;
}

APlayerController* UCoopNetTestDriver::PrimaryPlayer(UWorld* World) const
{
	APlayerController* Partner = PartnerPlayer(World);
	if (!Partner) return nullptr;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		if (It->Get() && It->Get() != Partner && It->Get()->GetCharacter()) return It->Get();
	return nullptr;
}

void UCoopNetTestDriver::ServerTick(UWorld* World)
{
	if (Phase == TEXT("Boot"))
	{
		Assert(TEXT("DedicatedAuthority"), World->GetNetMode() == NM_DedicatedServer
			&& GameInstance->GetLocalPlayers().IsEmpty(), TEXT("DS has zero local players"));
		if (bDone) return;
		if (bVisualAcceptance && !PrepareVisualFixture(World)) return;
		// 固定数值的战斗/扩展夹具必须在首位玩家接纳前隔离地图 AI，不能让等候第二端的玩家先被小兵打伤。
		// 仅测试模式清场；正常关卡和专门的真实地图 AI 测试仍保留全部小兵。
		if (Mode == TEXT("Combat") || Mode == TEXT("CombatExpansion"))
		{
			for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) It->Destroy();
			for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It) It->Destroy();
			for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) It->Destroy();
		}
		SetPhase(TEXT("ReadyForPartner"));
		Emit(TEXT("READY"));
	}
	// 首次只有 Partner 进程获准连接；随后按稳定的网络 ID 识别，不能依赖重开后的登录顺序。
	if (PartnerPlayerId.IsEmpty())
		if (const auto* First = World->GetFirstPlayerController(); First && First->PlayerState)
			PartnerPlayerId = First->PlayerState->GetUniqueId().ToString();
	APlayerController* Partner = PartnerPlayer(World);
	if (Partner && Partner->GetCharacter() && !PartnerProbe.IsValid())
	{
		FActorSpawnParameters Params; Params.Owner = Partner;
		PartnerProbe = World->SpawnActor<ACoopNetTestProbe>(Params);
		if (!PartnerProbe.IsValid()) { Finish(false, TEXT("Could not create partner probe")); return; }
		PartnerProbe->Stage = TEXT("PartnerReady");
	}
	if (Phase == TEXT("ReadyForPartner"))
	{
		if (!Receipts.Contains(TEXT("PartnerReady"))) return;
		if (Mode == TEXT("CombatLifecycle")) PrepareLifecycleLateJoin(World);
		if (bDone) return;
		SetPhase(TEXT("ReadyForClient"));
		Emit(TEXT("PRIMARY_READY"));
	}
	if (Phase == TEXT("ReadyForClient"))
	{
		// 接纳握手可能晚于 Pawn/Probe 到达；所有场景在同一入口等服务器真正接纳双端。
		auto* Remote = Cast<AmultiplayerCoopPlayerController>(PrimaryPlayer(World));
		auto* PartnerSession = Cast<AmultiplayerCoopPlayerController>(Partner);
		if (!Remote || !PartnerSession || !Remote->GetPawn() || !PartnerSession->GetPawn()
			|| !Remote->HasAcceptedCombatSession() || !PartnerSession->HasAcceptedCombatSession()) return;
		FActorSpawnParameters Params; Params.Owner = Remote;
		Probe = Mode != TEXT("RideMotion")
			? World->SpawnActor<ACoopNetTestProbe>(Params)
			: World->SpawnActor<ACoopPlatformRideProbe>(Params);
		Assert(TEXT("Join"), Probe.IsValid(), TEXT("Two remote players; primary probe owned by second connection"));
		if (!Probe.IsValid()) return;
		// 旧场景专门验收终局死亡，不能因新加入的合作倒地把原断言悄悄改成另一种生命流程。
		if (Mode == TEXT("Combat") || Mode == TEXT("CombatLifecycle") || Mode == TEXT("CombatReconnect"))
			for (auto* PC : {Remote, PartnerSession})
				if (auto* Character = Cast<AmultiplayerCombatCharacter>(PC->GetPawn())) Character->GetCoopComponent()->bEnableDowned = false;
		SetPhase(TEXT("ScenarioStart"));
	}
	if (Mode == TEXT("Combat")) CombatServerTick(World);
	else if (Mode == TEXT("CombatTraffic")) TrafficServerTick(World);
	else if (Mode == TEXT("CombatReconnect")) ReconnectServerTick(World);
	else if (Mode == TEXT("CombatLifecycle")) LifecycleServerTick(World);
	else if (Mode == TEXT("CombatExpansion")) ExpansionServerTick(World);
	else if (Mode == TEXT("CoopEscape")) EscapeServerTick(World);
	else MotionServerTick(World);
	// 广播完成状态；服务器延迟退出，外层脚本还会要求 Partner 独立 DONE 才判通过。
	if (bDone && PartnerProbe.IsValid()) { PartnerProbe->Stage = TEXT("Complete"); PartnerProbe->ForceNetUpdate(); }
}

void UCoopNetTestDriver::PartnerTick(UWorld* World)
{
	if (World->GetNetMode() != NM_Client)
	{
		if (Phase == TEXT("Boot") && World->GetFirstPlayerController())
		{
			SetPhase(TEXT("Connecting"));
			GameInstance->ConnectToServer(ServerAddress);
		}
		return;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC || !PC->GetCharacter()) return;
	// Lifecycle 必须先验证真实输入和施法后的移动恢复，不能由夹具预先禁用移动。
	if (Mode != TEXT("CombatLifecycle") && Mode != TEXT("CombatExpansion") && Mode != TEXT("CoopEscape")) PC->GetCharacter()->GetCharacterMovement()->DisableMovement();
	if (!Probe.IsValid())
	{
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It)
			if (It->GetOwner() == PC) { Probe = *It; break; }
	}
	if (!Probe.IsValid()) return;
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (!State || State->GetObjectiveState().RequiredKeys <= 0) return;
	if (!SentReceipts.Contains(TEXT("PartnerReady")))
		SendReceipt(TEXT("PartnerReady"), true, TEXT("First remote client received pawn, owned probe and shared objective"));
	if (Mode == TEXT("Combat")) CombatClientTick(World);
	else if (Mode == TEXT("CombatTraffic")) TrafficClientTick(World);
	else if (Mode == TEXT("CombatReconnect")) ReconnectClientTick(World);
	else if (Mode == TEXT("CombatLifecycle")) LifecycleClientTick(World);
	else if (Mode == TEXT("CombatExpansion")) ExpansionClientTick(World);
	else if (Mode == TEXT("CoopEscape")) EscapeClientTick(World);
	if (Probe->Stage == TEXT("Complete"))
	{
		SendReceipt(TEXT("PartnerComplete"), true, TEXT("Partner observed completion"));
		Finish(true);
	}
}

void UCoopNetTestDriver::ClientTick(UWorld* World)
{
	if (World->GetNetMode() != NM_Client)
	{
		// 死亡拒绝后的合法重试走真实主动退出和全新会话，不复活已认证控制器的旧 Pawn。
		if (Mode == TEXT("Combat") && SentReceipts.Contains(TEXT("FreshTravelRequested"))
			&& !SentReceipts.Contains(TEXT("FreshConnectRequested")) && World->GetFirstPlayerController()
			&& GameInstance->GetConnectionState() == EMultiplayerConnectionState::Idle)
		{
			SentReceipts.Add(TEXT("FreshConnectRequested"));
			GameInstance->ConnectToServer(ServerAddress);
			return;
		}
		if (Phase == TEXT("Boot") && World->GetFirstPlayerController())
		{
			SetPhase(TEXT("Connecting"));
			GameInstance->ConnectToServer(ServerAddress);
		}
		return;
	}
	if (Probe.IsValid() && Probe->GetOwner() != World->GetFirstPlayerController()) Probe.Reset();
	if (!Probe.IsValid())
	{
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It)
			if (It->GetOwner() == World->GetFirstPlayerController()) { Probe = *It; break; }
		if (!Probe.IsValid()) return;
		if (!SentReceipts.Contains(TEXT("InitialJoinObserved")))
		{
			SentReceipts.Add(TEXT("InitialJoinObserved"));
			Assert(TEXT("Join"), true, TEXT("Remote-owned replicated test probe received after direct connection"));
		}
	}
	if (Mode == TEXT("Combat")) CombatClientTick(World);
	else if (Mode == TEXT("CombatTraffic")) TrafficClientTick(World);
	else if (Mode == TEXT("CombatReconnect")) ReconnectClientTick(World);
	else if (Mode == TEXT("CombatLifecycle")) LifecycleClientTick(World);
	else if (Mode == TEXT("CombatExpansion")) ExpansionClientTick(World);
	else if (Mode == TEXT("CoopEscape")) EscapeClientTick(World);
	else MotionClientTick(World);
}
