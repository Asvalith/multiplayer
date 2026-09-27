#include "Testing/CoopNetTestDriver.h"
#include "Testing/CoopPlatformRideProbe.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerPressurePlate.h"
#include "Mechanisms/multiplayerTransporterComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Net/UnrealNetwork.h"
#include "ProfilingDebugging/CsvProfiler.h"
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
	DOREPLIFETIME(ACoopNetTestProbe, Endpoint);
	DOREPLIFETIME(ACoopNetTestProbe, Subjects);
}

void ACoopNetTestProbe::ServerReceipt_Implementation(const FString& Name, bool bPassed, const FString& Detail)
{
#if !UE_BUILD_SHIPPING
	if (Driver) Driver->Receipt(Name, bPassed, Detail);
#endif
}

void StartCoopNetTests()
{
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
	Optimization = TEXT("Combined");
	FParse::Value(FCommandLine::Get(), TEXT("CoopOptimization="), Optimization);
	FParse::Value(FCommandLine::Get(), TEXT("CoopMatrix="), Matrix);
	FParse::Value(FCommandLine::Get(), TEXT("CoopCsvPath="), CsvRequestedPath);
	FParse::Value(FCommandLine::Get(), TEXT("CoopStaticCount="), StaticCount);
	FParse::Value(FCommandLine::Get(), TEXT("CoopMovingCount="), MovingCount);
	FParse::Value(FCommandLine::Get(), TEXT("CoopWarmupSeconds="), WarmupSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("CoopSampleSeconds="), SampleSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("CoopTestTimeout="), TimeoutSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("CoopOutageSeconds="), OutageSeconds);
	bHost = Role == TEXT("Host");
	StartedAt = Now();
	SetPhase(TEXT("Boot"));
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &ThisClass::Tick), .1f);
	if ((Role != TEXT("Host") && Role != TEXT("Client")) || Token.IsEmpty())
		Finish(false, TEXT("Requires CoopTestRole=Host|Client and a unique CoopTestToken"));
	if (!TArray<FString>{TEXT("Scale"), TEXT("Flow"), TEXT("LateJoin"), TEXT("Reconnect"), TEXT("SessionRetry"), TEXT("Ride"), TEXT("RideMotion")}.Contains(Mode))
		Finish(false, TEXT("Unknown test mode"));
	if (StaticCount < 0 || StaticCount > 500 || MovingCount < 0 || MovingCount > 20 || SampleSeconds <= 0 || WarmupSeconds < 0)
		Finish(false, TEXT("Invalid scale limits"));
#endif
}

void UCoopNetTestDriver::Stop()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	if (GEngine) GEngine->OnNetworkFailure().RemoveAll(this);
	if (GameInstance.IsValid())
	{
		GameInstance->OnFindComplete.RemoveDynamic(this, &ThisClass::FoundGames);
		GameInstance->OnSessionOperationChanged.RemoveDynamic(this, &ThisClass::SessionOperation);
	}
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

void UCoopNetTestDriver::Assert(const FString& Name, bool bPassed, const FString& Detail)
{
	TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetStringField(TEXT("assertion"), Name);
	Extra->SetBoolField(TEXT("passed"), bPassed);
	Emit(TEXT("ASSERT"), Detail, Extra);
	if (!bPassed) Finish(false, Name + TEXT(": ") + Detail);
}

void UCoopNetTestDriver::Finish(bool bPassed, const FString& Detail)
{
	if (bDone) return;
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
		if (Now() - PhaseAt > 1) FPlatformMisc::RequestExit(false);
		return true;
	}
	if (Now() - StartedAt > TimeoutSeconds) { Finish(false, TEXT("Test timeout")); return true; }
	UWorld* World = FindWorld();
	if (!World) return true;
	UmultiplayerGameInstance* GI = Cast<UmultiplayerGameInstance>(World->GetGameInstance());
	if (!GI) { Finish(false, TEXT("Wrong GameInstance")); return true; }
	if (GameInstance.Get() != GI)
	{
		// 模块启动时 GEngine 可能尚未构造；在游戏实例就绪后再订阅全局失败事件。
		GEngine->OnNetworkFailure().RemoveAll(this);
		GEngine->OnNetworkFailure().AddUObject(this, &ThisClass::NetworkFailure);
		GameInstance = GI;
		GI->OnFindComplete.AddUniqueDynamic(this, &ThisClass::FoundGames);
		GI->OnSessionOperationChanged.AddUniqueDynamic(this, &ThisClass::SessionOperation);
	}
	if (bHost) HostTick(World); else ClientTick(World);
#endif
	return true;
}

void UCoopNetTestDriver::SessionOperation(EMultiplayerSessionOperation Value) { Operation = Value; }

void UCoopNetTestDriver::FoundGames(bool bSuccess, const TArray<FmultiplayerSessionInfo>& Results)
{
#if !UE_BUILD_SHIPPING
	if (bDone || bHost || !GameInstance.IsValid()) return;
	if (bSuccess) for (const FmultiplayerSessionInfo& Result : Results)
	{
		if (Result.ServerName == Token)
		{
			SetPhase(TEXT("Joining"));
			GameInstance->JoinGame(Result.ResultIndex);
			return;
		}
	}
	NextAttemptAt = Now() + 2;
#endif
}

void UCoopNetTestDriver::HostTick(UWorld* World)
{
	if (Phase == TEXT("Boot"))
	{
		if (Now() - StartedAt < 1) return;
		SetPhase(TEXT("Hosting"));
		GameInstance->HostGame(Token, 2, true);
		NextAttemptAt = Now() + 3;
	}
	if (Phase == TEXT("Hosting"))
	{
		if (Mode == TEXT("SessionRetry") && !bSessionFailureChecked && Operation == EMultiplayerSessionOperation::None)
		{
			bSessionFailureChecked = true;
			Assert(TEXT("SessionFailureObserved"), World->GetNetDriver() == nullptr && World->GetNetMode() == NM_Standalone,
				TEXT("Injected CreateSession failure must roll back only the newly created listener"));
			if (bDone) return;
		}
		if (World->GetNetMode() == NM_ListenServer && Operation == EMultiplayerSessionOperation::None)
		{
			if (Mode == TEXT("LateJoin")) PrepareLateJoin(World);
			SetPhase(TEXT("ReadyForClient"));
			Emit(TEXT("READY"));
		}
		else if (Operation == EMultiplayerSessionOperation::None && Now() > NextAttemptAt)
		{
			GameInstance->HostGame(Token, 2, true);
			NextAttemptAt = Now() + 3;
		}
		return;
	}
	if (Phase == TEXT("ReadyForClient"))
	{
		APlayerController* Remote = nullptr;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			if (It->Get() && !It->Get()->IsLocalController() && It->Get()->GetCharacter()) Remote = It->Get();
		if (!Remote) return;
		FActorSpawnParameters Params;
		Params.Owner = Remote;
		Probe = Mode == TEXT("RideMotion") ? World->SpawnActor<ACoopPlatformRideProbe>(Params) : World->SpawnActor<ACoopNetTestProbe>(Params);
		Assert(TEXT("Join"), Probe.IsValid(), TEXT("Real remote PlayerController with possessed character"));
		if (!Probe.IsValid()) return;
		if (Mode == TEXT("Scale"))
		{
			SpawnScale(World);
			if (bDone) return;
			SetPhase(TEXT("Warmup"));
			Probe->Stage = TEXT("Warmup");
			Probe->ForceNetUpdate();
		}
		else SetPhase(TEXT("ScenarioStart"));
	}
	if (Mode == TEXT("Scale")) ScaleTick(World);
	else if (Mode == TEXT("RideMotion")) MotionHostTick(World);
	else ScenarioHostTick(World);
}

void UCoopNetTestDriver::ClientTick(UWorld* World)
{
	if (Mode != TEXT("Scale") && (Phase == TEXT("Leaving") || Phase == TEXT("Restarting")))
	{
		ScenarioClientTick(World);
		return;
	}
	if (World->GetNetMode() != NM_Client)
	{
		if ((Phase == TEXT("Boot") || Phase == TEXT("Finding")) && Operation == EMultiplayerSessionOperation::None && Now() > NextAttemptAt)
		{
			SetPhase(TEXT("Finding"));
			NextAttemptAt = Now() + 5;
			GameInstance->FindGames(100, true);
		}
		return;
	}
	if (!Probe.IsValid())
	{
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It) { Probe = *It; break; }
		if (!Probe.IsValid()) return;
		Assert(TEXT("Join"), true, TEXT("Remote-owned replicated test probe received after Find/Join"));
	}
	const FString Command = Probe->Stage;
	if (Mode == TEXT("RideMotion")) { MotionClientTick(World); return; }
	if (Mode != TEXT("Scale")) { ScenarioClientTick(World); return; }
	if (Command == TEXT("Warmup") && !SentReceipts.Contains(TEXT("ScaleClientLoad")))
	{
		if (Phase != TEXT("Warmup")) SetPhase(TEXT("Warmup"));
		// 跨 Actor 的属性到达顺序没有保证；等所有引用解析完，再验证实际类型和位置变化。
		if (CheckScaleLoad(World, true))
		{
			Assert(TEXT("ScaleClientLoad"), true, ScaleLoadDetail);
			SentReceipts.Add(TEXT("ScaleClientLoad"));
			Probe->ServerReceipt(TEXT("ScaleClientLoad"), true, ScaleLoadDetail);
		}
		else if (Now() - PhaseAt > 30)
		{
			Probe->ServerReceipt(TEXT("ScaleClientLoad"), false, ScaleLoadDetail);
			Assert(TEXT("ScaleClientLoad"), false, TEXT("Client load confirmation timed out: ") + ScaleLoadDetail);
		}
		return;
	}
	if (Command == TEXT("Sample") && Phase != TEXT("Sample"))
	{
		if (!SentReceipts.Contains(TEXT("ScaleClientLoad")))
		{
			Finish(false, TEXT("Server requested sampling before client load confirmation"));
			return;
		}
		SetPhase(TEXT("Sample"));
		BeginSample(World);
		if (bDone) return;
	}
	if (Command == TEXT("SampleEnd") && !SentReceipts.Contains(Command))
	{
		if (EndSample(World))
		{
			SentReceipts.Add(Command);
			Probe->ServerReceipt(Command, true, TEXT("Client counters and CSV completed"));
		}
	}
	if (Command == TEXT("Complete") && !SentReceipts.Contains(Command))
	{
		SentReceipts.Add(Command);
		Probe->ServerReceipt(Command, true, TEXT("Client complete"));
		Finish(true);
	}
}

void UCoopNetTestDriver::Receipt(const FString& Name, bool bPassed, const FString& Detail)
{
	if (!bHost || bDone || Receipts.Contains(Name)) return;
	Receipts.Add(Name);
	Emit(TEXT("RECEIPT"), Name + TEXT(": ") + Detail);
	if (!bPassed) Assert(Name, false, Detail);
}

void UCoopNetTestDriver::SpawnScale(UWorld* World)
{
	const bool bFrequency = Optimization == TEXT("Frequency") || Optimization == TEXT("Combined");
	const bool bDormancy = Optimization == TEXT("Dormancy") || Optimization == TEXT("Combined");
	if ((Matrix == TEXT("Static") && (MovingCount != 0 || (Optimization != TEXT("Baseline") && Optimization != TEXT("Dormancy"))))
		|| (Matrix == TEXT("Moving") && (StaticCount != 0 || (Optimization != TEXT("Baseline") && Optimization != TEXT("Frequency")))))
	{
		Finish(false, TEXT("Matrix must isolate Dormancy for static plates or update frequency for moving platforms"));
		return;
	}
	if (!bFrequency && !bDormancy && Optimization != TEXT("Baseline"))
	{
		Finish(false, TEXT("Unknown CoopOptimization"));
		return;
	}
	Probe->Subjects.Reset();
	for (int32 Index = 0; Index < StaticCount; ++Index)
	{
		const FVector Location(20000 + (Index % 25) * 400, (Index / 25) * 400, 2000);
		AmultiplayerPressurePlate* Plate = World->SpawnActor<AmultiplayerPressurePlate>(Location, FRotator::ZeroRotator);
		if (!Plate) continue;
		Plate->SetActorEnableCollision(false);
		Plate->bAlwaysRelevant = true;
		// 静态系列仅改变休眠，网络频率两组同为 30 Hz；不改现有关卡背景机关。
		const float PlateFrequency = Matrix == TEXT("Static") ? 30.f : (bFrequency ? 5.f : 100.f);
		Plate->SetNetUpdateFrequency(PlateFrequency);
		Plate->SetMinNetUpdateFrequency(PlateFrequency);
		Plate->SetNetDormancy(bDormancy ? DORM_DormantAll : DORM_Awake);
		Plate->ForceNetUpdate();
		Probe->Subjects.Add(Plate);
	}
	for (int32 Index = 0; Index < MovingCount; ++Index)
	{
		const FVector Location(20000, -1000 - Index * 400, 2000);
		AmultiplayerMovingPlatform* Platform = World->SpawnActor<AmultiplayerMovingPlatform>(Location, FRotator::ZeroRotator);
		if (!Platform) continue;
		Platform->bAlwaysRelevant = true;
		Platform->SetActorEnableCollision(false);
		Platform->SetNetUpdateFrequency(bFrequency ? 30 : 100);
		Platform->SetMinNetUpdateFrequency(bFrequency ? 30 : 100);
		if (UmultiplayerTransporterComponent* Transport = Platform->FindComponentByClass<UmultiplayerTransporterComponent>())
		{
			Transport->ConfigureWorldTargets(Location, Location + FVector(500, 0, 0));
			Transport->SetTransportActive(true);
		}
		MovingActors.Add(Platform);
		MovingStarts.Add(Location);
		MovingForward.Add(true);
		Probe->Subjects.Add(Platform);
	}
	Assert(TEXT("ScaleCount"), CheckScaleLoad(World, false), ScaleLoadDetail);
}

bool UCoopNetTestDriver::CheckScaleLoad(UWorld* World, bool bRequireObservedMovement)
{
	ScaleObservedStaticCount = 0;
	ScaleObservedMovingCount = 0;
	TSet<AActor*> UniqueActors;
	bool bReferencesValid = Probe.IsValid();
	if (Probe.IsValid()) for (AActor* Actor : Probe->Subjects)
	{
		if (!IsValid(Actor) || Actor->GetWorld() != World || UniqueActors.Contains(Actor))
		{
			bReferencesValid = false;
			continue;
		}
		UniqueActors.Add(Actor);
		if (Cast<AmultiplayerPressurePlate>(Actor)) ++ScaleObservedStaticCount;
		else if (AmultiplayerMovingPlatform* Platform = Cast<AmultiplayerMovingPlatform>(Actor))
		{
			++ScaleObservedMovingCount;
			if (bRequireObservedMovement)
			{
				const TWeakObjectPtr<AmultiplayerMovingPlatform> Key(Platform);
				if (const FVector* Start = ScaleObservedStarts.Find(Key))
				{
					if (FVector::DistSquared(*Start, Platform->GetActorLocation()) >= FMath::Square(5.f))
						ScaleMovedActors.Add(Key);
				}
				else ScaleObservedStarts.Add(Key, Platform->GetActorLocation());
			}
		}
		else bReferencesValid = false;
	}
	const bool bCountMatches = Probe.IsValid() && Probe->Subjects.Num() == StaticCount + MovingCount
		&& ScaleObservedStaticCount == StaticCount && ScaleObservedMovingCount == MovingCount;
	const bool bMovementConfirmed = !bRequireObservedMovement || ScaleMovedActors.Num() == MovingCount;
	ScaleLoadDetail = FString::Printf(TEXT("Observed static=%d/%d moving=%d/%d; validUniqueReferences=%s; platformsMovedAtLeast5cm=%d/%d"),
		ScaleObservedStaticCount, StaticCount, ScaleObservedMovingCount, MovingCount,
		bReferencesValid ? TEXT("true") : TEXT("false"), ScaleMovedActors.Num(), MovingCount);
	// 0 个新增对象也是有效基线：收到 Warmup 指令且空清单与期望一致即可确认。
	return bReferencesValid && bCountMatches && bMovementConfirmed;
}

void UCoopNetTestDriver::ScaleTick(UWorld* World)
{
	for (int32 Index = 0; Index < MovingActors.Num(); ++Index)
	{
		AmultiplayerMovingPlatform* Platform = MovingActors[Index].Get();
		if (!Platform) { Finish(false, TEXT("Scale platform destroyed")); return; }
		const FVector Target = MovingStarts[Index] + (MovingForward[Index] ? FVector(500, 0, 0) : FVector::ZeroVector);
		if (Platform->GetActorLocation().Equals(Target, 1))
		{
			MovingForward[Index] = !MovingForward[Index];
			Platform->FindComponentByClass<UmultiplayerTransporterComponent>()->SetTransportActive(MovingForward[Index]);
		}
	}
	if (Phase == TEXT("Warmup") && !Receipts.Contains(TEXT("ScaleClientLoad")) && Now() - PhaseAt > 45)
	{
		Finish(false, TEXT("Timed out waiting for ScaleClientLoad: client must resolve every synthetic actor and observe platform movement"));
		return;
	}
	if (Phase == TEXT("Warmup") && Receipts.Contains(TEXT("ScaleClientLoad")) && Now() - PhaseAt >= WarmupSeconds)
	{
		SetPhase(TEXT("Sample"));
		BeginSample(World);
		if (bDone) return;
		Probe->Stage = TEXT("Sample");
		Probe->ForceNetUpdate();
	}
	if (Phase == TEXT("Sample") && Now() - SampleAt >= SampleSeconds)
	{
		SetPhase(TEXT("SampleEnd"));
		Probe->Stage = TEXT("SampleEnd");
		Probe->ForceNetUpdate();
	}
	if (Phase == TEXT("SampleEnd") && EndSample(World) && Receipts.Contains(TEXT("SampleEnd")))
	{
		Assert(TEXT("SampleCompleted"), true, TEXT("Both real network drivers sampled and CSV writes finished"));
		SetPhase(TEXT("Completing"));
		Probe->Stage = TEXT("Complete");
		Probe->ForceNetUpdate();
	}
	if (Phase == TEXT("Completing") && Receipts.Contains(TEXT("Complete"))) Finish(true);
}

void UCoopNetTestDriver::BeginSample(UWorld* World)
{
	if (!CheckScaleLoad(World, !bHost)) { Finish(false, TEXT("Scale load changed before sample: ") + ScaleLoadDetail); return; }
	UNetDriver* Net = World->GetNetDriver();
	if (!Net) { Finish(false, TEXT("No NetDriver at sample start")); return; }
	SampleDriver = Net;
	StartBytes = Net->OutTotalBytes;
	StartPackets = Net->OutTotalPackets;
	SampleAt = Now();
#if CSV_PROFILER
	const FString CsvFolder = CsvRequestedPath.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NetworkValidation/Csv")) : FPaths::GetPath(CsvRequestedPath);
	const FString CsvName = CsvRequestedPath.IsEmpty() ? Token + TEXT("-") + Role + TEXT(".csv") : FPaths::GetCleanFilename(CsvRequestedPath);
	FCsvProfiler::Get()->BeginCapture(-1, CsvFolder, CsvName);
#endif
}

bool UCoopNetTestDriver::EndSample(UWorld* World)
{
	if (bMetricEmitted) return true;
	if (!CheckScaleLoad(World, !bHost)) { Finish(false, TEXT("Scale load changed during sample: ") + ScaleLoadDetail); return false; }
	UNetDriver* Net = SampleDriver.Get();
	if (!Net || World->GetNetDriver() != Net) { Finish(false, TEXT("NetDriver changed during sample")); return false; }
	if (!bSampleEnding)
	{
		SampleElapsed = Now() - SampleAt;
		SampleBytes = Net->OutTotalBytes - StartBytes;
		SamplePackets = Net->OutTotalPackets - StartPackets;
		bSampleEnding = true;
#if CSV_PROFILER
		CsvFinished = FCsvProfiler::Get()->EndCapture();
#endif
	}
	FString CsvPath;
#if CSV_PROFILER
	if (!CsvFinished.IsValid() || !CsvFinished.IsReady()) return false;
	CsvPath = FPaths::ConvertRelativePathToFull(CsvFinished.Get());
#endif
	int32 ActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It) ++ActorCount;
	TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
	Metrics->SetNumberField(TEXT("outBytes"), SampleBytes);
	Metrics->SetNumberField(TEXT("outPackets"), SamplePackets);
	Metrics->SetNumberField(TEXT("sampleSeconds"), SampleElapsed);
	Metrics->SetNumberField(TEXT("bytesPerSecond"), SampleBytes / FMath::Max(.001, SampleElapsed));
	Metrics->SetNumberField(TEXT("connections"), bHost ? Net->ClientConnections.Num() : (Net->ServerConnection ? 1 : 0));
	Metrics->SetNumberField(TEXT("actorCount"), ActorCount);
	Metrics->SetNumberField(TEXT("syntheticStaticCount"), ScaleObservedStaticCount);
	Metrics->SetNumberField(TEXT("syntheticMovingCount"), ScaleObservedMovingCount);
	Metrics->SetNumberField(TEXT("expectedStaticCount"), StaticCount);
	Metrics->SetNumberField(TEXT("expectedMovingCount"), MovingCount);
	if (!bHost) Metrics->SetNumberField(TEXT("platformsWithObservedMovement"), ScaleMovedActors.Num());
	Metrics->SetStringField(TEXT("optimization"), Optimization);
	Metrics->SetStringField(TEXT("matrix"), Matrix);
	Metrics->SetStringField(TEXT("counterSource"), TEXT("UNetDriver.OutTotalBytes/OutTotalPackets; engine send counters including configured packet overhead, not NIC capture or delivered bytes"));
	Metrics->SetStringField(TEXT("csvPath"), CsvPath);
	TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetObjectField(TEXT("metrics"), Metrics);
	Emit(TEXT("METRIC"), TEXT("Server measurement is total outbound across its remote connections; synthetic actors plus constant map background"), Extra);
	bMetricEmitted = true;
	return true;
}
