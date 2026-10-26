// Copyright Epic Games, Inc. All Rights Reserved.

// 回归会话接纳后启动波次、容量预留、终局停止以及补给一次性领取。
// 使用真实 GameMode／GameState 和生成器；复制结构检查不冒充双端传输验收。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "BrainComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatEncounter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SphereComponent.h"
#include "Components/TextRenderComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Net/UnrealNetwork.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UnrealType.h"

namespace
{
	// 隔离波次调度与实际战斗，记录已出生及待生成名额，防止容量统计只看存活 Actor。
	struct FEncounterFixture
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle SpawnHandler;
		TArray<TWeakObjectPtr<AmultiplayerCombatEnemy>> Enemies;
		TArray<TWeakObjectPtr<AmultiplayerEnemySpawner>> Spawners;
		TArray<int32> WaveBirths;
		bool bSuppressEnemyAI = true;
		float SpawnDelay = 0.f;
		TSubclassOf<AmultiplayerCombatEnemy> SpawnEnemyClass = AmultiplayerCombatEnemy::StaticClass();
		int32 PeakReserved = 0;

		// 建立正式规则世界并监听真实生成器，按用例关闭敌人决策以隔离容量断言。
		FEncounterFixture(ENetMode Mode = NM_DedicatedServer)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(true);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			auto* Instance = NewObject<UGameInstance>(GEngine);
			Context.OwningGameInstance = Instance;
			World->SetGameInstance(Instance);
			World->SetPlayInEditorInitialNetMode(Mode);
			World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
			World->SetGameMode(FURL());
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			if (Mode != NM_Standalone) World->WorldType = EWorldType::PIE;
			auto* Ground = World->SpawnActor<AActor>();
			auto* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(16000.f, 16000.f, 10.f));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0.f, 0.f, -10.f));
			SpawnHandler = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (auto* Enemy = Cast<AmultiplayerCombatEnemy>(Actor))
				{
					Enemies.Add(Enemy);
					if (bSuppressEnemyAI) Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
					// 波次容量测试不让不同波复用出生点的物理阻挡造成生成失败。
					Enemy->SetActorEnableCollision(false);
					Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
				}
				if (auto* Spawner = Cast<AmultiplayerEnemySpawner>(Actor))
				{
					Spawners.Add(Spawner);
					const int32 Index = WaveBirths.Add(0);
					Spawner->OnEnemySpawned.AddLambda([this, Index](AmultiplayerCombatEnemy*) { ++WaveBirths[Index]; });
					if (SpawnDelay > 0.f)
					{
						FindFProperty<FFloatProperty>(Spawner->GetClass(), TEXT("MinSpawnInterval"))->SetPropertyValue_InContainer(Spawner, SpawnDelay);
						FindFProperty<FFloatProperty>(Spawner->GetClass(), TEXT("MaxSpawnInterval"))->SetPropertyValue_InContainer(Spawner, SpawnDelay);
					}
				}
			}));
		}
		// 移除生成监听并销毁世界，确保旧波次计时器不会影响后续用例。
		~FEncounterFixture()
		{
			World->RemoveOnActorSpawnedHandler(SpawnHandler);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		// 返回夹具的权威规则入口，不构造测试专用的接纳替代物。
		AmultiplayerGameMode* Mode() const { return World->GetAuthGameMode<AmultiplayerGameMode>(); }
		// 返回真实共享状态，检查波次和终局快照。
		AmultiplayerCoopGameState* State() const { return World->GetGameState<AmultiplayerCoopGameState>(); }
		// 计算存活敌人加待生成项的总预留，作为容量上限的独立观察值。
		int32 Reserved() const
		{
			int32 Count = 0;
			for (auto Weak : Enemies) if (auto* Enemy = Weak.Get(); Enemy && Enemy->IsAlive()) ++Count;
			for (auto Weak : Spawners) if (auto* Spawner = Weak.Get()) Count += Spawner->GetPendingSpawnCount();
			return Count;
		}
		// 小步推进正式生成计时器，同时记录全过程容量峰值，避免只检查最终数量。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				PeakReserved = FMath::Max(PeakReserved, Reserved());
				Elapsed += Step;
			}
		}
		// 建立真实控制器／角色拥有关系，但保持未接纳，等待正式会话 Claim。
		AmultiplayerCoopPlayerController* Player(int32 Index)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Pawn = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(-10000.f, Index * 300.f, 100.f), FRotator::ZeroRotator, Params);
			auto* PC = World->SpawnActor<AmultiplayerCoopPlayerController>(Params);
			if (!Pawn || !PC) return nullptr;
			if (!PC->PlayerState) PC->SetPlayerState(World->SpawnActor<APlayerState>());
			// 真实拥有关系与 ASC；由正式 Claim 接纳，不伪造本地网络握手回执。
			PC->SetPawn(Pawn);
			Pawn->PossessedBy(PC);
			Pawn->SetCombatSessionReady(false);
			Pawn->GetCharacterMovement()->SetComponentTickEnabled(false);
			return PC;
		}
		// 通过正式会话入口接纳控制器，不直接改 ready 标记触发波次。
		bool Accept(AmultiplayerCoopPlayerController* PC)
		{
			FGuid Ticket; FText Error; bool Retryable = false;
			return Mode()->ClaimCombatSession(PC, FGuid(), Ticket, Error, Retryable);
		}
		// 在正式 BeginPlay 前配置波次数量和容量，使测试走实际启动与生成链路。
		AmultiplayerCombatEncounter* Encounter(int32 Cap = 8, TArray<int32> Sizes = {3, 4, 5})
		{
			const FTransform Transform(FVector(0.f, 0.f, 100.f));
			auto* Result = World->SpawnActorDeferred<AmultiplayerCombatEncounter>(AmultiplayerCombatEncounter::StaticClass(), Transform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			FindFProperty<FClassProperty>(Result->GetClass(), TEXT("EnemyClass"))->SetObjectPropertyValue_InContainer(Result, SpawnEnemyClass.Get());
			FindFProperty<FFloatProperty>(Result->GetClass(), TEXT("WaveInterval"))->SetPropertyValue_InContainer(Result, 5.f);
			FindFProperty<FIntProperty>(Result->GetClass(), TEXT("MaxLivingEnemies"))->SetPropertyValue_InContainer(Result, Cap);
			const FArrayProperty* Waves = FindFProperty<FArrayProperty>(Result->GetClass(), TEXT("WaveSizes"));
			FScriptArrayHelper Helper(Waves, Waves->ContainerPtrToValuePtr<void>(Result));
			Helper.Resize(Sizes.Num());
			for (int32 Index = 0; Index < Sizes.Num(); ++Index) CastFieldChecked<FIntProperty>(Waves->Inner)->SetPropertyValue(Helper.GetRawPtr(Index), Sizes[Index]);
			Result->FinishSpawning(Transform);
			return Result;
		}
		// 通过权威目标快照发布胜负，观察正式终局订阅者是否停止波次和技能。
		void FinishRound(bool bWin)
		{
			auto Snapshot = State()->GetObjectiveState();
			Snapshot.RequiredKeys = 1;
			Snapshot.ActivatedKeys = bWin ? 1 : 0;
			Snapshot.bGameWon = bWin;
			Snapshot.bGameLost = !bWin;
			State()->ApplyAuthoritativeState(Snapshot);
		}
	};
	// 读取控制器实际占有的战斗角色，避免测试保留已被替换的旧 Pawn。
	AmultiplayerCombatCharacter* Pawn(AmultiplayerCoopPlayerController* Controller)
	{
		return Controller ? Cast<AmultiplayerCombatCharacter>(Controller->GetPawn()) : nullptr;
	}
	// 经 ASC 设置测试初值，仍触发正式属性约束与通知链。
	void SetResources(AmultiplayerCombatCharacter* Player, float Health, float Energy)
	{
		auto* ASC = Player->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), Health);
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), Energy);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEncounterAdmissionTest, "Multiplayer.Combat.Encounter.AdmissionAndPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 专服等两人、单机允许一人预览；重复接纳通知不得重复启动首波。
bool FCoopEncounterAdmissionTest::RunTest(const FString&)
{
	for (ENetMode NetMode : {NM_DedicatedServer, NM_Standalone})
	{
		FEncounterFixture Fixture(NetMode);
		if (!TestTrue(TEXT("Fixture uses the requested server/standalone branch"), Fixture.World->GetNetMode() == NetMode)
			|| !TestNotNull(TEXT("Production admission GameMode exists"), Fixture.Mode())) return false;
		auto* Encounter = Fixture.Encounter();
		auto* First = Fixture.Player(0);
		auto* Second = Fixture.Player(1);
		if (!TestNotNull(TEXT("Both real cooperative controllers exist"), First) || !TestNotNull(TEXT("Second controller exists"), Second)) return false;
		TestEqual(TEXT("Unaccepted owned pawns do not start a wave"), Encounter->GetCurrentWave(), 0);
		if (!TestTrue(TEXT("First player enters through genuine ClaimCombatSession"), Fixture.Accept(First))) return false;
		TestEqual(TEXT("DS waits for two; standalone allows one-player preview"), Encounter->GetCurrentWave(), NetMode == NM_Standalone ? 1 : 0);
		if (!TestTrue(TEXT("Second player is genuinely accepted"), Fixture.Accept(Second))) return false;
		TestEqual(TEXT("Second acceptance starts or preserves exactly the first wave"), Encounter->GetCurrentWave(), 1);
		TestFalse(TEXT("Repeated claim cannot create another session or wave"), Fixture.Accept(Second));
		Fixture.Mode()->OnCombatSessionAccepted.Broadcast();
		TestEqual(TEXT("Repeated real admission notification cannot restart the encounter"), Encounter->GetCurrentWave(), 1);
		TestEqual(TEXT("Only one production wave spawner was created"), Fixture.Spawners.Num(), 1);
		TestTrue(TEXT("Replicated encounter snapshot is active with three configured waves"), Fixture.State()->GetEncounterState().bActive
			&& Fixture.State()->GetEncounterState().TotalWaves == 3);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEncounterWavesTest, "Multiplayer.Combat.Encounter.ThreeWavesAndCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 三波数量按配置生成，满容量时等待死亡释放名额，结束后不补出第四波。
bool FCoopEncounterWavesTest::RunTest(const FString&)
{
	FEncounterFixture Fixture;
	auto* Encounter = Fixture.Encounter();
	if (!TestTrue(TEXT("First player accepted"), Fixture.Accept(Fixture.Player(0)))
		|| !TestTrue(TEXT("Second player accepted"), Fixture.Accept(Fixture.Player(1)))) return false;
	Fixture.Advance(15.f);
	TestEqual(TEXT("Third wave waits while seven earlier minions occupy capacity"), Encounter->GetCurrentWave(), 2);
	if (!TestEqual(TEXT("The first two wave spawners completed"), Fixture.WaveBirths.Num(), 2)) return false;
	TestEqual(TEXT("First wave actually spawned three enemies"), Fixture.WaveBirths[0], 3);
	TestEqual(TEXT("Second wave actually spawned four enemies"), Fixture.WaveBirths[1], 4);
	TestEqual(TEXT("Published living count follows the seven actual actors"), Fixture.State()->GetEncounterState().LivingEnemies, 7);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		auto* Enemy = Fixture.Enemies[Index].Get();
		if (!TestNotNull(TEXT("Enemy selected for capacity release still exists"), Enemy)) return false;
		Enemy->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	}
	Fixture.Advance(15.f);
	TestEqual(TEXT("Capacity release starts exactly the third wave"), Encounter->GetCurrentWave(), 3);
	if (!TestEqual(TEXT("There are exactly three production wave spawners"), Fixture.WaveBirths.Num(), 3)) return false;
	TestEqual(TEXT("Third wave actually spawned five enemies"), Fixture.WaveBirths[2], 5);
	TestEqual(TEXT("Three four five wave sizes produce twelve total enemies"), Fixture.Enemies.Num(), 12);
	TestEqual(TEXT("Four deaths leave eight living enemies, not twelve"), Encounter->GetLivingEnemyCount(), 8);
	TestTrue(TEXT("Living plus unspawned reservations never exceed the eight-enemy cap"), Fixture.PeakReserved <= 8);
	Fixture.Advance(10.f);
	TestEqual(TEXT("No fourth wave or catch-up burst appears after all configured waves"), Fixture.Enemies.Num(), 12);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEncounterPendingTest, "Multiplayer.Combat.Encounter.PendingSpawnReservations",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 尚未出生的旧波次请求也占容量，交错计时器不能让总预留越过上限。
bool FCoopEncounterPendingTest::RunTest(const FString&)
{
	FEncounterFixture Fixture;
	Fixture.SpawnDelay = 8.f;
	auto* Encounter = Fixture.Encounter(6, {4, 4});
	if (!TestTrue(TEXT("First player accepted"), Fixture.Accept(Fixture.Player(0)))
		|| !TestTrue(TEXT("Second player accepted"), Fixture.Accept(Fixture.Player(1)))) return false;
	Fixture.Advance(10.f);
	TestEqual(TEXT("Second wave cannot ignore two still-pending first-wave spawns"), Encounter->GetCurrentWave(), 1);
	TestEqual(TEXT("Only two enemies have actually been generated at eight-second spacing"), Fixture.Enemies.Num(), 2);
	TestEqual(TEXT("Both living actors and both pending entries reserve four slots"), Fixture.Reserved(), 4);
	for (auto Weak : Fixture.Enemies) if (auto* Enemy = Weak.Get())
		Enemy->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	Fixture.Advance(2.5f);
	TestEqual(TEXT("Two deaths release enough reserved capacity for the next complete wave"), Encounter->GetCurrentWave(), 2);
	TestEqual(TEXT("Remaining old-wave requests plus new wave reserve exactly the cap"), Fixture.Reserved(), 6);
	Fixture.Advance(35.f);
	TestTrue(TEXT("Capacity remains bounded even while two wave timers interleave"), Fixture.PeakReserved <= 6);
	TestEqual(TEXT("Both waves eventually complete their intended four attempts"), Fixture.Enemies.Num(), 8);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEncounterTerminalTest, "Multiplayer.Combat.Encounter.TerminalStopsTasksAndAI",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 胜利或失败均停止真实 AI、在途技能和后续生成，晚创建的波次对象也不得重启。
bool FCoopEncounterTerminalTest::RunTest(const FString&)
{
	for (bool bWin : {true, false})
	{
		FEncounterFixture Fixture;
		Fixture.bSuppressEnemyAI = false;
		Fixture.SpawnEnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
			TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
		if (!TestNotNull(TEXT("Terminal test uses the actual minion BP and attack montage"), Fixture.SpawnEnemyClass.Get())) return false;
		auto* Encounter = Fixture.Encounter();
		auto* First = Fixture.Player(0);
		if (!TestTrue(TEXT("First player accepted"), Fixture.Accept(First))
			|| !TestTrue(TEXT("Second player accepted"), Fixture.Accept(Fixture.Player(1)))) return false;
		if (!TestEqual(TEXT("The first wave immediately generates only its first enemy"), Fixture.Enemies.Num(), 1)) return false;
		auto* Enemy = Fixture.Enemies[0].Get();
		auto* AI = Enemy ? Cast<AAIController>(Enemy->GetController()) : nullptr;
		if (!TestNotNull(TEXT("The real enemy owns its production AI controller"), AI)
			|| !TestNotNull(TEXT("The production BT has a brain component"), AI->GetBrainComponent())) return false;
		TestTrue(TEXT("The real behavior tree is running before the result"), AI->GetBrainComponent()->IsRunning());
		if (!TestNotNull(TEXT("The production BT owns a real blackboard"), AI->GetBlackboardComponent())) return false;
		auto* Target = Pawn(First);
		Target->SetActorLocation(Enemy->GetActorLocation() + Enemy->GetActorForwardVector() * 100.f);
		AI->GetBlackboardComponent()->PauseObserverNotifications();
		AI->GetBlackboardComponent()->SetValueAsObject(CombatAIKeys::TargetActor, Target);
		AI->GetBlackboardComponent()->ResumeObserverNotifications(false);
		AI->SetControlRotation((Target->GetActorLocation() - Enemy->GetActorLocation()).Rotation());
		auto* ASC = Enemy->GetAbilitySystemComponent();
		if (!TestTrue(TEXT("The minion enters its real asynchronous melee windup before the result"),
			ASC->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass()))) return false;
		const auto* Attack = ASC->FindAbilitySpecFromClass(Enemy->GetAttackAbilityClass());
		TestTrue(TEXT("The actual granted attack is active, not an injected delay"), Attack && Attack->IsActive());
		const float TargetHealth = Target->GetHealth();
		Fixture.FinishRound(bWin);
		TestFalse(TEXT("Win and defeat both close the existing enemy's combat gate"), Enemy->IsCombatReady());
		TestFalse(TEXT("Win and defeat both stop the actual behavior tree"), AI->GetBrainComponent()->IsRunning());
		TestFalse(TEXT("Win and defeat both cancel the granted in-flight GAS attack"), Attack->IsActive());
		TestFalse(TEXT("Shared encounter state is inactive after the result"), Fixture.State()->GetEncounterState().bActive);
		const int32 TerminalWave = Fixture.State()->GetEncounterState().Wave;
		Fixture.Advance(20.f);
		TestEqual(TEXT("Neither pending births nor future-wave timers survive the result"), Fixture.Enemies.Num(), 1);
		TestEqual(TEXT("Canceled melee windup cannot deliver a delayed hit after the result"), Target->GetHealth(), TargetHealth);
		TestEqual(TEXT("Stopped wave keeps its current number rather than restarting"), Encounter->GetCurrentWave(), 1);
		auto* LateEncounter = Fixture.Encounter();
		Fixture.Mode()->OnCombatSessionAccepted.Broadcast();
		TestEqual(TEXT("An encounter loaded after an existing terminal snapshot cannot start"), LateEncounter->GetCurrentWave(), 0);
		TestEqual(TEXT("An unstarted late actor does not erase the existing terminal wave snapshot"), Fixture.State()->GetEncounterState().Wave, TerminalWave);
		Fixture.Advance(10.f);
		TestEqual(TEXT("Late loading cannot regenerate enemies after a persistent result"), Fixture.Enemies.Num(), 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSupplyTest, "Multiplayer.Combat.Encounter.SupplyAuthorityAndReentry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 真实补给先认领再应用 GE，重入触发只能给一人结算，倒地和终局不能绕过资格。
bool FCoopCombatSupplyTest::RunTest(const FString&)
{
	FEncounterFixture Fixture;
	const auto SupplyClass = LoadClass<AmultiplayerCombatSupply>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatSupply.BP_CombatSupply_C"));
	if (!TestNotNull(TEXT("Supply rules run on the formal Blueprint with authored presentation"), SupplyClass)) return false;
	auto* FirstPC = Fixture.Player(0); auto* SecondPC = Fixture.Player(1);
	if (!TestTrue(TEXT("First player accepted"), Fixture.Accept(FirstPC))
		|| !TestTrue(TEXT("Second player accepted"), Fixture.Accept(SecondPC))) return false;
	auto* First = Pawn(FirstPC); auto* Second = Pawn(SecondPC);
	const FVector Position(0.f, 0.f, 100.f);
	auto* FullSupply = Fixture.World->SpawnActor<AmultiplayerCombatSupply>(SupplyClass, Position, FRotator::ZeroRotator);
	const auto* Label = FullSupply->FindComponentByClass<UTextRenderComponent>();
	TestTrue(TEXT("Supply label remains present but hidden in game"), Label && Label->bHiddenInGame);
	First->SetActorLocation(Position);
	TestFalse(TEXT("A real full-resource overlap does not consume a supply"), FullSupply->IsConsumed());
	First->SetActorLocation(Position + FVector(-500.f, 0.f, 0.f));
	SetResources(First, First->GetMaxHealth(), 10.f);
	First->SetActorLocation(Position);
	TestTrue(TEXT("Energy alone can consume an otherwise useful supply"), FullSupply->IsConsumed());
	TestEqual(TEXT("Actual supply GE restores forty energy"), First->GetEnergy(), 50.f);
	TestEqual(TEXT("Supply does not overfill full health"), First->GetHealth(), First->GetMaxHealth());
	First->SetActorLocation(FVector(-500.f, 0.f, 100.f));
	SetResources(First, 20.f, 0.f); SetResources(Second, 20.f, 0.f);
	auto* Shared = Fixture.World->SpawnActor<AmultiplayerCombatSupply>(SupplyClass, FVector(0.f, 1000.f, 100.f), FRotator::ZeroRotator);
	auto* Trigger = Shared->FindComponentByClass<USphereComponent>();
	int32 ReentrantCallbacks = 0;
	const FDelegateHandle DamageCallback = First->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerCombatAttributes::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData&)
	{
		++ReentrantCallbacks;
		// 模拟同帧已经排队的第二个真实触发器通知；不直接调用补给内部结算函数。
		Trigger->OnComponentBeginOverlap.Broadcast(Trigger, Second, Second->GetCapsuleComponent(), 0, false, FHitResult());
	});
	First->SetActorLocation(Shared->GetActorLocation());
	First->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(DamageCallback);
	TestTrue(TEXT("The original physical overlap consumes the shared supply"), Shared->IsConsumed());
	TestTrue(TEXT("The authoritative GE really invoked the reentrant callback"), ReentrantCallbacks > 0);
	TestEqual(TEXT("Only the first player receives health"), First->GetHealth(), 55.f);
	TestEqual(TEXT("Only the first player receives energy"), First->GetEnergy(), 40.f);
	TestEqual(TEXT("Queued second overlap cannot award duplicate health"), Second->GetHealth(), 20.f);
	TestEqual(TEXT("Queued second overlap cannot award duplicate energy"), Second->GetEnergy(), 0.f);
	First->GetCoopComponent()->bEnableDowned = true;
	First->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	if (!TestTrue(TEXT("The first player genuinely enters cooperative downed state"), First->GetCoopComponent()->IsDowned())) return false;
	auto* DownedSupply = Fixture.World->SpawnActor<AmultiplayerCombatSupply>(SupplyClass, FVector(0.f, 2000.f, 100.f), FRotator::ZeroRotator);
	auto* DownedTrigger = DownedSupply->FindComponentByClass<USphereComponent>();
	DownedTrigger->OnComponentBeginOverlap.Broadcast(DownedTrigger, First, First->GetCapsuleComponent(), 0, false, FHitResult());
	TestFalse(TEXT("A queued overlap cannot let a downed player claim a supply"), DownedSupply->IsConsumed());
	TestEqual(TEXT("Supply does not bypass rescue or replenish downed health"), First->GetHealth(), 1.f);
	Fixture.FinishRound(true);
	DownedTrigger->OnComponentBeginOverlap.Broadcast(DownedTrigger, Second, Second->GetCapsuleComponent(), 0, false, FHitResult());
	TestFalse(TEXT("Persistent victory prevents a later otherwise eligible claim"), DownedSupply->IsConsumed());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopEncounterSnapshotTest, "Multiplayer.Combat.Encounter.ReplicatedSnapshotContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 检查正式复制字段和 OnRep 契约，已领取补给晚加入时隐藏而不是重放领取事件。
bool FCoopEncounterSnapshotTest::RunTest(const FString&)
{
	FEncounterFixture Fixture;
	const auto SupplyClass = LoadClass<AmultiplayerCombatSupply>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatSupply.BP_CombatSupply_C"));
	if (!TestNotNull(TEXT("Snapshot fixture uses the actual authored supply class"), SupplyClass)) return false;
	auto* Supply = Fixture.World->SpawnActor<AmultiplayerCombatSupply>(SupplyClass, FVector(0.f, 3000.f, 100.f), FRotator::ZeroRotator);
	const FBoolProperty* Consumed = FindFProperty<FBoolProperty>(Supply->GetClass(), TEXT("bConsumed"));
	if (!TestNotNull(TEXT("Supply exposes its real replicated consumed property"), Consumed)) return false;
	TestTrue(TEXT("Supply snapshot is replicated with its real RepNotify"), Consumed->HasAnyPropertyFlags(CPF_Net)
		&& Consumed->RepNotifyFunc == FName(TEXT("OnRep_Consumed")));
	// 只验晚加入复制赋值后的真实 OnRep 契约；网络传输由双客户端场景单独验收。
	Consumed->SetPropertyValue_InContainer(Supply, true);
	Supply->ProcessEvent(Supply->FindFunctionChecked(Consumed->RepNotifyFunc), nullptr);
	TestTrue(TEXT("A consumed snapshot hides the late-created supply"), Supply->IsHidden());
	TestTrue(TEXT("A consumed snapshot disables interaction instead of replaying a pickup event"),
		Supply->FindComponentByClass<USphereComponent>()->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
	const FStructProperty* Encounter = FindFProperty<FStructProperty>(Fixture.State()->GetClass(), TEXT("EncounterState"));
	TestTrue(TEXT("Shared wave status is a replicated snapshot with its own RepNotify"), Encounter && Encounter->HasAnyPropertyFlags(CPF_Net)
		&& Encounter->RepNotifyFunc == FName(TEXT("OnRep_EncounterState")));
	return !HasAnyErrors();
}

#endif
