#include "Testing/CoopNetTestDriver.h"

#include "Blueprint/UserWidget.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Mechanisms/multiplayerCoopGate.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Mechanisms/multiplayerTransporterComponent.h"
#include "Mechanisms/multiplayerWinArea.h"
#include "Online/OnlineSessionNames.h"
#include "OnlineSessionSettings.h"
#include "OnlineSubsystem.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UnrealType.h"

namespace
{
	const FVector TestOrigin(30000, 0, 2000);
	double Clock() { return FPlatformTime::Seconds(); }

	APlayerController* RemotePlayer(UWorld* World)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			if (It->Get() && !It->Get()->IsLocalController() && It->Get()->GetCharacter()) return It->Get();
		return nullptr;
	}

	bool Flag(UObject* Object, const FName Name)
	{
		const FBoolProperty* Property = Object ? FindFProperty<FBoolProperty>(Object->GetClass(), Name) : nullptr;
		return Property && Property->GetPropertyValue_InContainer(Object);
	}

	// 反射只用于搭建测试关卡参数及只读断言，不直接写激活、胜利等运行状态。
	void Place(ACharacter* Character, const FVector& Location)
	{
		if (!Character) return;
		Character->GetCharacterMovement()->StopMovementImmediately();
		Character->GetCharacterMovement()->DisableMovement();
		Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		Character->ForceNetUpdate();
	}

	bool IsMenu(UWorld* World)
	{
		return World->GetNetMode() == NM_Standalone && World->GetMapName().EndsWith(TEXT("mainmenu"));
	}

#if DO_ENABLE_NET_TEST
	FPacketSimulationSettings SavedPackets;
#endif
}

void UCoopNetTestDriver::SetCommand(const FString& Command)
{
	SetPhase(Command);
	if (Probe.IsValid()) { Probe->Stage = Command; Probe->ForceNetUpdate(); }
}

void UCoopNetTestDriver::SendReceipt(const FString& Name, bool bPassed, const FString& Detail)
{
	if (!Probe.IsValid() || SentReceipts.Contains(Name)) return;
	SentReceipts.Add(Name);
	Assert(Name, bPassed, Detail);
	Probe->ServerReceipt(Name, bPassed, Detail);
}

void UCoopNetTestDriver::NetworkFailure(UWorld* World, UNetDriver*, ENetworkFailure::Type Type, const FString& Error)
{
	if (bDone || bHost || Mode != TEXT("Reconnect") || !World || World->GetGameInstance() != GameInstance.Get()) return;
	if (Type == ENetworkFailure::ConnectionLost || Type == ENetworkFailure::ConnectionTimeout)
	{
		bSawNetworkFailure = true;
		Assert(TEXT("ConnectionLostDetected"), true, Error);
		Probe.Reset();
	}
}

void UCoopNetTestDriver::CollectMapKeys(UWorld* World, bool bTwoPlayers)
{
	AmultiplayerCoopGameState* State = World->GetGameState<AmultiplayerCoopGameState>();
	ACharacter* Host = World->GetFirstPlayerController()->GetCharacter();
	ACharacter* Remote = RemotePlayer(World) ? RemotePlayer(World)->GetCharacter() : nullptr;
	const int32 Before = State->GetObjectiveState().ActivatedKeys;
	int32 Count = 0;
	for (TActorIterator<AmultiplayerCoopKey> It(World); It; ++It)
	{
		const FObjectPropertyBase* Destination = FindFProperty<FObjectPropertyBase>(It->GetClass(), TEXT("DestinationSocket"));
		if (!Destination || !Destination->GetObjectPropertyValue_InContainer(*It) || Flag(*It, TEXT("bInstalled"))) continue;
		const FVector Location = It->GetActorLocation();
		Place(Host, Location);
		if (bTwoPlayers) Place(Remote, Location);
		// 从碰撞入口触发，不调用 StoreCollectedKey/InstallAtSocket 来冒充拾取。
		if (!Flag(*It, TEXT("bInstalled"))) { Assert(TEXT("OverlapKeys"), false, It->GetName()); return; }
		++Count;
	}
	Assert(TEXT("OverlapKeys"), Count > 0 && State->GetObjectiveState().ActivatedKeys == Before + Count,
		FString::Printf(TEXT("Real overlap on %d prebound map keys; two server moves in one tick=%d; progress once per socket"), Count, bTwoPlayers));
	Place(Host, TestOrigin + FVector(0, -1000, 100));
	Place(Remote, TestOrigin + FVector(0, -1500, 100));
}

void UCoopNetTestDriver::CreateGateFixture(UWorld* World)
{
	Fixture.Reset();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Plate = World->SpawnActor<AmultiplayerPressurePlate>(TestOrigin + FVector(Index * 100, 0, 0), FRotator::ZeroRotator);
		Plate->bAlwaysRelevant = true;
		Fixture.Add(Plate);
	}
	const FTransform Transform(TestOrigin + FVector(0, 800, 0));
	auto* Gate = World->SpawnActorDeferred<AmultiplayerCoopGate>(AmultiplayerCoopGate::StaticClass(), Transform);
	Gate->bAlwaysRelevant = true;
	FArrayProperty* Plates = FindFProperty<FArrayProperty>(Gate->GetClass(), TEXT("RequiredPlates"));
	FScriptArrayHelper Array(Plates, Plates->ContainerPtrToValuePtr<void>(Gate));
	for (const auto& Plate : Fixture)
	{
		const int32 Index = Array.AddValue();
		CastFieldChecked<FObjectPropertyBase>(Plates->Inner)->SetObjectPropertyValue(Array.GetRawPtr(Index), Plate.Get());
	}
	FindFProperty<FIntProperty>(Gate->GetClass(), TEXT("RequiredActivePlateCount"))->SetPropertyValue_InContainer(Gate, 2);
	Gate->FinishSpawning(Transform);
	Fixture.Add(Gate);
	for (const auto& Actor : Fixture) Probe->Subjects.Add(Actor.Get());
}

void UCoopNetTestDriver::PrepareLateJoin(UWorld* World)
{
	CollectMapKeys(World, false);
	auto* Plate = World->SpawnActor<AmultiplayerPressurePlate>(TestOrigin, FRotator::ZeroRotator);
	Plate->bAlwaysRelevant = true;
	Fixture.Add(Plate);
	Place(World->GetFirstPlayerController()->GetCharacter(), TestOrigin + FVector(0, 0, 100));
	Assert(TEXT("LateJoinPrepared"), Plate->IsPlateActive(), TEXT("Plate active and objective complete before client process starts"));
}

void UCoopNetTestDriver::ScenarioHostTick(UWorld* World)
{
	APlayerController* Remote = RemotePlayer(World);
	AmultiplayerCoopGameState* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (Phase == TEXT("HostLeaving"))
	{
		if (IsMenu(World)) { Assert(TEXT("HostLeave"), true); Finish(true); }
		return;
	}
	if (Phase == TEXT("Restart"))
	{
		if (World != PreviousWorld.Get() && Remote && State && !State->GetObjectiveState().bGameWon && State->GetObjectiveState().ActivatedKeys == 0)
		{
			FActorSpawnParameters Params; Params.Owner = Remote;
			Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
			Assert(TEXT("Restart"), true, TEXT("New World, reset objectives and rejoined remote player"));
			SetCommand(TEXT("Restarted"));
		}
		return;
	}
	if (Phase == TEXT("Restarted"))
	{
		if (!Remote && Clock() - PhaseAt > 2)
		{
			SetPhase(TEXT("HostLeaving")); GameInstance->LeaveGame();
		}
		return;
	}
	if (Phase == TEXT("ScenarioStart"))
	{
		if (Mode == TEXT("SessionRetry"))
		{
			Assert(TEXT("SessionRetrySucceeded"), bSessionFailureChecked && Remote != nullptr);
			SetCommand(TEXT("Complete"));
		}
		else if (Mode == TEXT("LateJoin"))
		{
			Probe->Subject = Fixture[0].Get(); SetCommand(TEXT("LateJoinState"));
		}
		else SetCommand(TEXT("Freeze"));
	}
	if (Phase == TEXT("LateJoinState") && Receipts.Contains(TEXT("LateJoinState"))) SetCommand(TEXT("Complete"));
	if (Phase == TEXT("Freeze") && Receipts.Contains(TEXT("Freeze")))
	{
		if (Mode != TEXT("Ride")) CollectMapKeys(World, true);
		if (bDone) return;
		if (Mode == TEXT("Reconnect"))
		{
			PreviousRemote = Remote;
			World->GetNetDriver()->ConnectionTimeout = 5;
			SetCommand(TEXT("BeforeOutage"));
		}
		else if (Mode == TEXT("Ride")) SetCommand(TEXT("RidePrepare"));
		else
		{
			CreateGateFixture(World);
			Place(World->GetFirstPlayerController()->GetCharacter(), TestOrigin + FVector(50, 0, 100));
			SetCommand(TEXT("OnePlayer"));
		}
	}
	if (Phase == TEXT("OnePlayer") && Clock() - PhaseAt > 1)
	{
		const bool bBothActive = Cast<AmultiplayerPressurePlate>(Fixture[0])->IsPlateActive() && Cast<AmultiplayerPressurePlate>(Fixture[1])->IsPlateActive();
		Assert(TEXT("PlateDistinctPlayers"), bBothActive && !Flag(Fixture[2].Get(), TEXT("bGateOpen")), TEXT("One player covers two plates but cannot open a two-player gate"));
		Place(Remote->GetCharacter(), TestOrigin + FVector(50, 90, 100));
		SetCommand(TEXT("GateOpen"));
	}
	if (Phase == TEXT("GateOpen") && Receipts.Contains(TEXT("GateOpen")))
	{
		Assert(TEXT("GateRules"), Flag(Fixture[2].Get(), TEXT("bGateOpen")), TEXT("Occupancy changes while both active flags remain true; gate reevaluates"));
		Place(Remote->GetCharacter(), TestOrigin + FVector(0, -1000, 100));
		SetCommand(TEXT("GateClosed"));
	}
	if (Phase == TEXT("GateClosed") && Receipts.Contains(TEXT("GateClosed")))
	{
		Assert(TEXT("GateReclosed"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")), TEXT("Leaving player closes gate although host still activates both plates"));
		Place(Remote->GetCharacter(), TestOrigin + FVector(50, 90, 100));
		Assert(TEXT("GateReopened"), Flag(Fixture[2].Get(), TEXT("bGateOpen")));
		Remote->GetCharacter()->Destroy();
		Assert(TEXT("DestroyedPawnCleanup"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")) &&
			Fixture[0]->FindComponentByClass<UmultiplayerPlayerOccupancyComponent>()->GetPlayerCount() == 1,
			TEXT("Destroy actual remote pawn while overlapping; remaining host counts once and gate closes"));
		if (bDone) return;
		World->GetAuthGameMode<AmultiplayerGameMode>()->RestartPlayer(Remote);
		SetCommand(TEXT("RidePrepare"));
	}
	if (Phase == TEXT("RidePrepare") && Receipts.Contains(TEXT("RidePrepare")))
	{
		const FVector Start = TestOrigin + FVector(0, 3000, 0);
		Place(Remote->GetCharacter(), Start + FVector(0, 0, 115));
		Remote->GetCharacter()->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		SetCommand(TEXT("Boarding"));
	}
	if (Phase == TEXT("RidePrepare") && !Probe->Subject)
	{
		// 先让平台复制到客户端，再放置乘客；否则 CMC 的相对基座校正可能先于平台 Actor 到达。
		const FVector Start = TestOrigin + FVector(0, 3000, 0);
		const FTransform Transform(Start);
		auto* Platform = World->SpawnActorDeferred<AmultiplayerMovingPlatform>(AmultiplayerMovingPlatform::StaticClass(), Transform);
		// 原生默认是外部压力板模式；测试须在 BeginPlay 绑定之前明确选择自身占用模式。
		*FindFProperty<FEnumProperty>(Platform->GetClass(), TEXT("ActivationSource"))->ContainerPtrToValuePtr<EMovingPlatformActivationSource>(Platform)
			= EMovingPlatformActivationSource::PlatformOccupancy;
		Platform->FinishSpawning(Transform);
		Platform->bAlwaysRelevant = true;
		Platform->FindComponentByClass<UBoxComponent>()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Platform->FindComponentByClass<UmultiplayerTransporterComponent>()->ConfigureWorldTargets(Start, Start + FVector(600, 0, 0));
		Probe->Subject = Platform; Probe->Endpoint = Start + FVector(600, 0, 0);
		Probe->ForceNetUpdate();
	}
	if (Phase == TEXT("Boarding") && Receipts.Contains(TEXT("Boarding")))
	{
		// 仅搭建测试初始站立条件；重新启用实际触发体后，仍由平台人数规则启动运动。
		Probe->Subject->FindComponentByClass<UBoxComponent>()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		SetCommand(TEXT("Ride"));
	}
	if ((Phase == TEXT("Boarding") || Phase == TEXT("Ride")) && Clock() - PhaseAt > 20)
	{
		Assert(TEXT("PlatformEndpoint"), false, FString::Printf(TEXT("Stage=%s platform=%s target=%s pawn=%s base=%s"),
			*Phase, *Probe->Subject->GetActorLocation().ToString(), *Probe->Endpoint.ToString(),
			*Remote->GetCharacter()->GetActorLocation().ToString(), *GetNameSafe(Remote->GetCharacter()->GetMovementBase())));
		return;
	}
	if (Phase == TEXT("Ride") && Receipts.Contains(TEXT("ClientRide")))
	{
		Assert(TEXT("PlatformEndpoint"), Probe->Subject && Probe->Subject->GetActorLocation().Equals(Probe->Endpoint, 3), TEXT("Server platform reached configured endpoint"));
		if (Mode == TEXT("Ride")) SetCommand(TEXT("Complete"));
		else
		{
			auto* Win = World->SpawnActor<AmultiplayerWinArea>(TestOrigin + FVector(0, 5000, 0), FRotator::ZeroRotator);
			Place(World->GetFirstPlayerController()->GetCharacter(), Win->GetActorLocation() + FVector(0, 0, 70));
			Place(Remote->GetCharacter(), Win->GetActorLocation() + FVector(90, 0, 70));
			SetCommand(TEXT("Victory"));
		}
	}
	if (Phase == TEXT("Victory") && Receipts.Contains(TEXT("ClientVictoryState")))
	{
		// 注入只覆盖“旧世界仍存活”的失败恢复，不声称验证所有损坏地图和引擎错误路径。
		auto* GM = World->GetAuthGameMode<AmultiplayerGameMode>();
		const bool bFirstAccepted = GM->RequestRestartCurrentRound(Remote);
		Assert(TEXT("RestartFailureRecovery"), !bFirstAccepted && World->NextURL.IsEmpty() && GM->GetMatchState() != MatchState::LeavingMap,
			TEXT("Synthetic old-world travel failure releases restart lock and restores match state"));
		if (bDone) return;
		PreviousWorld = World; SetCommand(TEXT("Restart"));
	}
#if DO_ENABLE_NET_TEST
	if (Phase == TEXT("BeforeOutage") && Receipts.Contains(TEXT("BeforeOutage")))
	{
		UNetDriver* Net = World->GetNetDriver();
		SavedPackets = Net->PacketSimulationSettings;
		FPacketSimulationSettings Drop = SavedPackets;
		Drop.PktLag = Drop.PktLagVariance = Drop.PktLagMin = Drop.PktLagMax = 0;
		Drop.PktLoss = Drop.PktIncomingLoss = 100;
		Net->SetPacketSimulationSettings(Drop);
		Assert(TEXT("OutageApplied"), true, FString::Printf(TEXT("Real 100%% inbound/outbound packet loss for %.1f seconds; ConnectionTimeout=5 test-only"), OutageSeconds));
		SetPhase(TEXT("Outage"));
	}
	if (Phase == TEXT("Outage") && Clock() - PhaseAt >= OutageSeconds)
	{
		World->GetNetDriver()->SetPacketSimulationSettings(SavedPackets);
		// 与业务 GameInstance 使用同一子系统接口；缓存地址能重连并不证明房间仍可被搜索。
		IOnlineSubsystem* Subsystem = IOnlineSubsystem::Get();
		const IOnlineSessionPtr Sessions = Subsystem ? Subsystem->GetSessionInterface() : nullptr;
		const FNamedOnlineSession* Session = Sessions.IsValid() ? Sessions->GetNamedSession(NAME_GameSession) : nullptr;
		FString AdvertisedName;
		const bool bMatchingToken = Session && Session->SessionSettings.Get(FName(TEXT("SERVER_NAME")), AdvertisedName)
			&& AdvertisedName == Token;
		Assert(TEXT("HostSessionRetained"), Session && Session->bHosting && Session->SessionSettings.bShouldAdvertise && bMatchingToken,
			FString::Printf(TEXT("After remote timeout: hostSession=%d hosting=%d advertised=%d matchingRunToken=%d"),
				Session != nullptr, Session && Session->bHosting, Session && Session->SessionSettings.bShouldAdvertise, bMatchingToken));
		if (bDone) return;
		SetPhase(TEXT("AwaitReconnect"));
	}
	if (Phase == TEXT("AwaitReconnect") && Remote && Remote != PreviousRemote.Get())
	{
		if (Probe.IsValid()) Probe->Destroy();
		FActorSpawnParameters Params; Params.Owner = Remote;
		Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
		Assert(TEXT("ReconnectAfterOutage"), true, TEXT("New remote controller after real timeout; server world retained"));
		SetCommand(TEXT("Reconnected"));
	}
#endif
	if (Phase == TEXT("Reconnected") && Receipts.Contains(TEXT("ReconnectStateRestored"))) SetCommand(TEXT("Complete"));
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))) Finish(true);
}

void UCoopNetTestDriver::ScenarioClientTick(UWorld* World)
{
	if (Phase == TEXT("Leaving"))
	{
		if (IsMenu(World)) { Assert(TEXT("ClientLeave"), true); Finish(true); }
		return;
	}
	if (Phase == TEXT("Restarting"))
	{
		if (World == PreviousWorld.Get() || World->GetNetMode() != NM_Client) return;
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It) { Probe = *It; break; }
		if (!Probe.IsValid() || Probe->Stage != TEXT("Restarted")) return;
		Assert(TEXT("ClientRestart"), true, TEXT("Client loaded replacement world and received new server probe"));
		SetPhase(TEXT("Leaving")); GameInstance->LeaveGame(); return;
	}
	if (!Probe.IsValid()) return;
	const FString Command = Probe->Stage;
	APlayerController* PC = World->GetFirstPlayerController();
	ACharacter* Character = PC ? PC->GetCharacter() : nullptr;
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (!Character || !State) return;
	if (Command == TEXT("Freeze"))
	{
		Character->GetCharacterMovement()->DisableMovement();
		SendReceipt(Command, true, TEXT("Client input frozen; server teleports drive real overlaps"));
	}
	if (Command == TEXT("LateJoinState") && Probe->Subject)
	{
		auto* Plate = Cast<AmultiplayerPressurePlate>(Probe->Subject);
		if (State->IsObjectiveComplete() && Plate && Plate->IsPlateActive())
			SendReceipt(Command, true, TEXT("Initial replication restores already completed objectives and active dormant plate"));
	}
	if ((Command == TEXT("GateOpen") || Command == TEXT("GateClosed")) && Probe->Subjects.Num() == 3)
	{
		const bool bOpen = Command == TEXT("GateOpen");
		if (Probe->Subjects[2] && Flag(Probe->Subjects[2], TEXT("bGateOpen")) == bOpen)
		{
			auto* Mesh = Probe->Subjects[2]->FindComponentByClass<UStaticMeshComponent>();
			const float ExpectedZ = bOpen ? 600.f : 200.f;
			if (Mesh && FMath::IsNearlyEqual(Mesh->GetRelativeLocation().Z, ExpectedZ, 2.f))
				SendReceipt(Command, true, TEXT("Client state and local door mesh converged after dormancy flush"));
		}
	}
	if (Command == TEXT("RidePrepare") && Probe->Subject && Probe->Subject->FindComponentByClass<UStaticMeshComponent>())
	{
		Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		SendReceipt(Command, true, TEXT("Release local movement for actual moving-base test"));
	}
	if (Command == TEXT("Boarding") && Character->GetMovementBase() && Character->GetMovementBase()->GetOwner() == Probe->Subject)
		SendReceipt(Command, true, TEXT("Client resolved platform and actually stands on its movement base"));
	if (Command == TEXT("Ride") && Probe->Subject)
	{
		if (Phase != Command) { SetPhase(Command); RideStart = Character->GetActorLocation(); }
		++RideSamples;
		if (Character->GetMovementBase() && Character->GetMovementBase()->GetOwner() == Probe->Subject) ++RideBasedSamples;
		const FVector Delta = Character->GetActorLocation() - Probe->Subject->GetActorLocation();
		if (Clock() - PhaseAt > 1) RideMaxOffset = FMath::Max(RideMaxOffset, FVector(Delta.X, Delta.Y, 0).Size());
		if (Probe->Subject->GetActorLocation().Equals(Probe->Endpoint, 3) && Clock() - PhaseAt > 5)
		{
			const float BasedRatio = float(RideBasedSamples) / FMath::Max(1, RideSamples);
			SendReceipt(TEXT("ClientRide"), BasedRatio >= .7f && RideMaxOffset < 100 && Character->GetActorLocation().X - RideStart.X > 250,
				FString::Printf(TEXT("Real CMC base samples=%d/%d; max horizontal offset=%.1f cm; no visual smoothness claim"), RideBasedSamples, RideSamples, RideMaxOffset));
		}
	}
	if (Command == TEXT("Victory") && State->GetObjectiveState().bGameWon)
	{
		const FObjectPropertyBase* WidgetProperty = FindFProperty<FObjectPropertyBase>(PC->GetClass(), TEXT("VictoryWidget"));
		auto* Widget = WidgetProperty ? Cast<UUserWidget>(WidgetProperty->GetObjectPropertyValue_InContainer(PC)) : nullptr;
		if (Widget && Widget->IsInViewport()) SendReceipt(TEXT("ClientVictoryState"), true, TEXT("Remote GameState won and real local VictoryWidget added to viewport; not pixel validation"));
	}
	if (Command == TEXT("Restart") && Phase != TEXT("Restarting"))
	{
		PreviousWorld = World; SetPhase(TEXT("Restarting"));
		CastChecked<AmultiplayerCoopPlayerController>(PC)->RequestRestartCurrentRound();
		Probe.Reset();
	}
	if (Command == TEXT("BeforeOutage") && State->IsObjectiveComplete())
	{
		World->GetNetDriver()->ConnectionTimeout = 5;
		SendReceipt(Command, true, TEXT("Client has authoritative progress before actual packet outage"));
	}
	if (Command == TEXT("Reconnected") && State->IsObjectiveComplete())
		SendReceipt(TEXT("ReconnectStateRestored"), bSawNetworkFailure, TEXT("New connection restored shared progress, not old pawn identity"));
	if (Command == TEXT("Complete"))
	{
		SendReceipt(Command, true, TEXT("Client finished all requested assertions")); Finish(true);
	}
}
