#include "Testing/CoopNetTestDriver.h"

#include "AbilitySystemComponent.h"
#include "AIController.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

// 流量对照夹具：在固定墙钟采样窗执行相同负载，服务器统计发送字节/包数，两端验证实际到达。
// 这里的准备、自动发射和采样是开发实验，不属于正常玩家技能循环或整局性能结论。

// 前后对照使用固定负载，不逐次发送测试 RPC；发射和属性修改仍走真实服务器路径。
void UCoopNetTestDriver::TrafficServerTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	UNetDriver* Net = World->GetNetDriver();
	const double Now = FPlatformTime::Seconds();
	if (Phase == TEXT("ScenarioStart"))
	{
		// 仅本测试服务器限为30帧；使用同等控制台优先级覆盖启动ExecCmds，预热后再计量实际帧率。
		IConsoleVariable* MaxFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
		if (!MaxFps) { Finish(false, TEXT("CombatTraffic requires t.MaxFPS")); return; }
		MaxFps->Set(30.f, ECVF_SetByConsole);
		if (MaxFps->GetFloat() != 30.f) { Finish(false, TEXT("CombatTraffic could not fix server t.MaxFPS to 30")); return; }
		CombatPrimary = Cast<AmultiplayerCombatCharacter>(PrimaryPlayer(World)->GetCharacter());
		CombatPartner = Cast<AmultiplayerCombatCharacter>(PartnerPlayer(World)->GetCharacter());
		Assert(TEXT("ServerTrafficFixture"), CombatPrimary.IsValid() && CombatPartner.IsValid(), TEXT("Two real combat player pawns"));
		if (bDone) return;
		for (TActorIterator<AmultiplayerEnemySpawner> It(World); It; ++It) It->Destroy();
		for (TActorIterator<AmultiplayerCombatEnemy> It(World); It; ++It)
		{
			if (AAIController* AI = Cast<AAIController>(It->GetController())) { AI->UnPossess(); AI->Destroy(); }
			It->Destroy();
		}
		for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It) It->Destroy();
		for (AmultiplayerCombatCharacter* Character : {CombatPrimary.Get(), CombatPartner.Get()})
		{
			Character->GetCharacterMovement()->DisableMovement();
			Character->SetActorLocation(FVector(0, Character == CombatPrimary.Get() ? 0 : 300, 4000));
			Character->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 100);
			Character->ForceNetUpdate();
		}
		FCollisionQueryParams Query(SCENE_QUERY_STAT(CoopTrafficClearPath), false, CombatPrimary.Get());
		Query.AddIgnoredActor(CombatPartner.Get());
		FCollisionObjectQueryParams Objects; Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
		FHitResult Hit;
		Assert(TEXT("ServerTrafficClearPath"), !World->SweepSingleByObjectType(Hit, FVector(0, 0, 4040), FVector(0, 0, 8927),
			FQuat::Identity, Objects, FCollisionShape::MakeSphere(12), Query), TEXT("Upward projectile corridor is unobstructed"));
		if (bDone) return;
		Probe->Subject = CombatPrimary.Get(); PartnerProbe->Subject = CombatPrimary.Get();
		SetCommand(TEXT("TrafficReady"));
	}
	if (Phase == TEXT("TrafficReady") && Receipts.Contains(TEXT("PrimaryTrafficReady")) && Receipts.Contains(TEXT("PartnerTrafficReady")))
		SetCommand(TEXT("TrafficWarmup"));
	else if (Phase == TEXT("TrafficWarmup") && Now - PhaseAt >= 3)
	{
		TrafficWarmupSeconds = Now - PhaseAt;
		Assert(TEXT("ServerTrafficConnections"), Net && Net->ClientConnections.Num() == 2, TEXT("Exactly two connections before sampling"));
		if (bDone) return;
		SetCommand(TEXT("TrafficSampling"));
		TrafficDriver = Net; TrafficStartedAt = FPlatformTime::Seconds(); TrafficStartFrame = GFrameCounter;
		TrafficStartOutBytes = Net->OutTotalBytes; TrafficStartOutPackets = Net->OutTotalPackets;
	}
	if (Phase == TEXT("TrafficSampling"))
	{
		if (!Net || Net != TrafficDriver.Get() || Net->ClientConnections.Num() != 2 || !CombatPrimary.IsValid())
		{ Finish(false, TEXT("Traffic sample lost its original driver, two connections or source pawn")); return; }
		const double Elapsed = FPlatformTime::Seconds() - TrafficStartedAt;
		if (Elapsed < 15)
		{
			while (TrafficProjectileSpawns < 30 && Elapsed >= TrafficProjectileSpawns * .5)
			{
				TrafficMaxScheduleDelay = FMath::Max(TrafficMaxScheduleDelay, Elapsed - TrafficProjectileSpawns * .5);
				PrimaryPlayer(World)->SetControlRotation(FRotator(90, 0, 0));
				if (!AmultiplayerCombatProjectile::FireFromSkill(*CombatPrimary, 25, 1600)) { Finish(false, TEXT("Real projectile spawn failed")); return; }
				++TrafficProjectileSpawns; // FireProjectile 仅在真实弹体完成生成后返回成功。
			}
			while (TrafficEnergyUpdates < 60 && Elapsed >= TrafficEnergyUpdates * .25)
			{
				TrafficMaxScheduleDelay = FMath::Max(TrafficMaxScheduleDelay, Elapsed - TrafficEnergyUpdates * .25);
				CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), TrafficEnergyUpdates % 2 ? 70 : 30);
				++TrafficEnergyUpdates;
			}
			return;
		}
		const uint32 EndBytes = Net->OutTotalBytes, EndPackets = Net->OutTotalPackets;
		const uint64 Frames = GFrameCounter - TrafficStartFrame;
		const double ActualFps = Frames / Elapsed;
		Assert(TEXT("ServerTrafficSchedule"), Elapsed <= 15.25 && TrafficWarmupSeconds <= 3.25 && TrafficMaxScheduleDelay < .25
			&& TrafficProjectileSpawns == 30 && TrafficEnergyUpdates == 60 && EndBytes > TrafficStartOutBytes && EndPackets > TrafficStartOutPackets && ActualFps >= 28 && ActualFps <= 34,
			FString::Printf(TEXT("Fixed workload: warmup=%.3fs sample=%.3fs FPS=%.2f (required28..34) spawns=%d energyUpdates=%d maxDelay=%.3fs; bytes=%u->%u packets=%u->%u"),
				TrafficWarmupSeconds, Elapsed, Frames / Elapsed, TrafficProjectileSpawns, TrafficEnergyUpdates, TrafficMaxScheduleDelay,
				TrafficStartOutBytes, EndBytes, TrafficStartOutPackets, EndPackets));
		if (bDone) return;
		TSharedRef<FJsonObject> Metrics = MakeShared<FJsonObject>();
		Metrics->SetStringField(TEXT("category"), TEXT("combatTraffic"));
		Metrics->SetStringField(TEXT("counterSource"), TEXT("UNetDriver.OutTotalBytes/OutTotalPackets"));
		Metrics->SetStringField(TEXT("scope"), TEXT("server-all-client-connections"));
		Metrics->SetStringField(TEXT("scheduleVersion"), TEXT("projectile-energy-v2"));
		Metrics->SetStringField(TEXT("trajectoryMode"), TEXT("initialLaunchState"));
		Metrics->SetBoolField(TEXT("replicateMovement"), GetDefault<AmultiplayerCombatProjectile>()->IsReplicatingMovement());
		Metrics->SetStringField(TEXT("projectileNetHzMeaning"), TEXT("actor-relevancy-check-limit-not-position-stream"));
		const auto Number = [&Metrics](const TCHAR* Name, double Value) { Metrics->SetNumberField(Name, Value); };
		Number(TEXT("warmupSeconds"), TrafficWarmupSeconds); Number(TEXT("targetSampleSeconds"), 15); Number(TEXT("elapsedSeconds"), Elapsed);
		Number(TEXT("beginOutTotalBytes"), TrafficStartOutBytes); Number(TEXT("endOutTotalBytes"), EndBytes); Number(TEXT("outBytes"), EndBytes - TrafficStartOutBytes);
		Number(TEXT("beginOutTotalPackets"), TrafficStartOutPackets); Number(TEXT("endOutTotalPackets"), EndPackets); Number(TEXT("outPackets"), EndPackets - TrafficStartOutPackets);
		Number(TEXT("bytesPerSecond"), (EndBytes - TrafficStartOutBytes) / Elapsed); Number(TEXT("packetsPerSecond"), (EndPackets - TrafficStartOutPackets) / Elapsed);
		Number(TEXT("projectileSpawns"), TrafficProjectileSpawns); Number(TEXT("energyUpdates"), TrafficEnergyUpdates); Number(TEXT("connectionCount"), 2);
		Number(TEXT("projectileIntervalSeconds"), .5); Number(TEXT("energyIntervalSeconds"), .25); Number(TEXT("maxScheduleDelaySeconds"), TrafficMaxScheduleDelay);
		Number(TEXT("projectileNetHz"), GetDefault<AmultiplayerCombatProjectile>()->GetNetUpdateFrequency());
		Number(TEXT("frameSamples"), Frames); Number(TEXT("framesPerSecond"), ActualFps); Number(TEXT("serverNetTickRate"), Net->GetNetServerMaxTickRate());
		Number(TEXT("maxFps"), IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"))->GetFloat());
		TSharedRef<FJsonObject> Extra = MakeShared<FJsonObject>(); Extra->SetObjectField(TEXT("metrics"), Metrics);
		Emit(TEXT("METRIC"), TEXT("NetDriver-accounted outbound traffic includes packet overhead, ACKs, retransmissions and all game/test traffic; not a NIC packet trace"), Extra);
		CombatPrimary->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 73);
		CombatPrimary->ForceNetUpdate(); SetCommand(TEXT("TrafficVerify"));
	}
	if (Phase == TEXT("TrafficVerify") && Receipts.Contains(TEXT("PrimaryTrafficObserved")) && Receipts.Contains(TEXT("PartnerTrafficObserved"))) SetCommand(TEXT("Complete"));
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete")) && Receipts.Contains(TEXT("PartnerComplete"))) Finish(true, TEXT("Fixed traffic load and both replicas verified"));
#endif
}

// 双端观察真实初始弹道状态与最终公开属性，采样结束才回执，避免测试 RPC 污染窗口。
void UCoopNetTestDriver::TrafficClientTick(UWorld* World)
{
#if !UE_BUILD_SHIPPING
	if (!Probe.IsValid()) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	auto* Subject = Cast<AmultiplayerCombatCharacter>(Probe->Subject.Get());
	auto* PC = World->GetFirstPlayerController();
	auto* Local = PC ? Cast<AmultiplayerCombatCharacter>(PC->GetCharacter()) : nullptr;
	if (!Subject || !Local) return;
	Local->GetCharacterMovement()->DisableMovement();
	const FString Prefix = bPartner ? TEXT("Partner") : TEXT("Primary");
	if (Phase == TEXT("TrafficReady") && Local->IsCombatReady() && GameInstance->GetConnectionState() == EMultiplayerConnectionState::Connected
		&& Subject->GetHealth() == 100 && FMath::IsNearlyEqual(Local->GetActorLocation().Z, 4000.f, 1.f))
		SendReceipt(Prefix + TEXT("TrafficReady"), true, TEXT("Real pawn, subject and public initial health are ready"));
	if (Phase == TEXT("TrafficSampling") || Phase == TEXT("TrafficVerify"))
		for (TActorIterator<AmultiplayerCombatProjectile> It(World); It; ++It)
		{
			if (It->GetOwner() != Subject || It->HasAuthority()) continue;
			const FMultiplayerCombatProjectileLaunchState& Launch = It->GetLaunchState();
			if (Launch.bInitialized && !It->IsReplicatingMovement() && FMath::IsNearlyEqual(Launch.Velocity.Size(), 1600.0, 1.0))
			{
				bTrafficProjectileObserved = true;
			}
		}
	if (Phase == TEXT("TrafficVerify") && bTrafficProjectileObserved && FMath::IsNearlyEqual(Subject->GetHealth(), 73.f)
		&& (bPartner || FMath::IsNearlyEqual(Subject->GetEnergy(), 70.f)))
	{
		// 额外契约回执只在采样结束后发送，采样期与旧版本保持相同的测试 RPC 负载。
		SendReceipt(Prefix + TEXT("TrafficLaunchState"), true,
			TEXT("Received initialized origin/velocity/server-time launch state without a continuous FRepMovement stream"));
		SendReceipt(Prefix + TEXT("TrafficObserved"), true, TEXT("Observed a real replicated projectile and post-sample public health73; owner also verified final energy70; observations are not packet counts"));
	}
	if (!bPartner && Phase == TEXT("Complete")) { SendReceipt(TEXT("Complete"), true, TEXT("Traffic fixture complete")); Finish(true); }
#endif
}
