#include "Testing/CoopNetTestDriver.h"

#include "Blueprint/UserWidget.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Mechanisms/multiplayerCoopGate.h"
#include "Mechanisms/multiplayerCoopCarryComponent.h"
#include "Mechanisms/multiplayerCoopKey.h"
#include "Mechanisms/multiplayerKeySocket.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Mechanisms/multiplayerTransporterComponent.h"
#include "Mechanisms/multiplayerWinArea.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Player/multiplayerCharacter.h"
#include "UI/multiplayerVictoryWidget.h"
#include "UObject/UnrealType.h"

namespace
{
	const FVector TestOrigin(30000, 0, 2000);
	double Clock() { return FPlatformTime::Seconds(); }

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
		return World->GetNetMode() == NM_Standalone && World->GetMapName().EndsWith(TEXT("DSMenu"));
	}

}

void UCoopNetTestDriver::SetCommand(const FString& Command)
{
	SetPhase(Command);
	if (Probe.IsValid()) { Probe->Stage = Command; Probe->ForceNetUpdate(); }
	if (PartnerProbe.IsValid()) { PartnerProbe->Stage = Command; PartnerProbe->ForceNetUpdate(); }
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
	if (bDone || bServer || Mode != TEXT("Reconnect") || !World || World->GetGameInstance() != GameInstance.Get()) return;
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
	ACharacter* Partner = PartnerPlayer(World)->GetCharacter();
	ACharacter* Remote = PrimaryPlayer(World) ? PrimaryPlayer(World)->GetCharacter() : nullptr;
	const int32 Before = State->GetObjectiveState().ActivatedKeys;
	int32 Count = 0;
	for (TActorIterator<AmultiplayerCoopKey> It(World); It; ++It)
	{
		const FObjectPropertyBase* Destination = FindFProperty<FObjectPropertyBase>(It->GetClass(), TEXT("DestinationSocket"));
		if (!Destination || !Destination->GetObjectPropertyValue_InContainer(*It) || Flag(*It, TEXT("bInstalled"))) continue;
		const FVector Location = It->GetActorLocation();
		Place(Partner, Location);
		if (bTwoPlayers) Place(Remote, Location);
		// 从碰撞入口触发，不调用 StoreCollectedKey/InstallAtSocket 来冒充拾取。
		if (!Flag(*It, TEXT("bInstalled"))) { Assert(TEXT("OverlapKeys"), false, It->GetName()); return; }
		++Count;
	}
	Assert(TEXT("OverlapKeys"), Count > 0 && State->GetObjectiveState().ActivatedKeys == Before + Count,
		FString::Printf(TEXT("Real overlap on %d prebound map keys; two server moves in one tick=%d; progress once per socket"), Count, bTwoPlayers));
	Place(Partner, TestOrigin + FVector(0, -1000, 100));
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
	for (TActorIterator<AmultiplayerCoopKey> It(World); It; ++It)
	{
		if (!Flag(*It, TEXT("bInstalled"))) continue;
		It->bAlwaysRelevant = true;
		if (AActor* Parent = It->GetAttachParentActor()) Parent->bAlwaysRelevant = true;
		Fixture.Add(*It);
	}
	Place(PartnerPlayer(World)->GetCharacter(), TestOrigin + FVector(0, 0, 100));
	Assert(TEXT("LateJoinPrepared"), Plate->IsPlateActive(), TEXT("Plate active and objective complete before client process starts"));
}

void UCoopNetTestDriver::ScenarioServerTick(UWorld* World)
{
	if (Mode == TEXT("Keys")) { KeyServerTick(World); return; }
	APlayerController* Remote = PrimaryPlayer(World);
	AmultiplayerCoopGameState* State = World->GetGameState<AmultiplayerCoopGameState>();
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
			Assert(TEXT("ServerSurvivesClientLeave"), World->GetNetMode() == NM_DedicatedServer
				&& PartnerPlayer(World) && PartnerProbe.IsValid(), TEXT("Primary left; DS and partner remain"));
			Finish(true);
		}
		return;
	}
	if (Phase == TEXT("ScenarioStart"))
	{
		if (Mode == TEXT("ConnectionRetry"))
		{
			Assert(TEXT("ConnectionRetrySucceeded"), Remote != nullptr);
			SetCommand(TEXT("Complete"));
		}
		else if (Mode == TEXT("LateJoin"))
		{
			for (int32 Index = 1; Index < Fixture.Num(); ++Index) Probe->Subjects.Add(Fixture[Index].Get());
			Probe->Subject = Fixture[0].Get(); SetCommand(TEXT("LateJoinState"));
		}
		else SetCommand(TEXT("Freeze"));
	}
	if (Phase == TEXT("LateJoinState") && Receipts.Contains(TEXT("LateJoinState"))
		&& Receipts.Contains(TEXT("LateJoinKeyAttachments"))) SetCommand(TEXT("Complete"));
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
			Place(PartnerPlayer(World)->GetCharacter(), TestOrigin + FVector(50, 0, 100));
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
		ACharacter* StandingCharacter = Remote->GetCharacter();
		Remote->UnPossess();
		Assert(TEXT("OccupancyUnpossess"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")),
			TEXT("Unpossess while still overlapping removes player eligibility without EndOverlap"));
		Remote->Possess(StandingCharacter);
		Assert(TEXT("OccupancyRepossess"), Flag(Fixture[2].Get(), TEXT("bGateOpen")),
			TEXT("Possess existing overlap candidate restores eligibility and gate state"));
		// 再验证首次进入时尚未被玩家控制的候选，不只是已计数玩家的重新控制。
		Remote->UnPossess();
		Place(StandingCharacter, TestOrigin + FVector(0, -1000, 100));
		Place(StandingCharacter, TestOrigin + FVector(50, 90, 100));
		Assert(TEXT("OccupancyUncontrolledEntry"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")));
		Remote->Possess(StandingCharacter);
		Assert(TEXT("OccupancyLatePossess"), Flag(Fixture[2].Get(), TEXT("bGateOpen")),
			TEXT("Initially uncontrolled overlap is tracked and becomes eligible on Possess"));
		if (bDone) return;
		Place(Remote->GetCharacter(), TestOrigin + FVector(0, -1000, 100));
		SetCommand(TEXT("GateClosed"));
	}
	if (Phase == TEXT("GateClosed") && Receipts.Contains(TEXT("GateClosed")))
	{
		Assert(TEXT("GateReclosed"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")), TEXT("Leaving player closes gate although partner still activates both plates"));
		Place(Remote->GetCharacter(), TestOrigin + FVector(50, 90, 100));
		Assert(TEXT("GateReopened"), Flag(Fixture[2].Get(), TEXT("bGateOpen")));
		Remote->GetCharacter()->Destroy();
		Assert(TEXT("DestroyedPawnCleanup"), !Flag(Fixture[2].Get(), TEXT("bGateOpen")) &&
			Fixture[0]->FindComponentByClass<UmultiplayerPlayerOccupancyComponent>()->GetPlayerCount() == 1,
			TEXT("Destroy actual remote pawn while overlapping; remaining partner counts once and gate closes"));
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
			Place(PartnerPlayer(World)->GetCharacter(), Win->GetActorLocation() + FVector(0, 0, 70));
			Place(Remote->GetCharacter(), Win->GetActorLocation() + FVector(90, 0, 70));
			SetCommand(TEXT("Victory"));
		}
	}
	if (Phase == TEXT("Victory") && Receipts.Contains(TEXT("ClientVictoryState")) && Receipts.Contains(TEXT("PartnerVictory")))
	{
		SetCommand(TEXT("RestartFailure"));
	}
	if (Phase == TEXT("RestartFailure") && Receipts.Contains(TEXT("ClientRestartFailureRecovery")))
	{
		// 失败必须从实际按钮、Controller RPC、GameMode 到客户端反馈走完，不能只调用规则层。
		auto* GM = World->GetAuthGameMode<AmultiplayerGameMode>();
		Assert(TEXT("RestartFailureRecovery"), World->NextURL.IsEmpty() && GM->GetMatchState() != MatchState::LeavingMap,
			TEXT("Real button request failed; old world and match state recovered; client acknowledged retry readiness"));
		if (bDone) return;
		PreviousWorld = World; SetCommand(TEXT("Restart"));
	}
#if DO_ENABLE_NET_TEST
	if (Phase == TEXT("BeforeOutage") && Receipts.Contains(TEXT("BeforeOutage")))
	{
		// 只切断被测玩家的服务器出站包；不能把另一客户端一起断开来冒充 DS 隔离。
		UNetConnection* Connection = Remote ? Remote->GetNetConnection() : nullptr;
		if (!Connection) { Finish(false, TEXT("Missing remote net connection")); return; }
		Connection->PacketSimulationSettings.PktLag = 0;
		Connection->PacketSimulationSettings.PktLoss = 100;
		Assert(TEXT("OutageApplied"), true, TEXT("100% server outbound loss on primary connection until its real timeout; partner unaffected"));
		SetPhase(TEXT("AwaitReconnect"));
	}
	if (Phase == TEXT("AwaitReconnect") && Remote && Remote != PreviousRemote.Get())
	{
		if (Probe.IsValid()) Probe->Destroy();
		FActorSpawnParameters Params; Params.Owner = Remote;
		Probe = World->SpawnActor<ACoopNetTestProbe>(Params);
		Assert(TEXT("ServerConnectionRetained"), World->GetNetMode() == NM_DedicatedServer
			&& PartnerProbe.IsValid() && PartnerProbe->GetOwner() == PartnerPlayer(World),
			TEXT("DS and original partner connection survived primary timeout"));
		Assert(TEXT("ReconnectAfterOutage"), true, TEXT("New remote controller after real timeout; server world retained"));
		SetCommand(TEXT("Reconnected"));
	}
#endif
	if (Phase == TEXT("Reconnected") && Receipts.Contains(TEXT("ReconnectStateRestored"))) SetCommand(TEXT("Complete"));
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))) Finish(true);
}

// 独立用例沿用现有会话、回执和计时框架，不修改正式玩法目标数；仅为此测试初始化两格进度。
void UCoopNetTestDriver::KeyServerTick(UWorld* World)
{
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))) { Finish(true); return; }
	APlayerController* PartnerPC = PartnerPlayer(World);
	APlayerController* RemotePC = PrimaryPlayer(World);
	if (!PartnerPC || !RemotePC) return;
	ACharacter* Partner = PartnerPC->GetCharacter();
	ACharacter* Remote = RemotePC->GetCharacter();
	auto* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (!Partner || !Remote || !State) return;
	auto* Carry = Partner->FindComponentByClass<UmultiplayerCoopCarryComponent>();
	if (Phase == TEXT("ScenarioStart")) SetCommand(TEXT("Freeze"));
	if (Phase == TEXT("Freeze") && Receipts.Contains(TEXT("Freeze")))
	{
		Place(Partner, TestOrigin + FVector(-1000, 0, 0));
		Place(Remote, TestOrigin + FVector(-1500, 0, 0));
		FmultiplayerCoopObjectiveState Initial;
		Initial.RequiredKeys = 2;
		State->ApplyAuthoritativeState(Initial);
		auto* Key = World->SpawnActor<AmultiplayerCoopKey>(TestOrigin, FRotator::ZeroRotator);
		auto* Socket = World->SpawnActor<AmultiplayerKeySocket>(TestOrigin + FVector(0, 500, 0), FRotator::ZeroRotator);
		Key->bAlwaysRelevant = Socket->bAlwaysRelevant = true;
		Fixture = {Key, Socket};
		Probe->Subjects = {Key, Socket, Partner};
		Place(Partner, Key->GetActorLocation());
		Assert(TEXT("KeyHeld"), Key->IsHeldBy(Partner) && Carry && Carry->GetCarriedKey() == Key
			&& Key->GetAttachParentActor() == Partner, TEXT("Real overlap acquires key and server carry slot"));
		if (!bDone) SetCommand(TEXT("KeyHeld"));
	}
	if (Phase == TEXT("KeyHeld") && Receipts.Contains(TEXT("ClientKeyHeld")))
	{
		auto* Key = CastChecked<AmultiplayerCoopKey>(Fixture[0]);
		auto* Socket = CastChecked<AmultiplayerKeySocket>(Fixture[1]);
		// 仅替换测试配置，模拟无效安装目标；不直接改写 Holder、Installed 或激活状态。
		auto* DisplayProperty = FindFProperty<FObjectPropertyBase>(Socket->GetClass(), TEXT("KeyDisplayPoint"));
		UObject* DisplayPoint = DisplayProperty->GetObjectPropertyValue_InContainer(Socket);
		DisplayProperty->SetObjectPropertyValue_InContainer(Socket, Key->GetRootComponent());
		const bool bRejected = !Socket->StoreCollectedKey(Key);
		DisplayProperty->SetObjectPropertyValue_InContainer(Socket, DisplayPoint);
		Assert(TEXT("KeyInstallFailureRollback"), bRejected && Key->IsHeldBy(Partner)
			&& Carry->GetCarriedKey() == Key && Key->GetAttachParentActor() == Partner
			&& State->GetObjectiveState().ActivatedKeys == 0 && !Flag(Key, TEXT("bInstalled")),
			TEXT("Failed key operation preserves holder, attachment, inventory and progress"));
		if (bDone) return;
		bool bNestedOperationRan = false;
		auto* GM = World->GetAuthGameMode<AmultiplayerGameMode>();
		const bool bOuterCommitted = GM->RegisterActivatedKey([&]()
		{
			GM->RegisterActivatedKey([&]() { bNestedOperationRan = true; return true; });
			return false;
		});
		Assert(TEXT("KeyCommitReentryBlocked"), !bOuterCommitted && !bNestedOperationRan
			&& State->GetObjectiveState().ActivatedKeys == 0);
		if (bDone) return;
		const bool bInstalled = Socket->StoreCollectedKey(Key);
		Assert(TEXT("KeyInstalledOnce"), bInstalled && Flag(Key, TEXT("bInstalled"))
			&& Carry->GetCarriedKey() == nullptr && Key->GetAttachParentActor() == Socket
			&& !Socket->StoreCollectedKey(Key) && State->GetObjectiveState().ActivatedKeys == 1);
		if (!bDone) SetCommand(TEXT("KeyInstalled"));
	}
	if (Phase == TEXT("KeyInstalled") && Receipts.Contains(TEXT("ClientKeyInstalled")))
	{
		auto* Key = World->SpawnActor<AmultiplayerCoopKey>(TestOrigin + FVector(600, 0, 0), FRotator::ZeroRotator);
		auto* Socket = World->SpawnActor<AmultiplayerKeySocket>(TestOrigin + FVector(1000, 0, 0), FRotator::ZeroRotator);
		Place(Partner, Key->GetActorLocation());
		Place(Partner, Socket->GetActorLocation());
		Assert(TEXT("KeyConsumeCommitted"), !IsValid(Key) && Carry->GetCarriedKey() == nullptr
			&& State->GetObjectiveState().ActivatedKeys == 2, TEXT("Socket overlap consumes key and clears carry slot"));
		if (bDone) return;
		Key = World->SpawnActor<AmultiplayerCoopKey>(TestOrigin + FVector(1600, 0, 0), FRotator::ZeroRotator);
		Socket = World->SpawnActor<AmultiplayerKeySocket>(TestOrigin + FVector(2000, 0, 0), FRotator::ZeroRotator);
		Assert(TEXT("KeyInstallRejectedRetainsKey"), !Socket->StoreCollectedKey(Key)
			&& IsValid(Key) && !Flag(Key, TEXT("bInstalled")) && Key->GetAttachParentActor() == nullptr,
			TEXT("Completed objective refuses installation before changing the key"));
		if (bDone) return;
		Place(Partner, Key->GetActorLocation());
		Place(Partner, Socket->GetActorLocation());
		Assert(TEXT("KeyConsumeRejectedRetainsKey"), IsValid(Key) && Key->IsHeldBy(Partner)
			&& Carry->GetCarriedKey() == Key && Key->GetAttachParentActor() == Partner
			&& Socket->FindComponentByClass<UBoxComponent>()->GetCollisionEnabled() == ECollisionEnabled::QueryOnly
			&& State->GetObjectiveState().ActivatedKeys == 2, TEXT("Rejected progress cannot consume the held key"));
		if (bDone) return;
		Key->Destroy();
		Assert(TEXT("KeyDestroyClearsCarry"), Carry->GetCarriedKey() == nullptr);
		if (bDone) return;
		// 用临时 Pawn 验证真实 OnDestroyed；恢复原协作玩家 Pawn，不破坏连接。
		auto* TemporaryHolder = World->SpawnActor<AmultiplayerCharacter>(TestOrigin + FVector(2600, 0, 0), FRotator::ZeroRotator);
		PartnerPC->Possess(TemporaryHolder);
		Key = World->SpawnActor<AmultiplayerCoopKey>(TestOrigin + FVector(3000, 0, 0), FRotator::ZeroRotator);
		Key->bAlwaysRelevant = true;
		Place(TemporaryHolder, Key->GetActorLocation());
		const bool bWasHeld = Key->IsHeldBy(TemporaryHolder);
		Place(Remote, Key->GetActorLocation());
		TemporaryHolder->Destroy();
		PartnerPC->Possess(Partner);
		const auto* RemoteCarry = Remote->FindComponentByClass<UmultiplayerCoopCarryComponent>();
		Assert(TEXT("KeyDropRepick"), bWasHeld && Key->IsHeldBy(Remote) && RemoteCarry
			&& RemoteCarry->GetCarriedKey() == Key && Key->GetAttachParentActor() == Remote,
			TEXT("Collision restoration repicks immediately; old drop path must not detach new holder"));
		Probe->Subjects = {Key, Socket, Remote};
		if (!bDone) SetCommand(TEXT("KeyRepicked"));
	}
	if (Phase == TEXT("KeyRepicked") && Receipts.Contains(TEXT("ClientKeyRepicked"))) SetCommand(TEXT("Complete"));
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
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It)
			if (It->GetOwner() == World->GetFirstPlayerController()) { Probe = *It; break; }
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
	if (Mode == TEXT("Keys") && Probe->Subjects.Num() == 3)
	{
		auto* Key = Cast<AmultiplayerCoopKey>(Probe->Subjects[0]);
		auto* Socket = Cast<AmultiplayerKeySocket>(Probe->Subjects[1]);
		auto* Holder = Cast<ACharacter>(Probe->Subjects[2]);
		if (Key && Holder && Key->IsHeldBy(Holder) && Key->GetAttachParentActor() == Holder)
		{
			if (Command == TEXT("KeyHeld")) SendReceipt(TEXT("ClientKeyHeld"), true, TEXT("Client resolves held key and native attachment"));
			if (Command == TEXT("KeyRepicked")) SendReceipt(TEXT("ClientKeyRepicked"), true, TEXT("Client resolves new holder after destruction and repick"));
		}
		if (Command == TEXT("KeyInstalled") && Key && Socket && Flag(Key, TEXT("bInstalled"))
			&& !Key->IsHeldBy(Holder) && Key->GetAttachParentActor() == Socket
			&& Key->GetActorLocation().Equals(Socket->GetActorLocation() + FVector(0, 0, 75), 3))
			SendReceipt(TEXT("ClientKeyInstalled"), true, TEXT("Held-to-installed converges through native attachment replication"));
	}
	if (Command == TEXT("LateJoinState") && Probe->Subject)
	{
		auto* Plate = Cast<AmultiplayerPressurePlate>(Probe->Subject);
		if (State->IsObjectiveComplete() && Plate && Plate->IsPlateActive())
			SendReceipt(Command, true, TEXT("Initial replication restores already completed objectives and active dormant plate"));
		int32 AttachedKeys = 0;
		for (AActor* Actor : Probe->Subjects)
		{
			auto* Key = Cast<AmultiplayerCoopKey>(Actor);
			if (Key && Flag(Key, TEXT("bInstalled")) && Cast<AmultiplayerKeySocket>(Key->GetAttachParentActor())) ++AttachedKeys;
		}
		if (AttachedKeys > 0 && AttachedKeys == Probe->Subjects.Num() && AttachedKeys == State->GetObjectiveState().ActivatedKeys)
			SendReceipt(TEXT("LateJoinKeyAttachments"), true, TEXT("All installed map keys resolve native socket attachments on late join"));
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
		const FObjectPropertyBase* WidgetProperty = FindFProperty<FObjectPropertyBase>(PC->GetClass(), TEXT("VictoryWidget"));
		auto* Widget = WidgetProperty ? Cast<UmultiplayerVictoryWidget>(WidgetProperty->GetObjectPropertyValue_InContainer(PC)) : nullptr;
		if (Widget == nullptr) return;
		PreviousWorld = World; SetPhase(TEXT("Restarting"));
		Widget->HandleRestartClicked();
		Probe.Reset();
	}
	if (Command == TEXT("RestartFailure"))
	{
		const FObjectPropertyBase* WidgetProperty = FindFProperty<FObjectPropertyBase>(PC->GetClass(), TEXT("VictoryWidget"));
		auto* Widget = WidgetProperty ? Cast<UmultiplayerVictoryWidget>(WidgetProperty->GetObjectPropertyValue_InContainer(PC)) : nullptr;
		if (Widget == nullptr) return;
		if (!SentReceipts.Contains(TEXT("RestartFailureClick")))
		{
			SentReceipts.Add(TEXT("RestartFailureClick"));
			Widget->HandleRestartClicked();
			Widget->HandleRestartClicked(); // 重复点击不能提交第二次有效请求。
		}
		if (CastChecked<AmultiplayerCoopPlayerController>(PC)->GetVictoryAction() == ECoopVictoryAction::Idle)
		{
			SendReceipt(TEXT("ClientRestartFailureRecovery"), Widget->IsInViewport()
				&& Widget->GetIsEnabled(),
				TEXT("Actual restart button + server failure + client receipt restored Idle in the same widget"));
		}
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
