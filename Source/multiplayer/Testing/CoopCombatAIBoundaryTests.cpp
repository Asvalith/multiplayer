// Copyright Epic Games, Inc. All Rights Reserved.

// 回归感知、行为树、近战技能与控制状态之间的职责边界。
// 夹具推进真实系统并记录生成行为；无导航用例不声称已经验收寻路追击画面。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "AI/Navigation/AvoidanceManager.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Navigation/PathFollowingComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AIPerceptionSystem.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"
#include "UObject/UnrealType.h"

namespace
{
	// 只为 AI 状态边界创建真实世界；无导航，不把测试中的位置变化当成追击验收。
	struct FCombatAIBoundaryWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle SpawnedHandle;
		int32 ProjectileCount = 0;
		TArray<double> EnemySpawnTimes;

		// 创建带感知和碰撞的世界，同时记录真实弹体及敌人生成时间。
		FCombatAIBoundaryWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(true);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			SpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (Actor->IsA<AmultiplayerCombatProjectile>()) ++ProjectileCount;
				if (Actor->IsA<AmultiplayerCombatEnemy>()) EnemySpawnTimes.Add(World->GetTimeSeconds());
			}));
		}

		// 先解除生成监听，再结束世界和还原帧号，防止测试统计串到下一用例。
		~FCombatAIBoundaryWorld()
		{
			World->RemoveOnActorSpawnedHandler(SpawnedHandle);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 推进真实 BT、GAS 和投射物，不直接执行 Service 或 Task。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.0f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(0.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				// 手动推进真实感官 Tick；不调用业务回调或写入伪造的视觉刺激。
				if (UAIPerceptionSystem* Perception = UAIPerceptionSystem::GetCurrent(World)) Perception->Tick(Step);
				Elapsed += Step;
			}
		}

		// 有界推进真实 AI／GAS，等待业务可观察条件，不直接调用被测决策回调。
		bool WaitFor(TFunctionRef<bool()> Condition, float Timeout = 1.5f)
		{
			for (float Elapsed = 0; Elapsed < Timeout; Elapsed += .02f)
			{
				if (Condition()) return true;
				Advance(.02f);
			}
			return Condition();
		}

		// 创建带控制器和 PlayerState 的真实目标，固定移动以隔离感知与近战判定。
		AmultiplayerCombatCharacter* SpawnPlayer()
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AmultiplayerCombatCharacter* Player = World->SpawnActor<AmultiplayerCombatCharacter>(
				FVector(300, 0, 100), FRotator::ZeroRotator, Spawn);
			APlayerController* PC = World->SpawnActor<APlayerController>();
			PC->SetPlayerState(World->SpawnActor<APlayerState>());
			PC->Possess(Player);
			Player->GetCharacterMovement()->DisableMovement();
			return Player;
		}

		// 使用关卡中相同的保存蓝图，不临时给通用角色授予另一套技能来代替小兵配置。
		AmultiplayerCombatEnemy* SpawnMinion()
		{
			UClass* Class = LoadClass<AmultiplayerCombatEnemy>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
			if (!Class) return nullptr;
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Enemy = World->SpawnActor<AmultiplayerCombatEnemy>(Class,
				FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
			if (Enemy)
			{
				Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
				Enemy->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
				Enemy->GetMesh()->bEnableUpdateRateOptimizations = false;
			}
			return Enemy;
		}

		// 单独检查一次 GAS 动作时暂停后续树请求；目标仍写入真实黑板，由技能自己取快照。
		bool SetMeleeTarget(AmultiplayerCombatEnemy* Enemy, AmultiplayerCombatCharacter* Player)
		{
			auto* AI = Enemy ? Cast<AmultiplayerCombatAIController>(Enemy->GetController()) : nullptr;
			UBlackboardComponent* Blackboard = AI ? AI->GetBlackboardComponent() : nullptr;
			if (!Blackboard) return false;
			AI->GetBrainComponent()->StopLogic(TEXT("Inspect one minion GAS activation"));
			AI->StopMovement();
			Player->SetActorLocation(Enemy->GetActorLocation() + FVector(120, 0, 0));
			Blackboard->SetValueAsObject(CombatAIKeys::TargetActor, Player);
			return true;
		}

		// 创建真实阻挡碰撞，检查出手阶段是否重新验证视线，而非伪造判定结果。
		AActor* BlockMeleeLine(const FVector& Location)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UBoxComponent* Wall = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Wall);
			Wall->SetBoxExtent(FVector(5, 100, 200));
			Wall->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Wall->SetCollisionObjectType(ECC_WorldStatic);
			Wall->SetCollisionResponseToAllChannels(ECR_Block);
			Wall->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAIControlBoundaryTest, "Multiplayer.Combat.AIControlBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Loose tag 只驱动 AI 订阅边界；眩晕伤害、效果持续时间和网络复制由技能用例另行验证。
bool FCoopCombatAIControlBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerCombatEnemy* Enemy = Fixture.World->SpawnActor<AmultiplayerCombatEnemy>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	Enemy->GetCharacterMovement()->DisableMovement();
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer();
	AmultiplayerCombatAIController* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	UBehaviorTreeComponent* Brain = AI ? Cast<UBehaviorTreeComponent>(AI->GetBrainComponent()) : nullptr;
	if (!TestNotNull(TEXT("Enemy owns its real AI controller"), AI)
		|| !TestNotNull(TEXT("Enemy runs its saved behavior tree"), Brain)) return false;
	UBlackboardComponent* Blackboard = AI->GetBlackboardComponent();
	if (!TestNotNull(TEXT("Tree owns its blackboard"), Blackboard)) return false;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	TestTrue(TEXT("Real Sight discovers the initially visible player"), Fixture.WaitFor([&]
	{
		return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player;
	}));
	TestTrue(TEXT("Living uncontrolled enemy can decide"), Brain->IsRunning());
	TestTrue(TEXT("Normal tree selected the player"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);

	// 第一层控制立即停止旧行为；第二层控制不能被第一层的解除提前恢复。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	TestFalse(TEXT("Stun stops tree immediately without waiting for Service"), Brain->IsRunning());
	TestEqual(TEXT("Stun leaves no active path"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
	TestNull(TEXT("Stun clears stale pursuit target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("Stun clears attack eligibility"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	const int32 BeforeBlocked = Fixture.ProjectileCount;
	ASC->AddLooseGameplayTag(CombatTags::State_Casting);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	AI->StartCombat();
	Fixture.Advance(0.70f);
	TestFalse(TEXT("Casting still blocks after stun is removed"), Brain->IsRunning());
	TestEqual(TEXT("Blocked tree emits no new projectiles"), Fixture.ProjectileCount, BeforeBlocked);
	ASC->RemoveLooseGameplayTag(CombatTags::State_Casting);
	Fixture.WaitFor([&]
	{
		return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player && Fixture.ProjectileCount > BeforeBlocked;
	});
	TestTrue(TEXT("Last control removal restarts decision"), Brain->IsRunning());
	TestTrue(TEXT("Resumed tree selects current player"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	TestTrue(TEXT("Resumed tree can request GAS attack"), Fixture.ProjectileCount > BeforeBlocked);

	// 解除控制移除旧 Pawn 的订阅；旧标签变化不能让无 Pawn 的控制器重新运行。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	AI->UnPossess();
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.35f);
	TestFalse(TEXT("Old pawn control removal cannot restart an unpossessed AI"), Brain->IsRunning());
	AI->Possess(Enemy);
	Enemy->GetCharacterMovement()->DisableMovement();
	Fixture.WaitFor([&] { return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player; });
	TestTrue(TEXT("Repossess restores normal decision"), Brain->IsRunning());
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	TestFalse(TEXT("Repossessed pawn still has its control subscription"), Brain->IsRunning());

	// 暂停期间目标销毁，恢复时从根重新选目标，不复用旧 MoveTo 或攻击快照。
	Player->Destroy();
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.35f);
	TestNull(TEXT("Destroyed target is not restored with the old behavior"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	AmultiplayerCombatCharacter* Replacement = Fixture.SpawnPlayer();
	Fixture.WaitFor([&] { return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Replacement; });
	TestTrue(TEXT("Tree can acquire a new player after recovery"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Replacement);

	// 再用正式权威接口施加短眩晕，验证实际 GE 到期也走同一条恢复链路。
	Enemy->ApplyStun(0.35f);
	TestTrue(TEXT("Authoritative stun applies the real state effect"), ASC->HasMatchingGameplayTag(CombatTags::State_Stunned));
	TestFalse(TEXT("Real stun effect immediately stops the tree"), Brain->IsRunning());
	const int32 BeforeRealStun = Fixture.ProjectileCount;
	Fixture.Advance(0.15f);
	TestEqual(TEXT("Real active stun emits no new attacks"), Fixture.ProjectileCount, BeforeRealStun);
	Fixture.Advance(0.35f);
	TestFalse(TEXT("Real stun effect expires"), ASC->HasMatchingGameplayTag(CombatTags::State_Stunned));
	TestTrue(TEXT("Actual effect expiration restarts the tree"), Brain->IsRunning());

	// 死亡优先于控制到期；真实伤害走 GE，移除控制不能使尸体重新执行树。
	ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
	UAbilitySystemComponent* SourceASC = Replacement->GetAbilitySystemComponent();
	FGameplayEffectSpecHandle Damage = SourceASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f, SourceASC->MakeEffectContext());
	Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.0f);
	SourceASC->ApplyGameplayEffectSpecToTarget(*Damage.Data.Get(), ASC);
	const int32 BeforeDeathRecovery = Fixture.ProjectileCount;
	ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	Fixture.Advance(0.70f);
	TestFalse(TEXT("Dead enemy stays dead"), Enemy->IsAlive());
	TestFalse(TEXT("Control ending never restarts a dead enemy"), Brain->IsRunning());
	TestEqual(TEXT("Dead enemy cannot emit new attacks"), Fixture.ProjectileCount, BeforeDeathRecovery);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSightBoundaryTest, "Multiplayer.Combat.SightBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 只移动真实玩家和阻挡盒，再推进 Sight 与树；不向感知组件报告成功事件或手写目标黑板。
bool FCoopCombatSightBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	auto* Enemy = Fixture.World->SpawnActor<AmultiplayerCombatEnemy>(FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("Sight fixture spawns the actual native enemy"), Enemy)) return false;
	Enemy->GetCharacterMovement()->DisableMovement();
	auto* Player = Fixture.SpawnPlayer();
	Player->SetActorLocation(FVector(-600, 0, 100));
	auto* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	UAIPerceptionComponent* Perception = AI ? AI->GetPerceptionComponent() : nullptr;
	UBlackboardComponent* Blackboard = AI ? AI->GetBlackboardComponent() : nullptr;
	if (!TestNotNull(TEXT("Enemy owns a real perception component"), Perception)
		|| !TestNotNull(TEXT("Sight decisions use the running tree's blackboard"), Blackboard)) return false;
	const auto* Sight = Cast<UAISenseConfig_Sight>(Perception->GetSenseConfig(UAISense::GetSenseID<UAISense_Sight>()));
	if (!TestNotNull(TEXT("Real perception component has Sight configured"), Sight)) return false;
	TestEqual(TEXT("Sight uses the enemy's configured radius"), Sight->SightRadius, Enemy->GetDetectionRadius());
	TestEqual(TEXT("Sight uses the configured 75-degree half angle"), Sight->PeripheralVisionAngleDegrees, Enemy->GetPeripheralVisionAngle());
	Fixture.Advance(1.f);
	TestNull(TEXT("A player behind the enemy is not discovered through a world scan"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("Never-seen players do not create last-known search positions"), Blackboard->IsVectorValueSet(CombatAIKeys::LastKnownLocation));
	TArray<AActor*> Seen;
	Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Seen);
	TestFalse(TEXT("The real Sight system itself does not perceive the rear player"), Seen.Contains(Player));

	AActor* Wall = Fixture.BlockMeleeLine(FVector(300, 0, 100));
	Player->SetActorLocation(FVector(600, 0, 100));
	Fixture.Advance(1.f);
	TestNull(TEXT("A never-seen player behind a real wall remains undiscovered"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor));
	TestFalse(TEXT("A wall-hidden first encounter cannot create a fabricated search location"), Blackboard->IsVectorValueSet(CombatAIKeys::LastKnownLocation));
	Wall->Destroy();
	TestTrue(TEXT("Removing the real wall lets Sight acquire the front player"), Fixture.WaitFor([&]
	{
		return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player;
	}));
	Seen.Reset();
	Perception->GetCurrentlyPerceivedActors(UAISense_Sight::StaticClass(), Seen);
	TestTrue(TEXT("The selected target is really present in current Sight results"), Seen.Contains(Player));
	TestTrue(TEXT("Seeing a player records its actual current location"),
		Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation).Equals(Player->GetActorLocation(), .1f));
	TestFalse(TEXT("A seen target outside attack range cannot activate the attack branch"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
	TestEqual(TEXT("Perception alone does not fire an out-of-range attack"), Fixture.ProjectileCount, 0);

	Player->SetCombatSessionReady(false);
	TestTrue(TEXT("A visible but unadmitted player is discarded without corpse-style searching"), Fixture.WaitFor([&]
	{
		return !Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) && !Blackboard->IsVectorValueSet(CombatAIKeys::LastKnownLocation);
	}));
	Player->SetCombatSessionReady(true);
	TestTrue(TEXT("Readmission of the same visible player restores selection through Sight"), Fixture.WaitFor([&]
	{
		return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player;
	}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAIRootBoundaryTest, "Multiplayer.Combat.AIRootBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 禁锢只取消路径权限，不能把攻击、感知和有限搜索记忆一起停掉；使用真实 GE 而非伪造黑板条件。
bool FCoopCombatAIRootBoundaryTest::RunTest(const FString& Parameters)
{
	{
		FCombatAIBoundaryWorld Fixture;
		FActorSpawnParameters Spawn;
		Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Enemy = Fixture.World->SpawnActor<AmultiplayerCombatEnemy>(
			FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
		auto* Player = Fixture.SpawnPlayer();
		if (!TestNotNull(TEXT("Root fixture has an enemy"), Enemy) || !TestNotNull(TEXT("Root fixture has a player"), Player)) return false;
		Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		Player->SetActorLocation(FVector(600, 0, 100));
		Player->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute(), 1000.f);
		Player->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 1000.f);
		auto* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
		UBlackboardComponent* Blackboard = AI ? AI->GetBlackboardComponent() : nullptr;
		if (!TestNotNull(TEXT("Root fixture uses the real AI and blackboard"), Blackboard)) return false;
		UBrainComponent* Brain = AI->GetBrainComponent();
		if (!TestTrue(TEXT("Sight acquires the out-of-range player before root"), Fixture.WaitFor([&]
		{
			return Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player;
		}))) return false;
		TestTrue(TEXT("An admitted unrestricted enemy permits movement branches"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		const FVector SeenBeforeRoot = Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation);
		const FActiveGameplayEffectHandle FirstRoot = Enemy->ApplyRoot(4.f);
		const FActiveGameplayEffectHandle SecondRoot = Enemy->ApplyRoot(6.f);
		if (!TestTrue(TEXT("Two roots own independent real effect handles"), FirstRoot.IsValid() && SecondRoot.IsValid() && FirstRoot != SecondRoot)) return false;
		TestTrue(TEXT("Root does not stop the whole behavior tree"), Brain && Brain->IsRunning());
		TestFalse(TEXT("Root immediately closes movement branch permission"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		TestEqual(TEXT("Root immediately stops the old path"), AI->GetMoveStatus(), EPathFollowingStatus::Idle);
		TestTrue(TEXT("Root keeps the currently visible target"), Blackboard->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
		TestTrue(TEXT("Root does not erase the last visible position"), Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation).Equals(SeenBeforeRoot));
		Fixture.Advance(.4f);
		TestEqual(TEXT("An out-of-range rooted enemy does not attack"), Fixture.ProjectileCount, 0);
		TestFalse(TEXT("The service cannot reopen movement while root remains"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		Enemy->GetAbilitySystemComponent()->RemoveActiveGameplayEffect(FirstRoot);
		TestTrue(TEXT("Removing one root leaves the second restriction"), Enemy->IsRooted());
		TestFalse(TEXT("One removal cannot reopen movement branches"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		TestEqual(TEXT("A remaining root keeps CMC disabled"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);

		Player->SetActorLocation(FVector(300, 0, 100));
		TestTrue(TEXT("Root still allows the real tree to issue an in-range GAS attack"), Fixture.WaitFor([&] { return Fixture.ProjectileCount > 0; }));
		TestTrue(TEXT("Root remains active while the attack succeeds"), Enemy->IsRooted());
		TestTrue(TEXT("Root does not clear attack eligibility"), Blackboard->GetValueAsBool(CombatAIKeys::CanAttack));
		TestFalse(TEXT("Attack eligibility does not grant movement"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		Enemy->ApplyStun(.25f);
		TestFalse(TEXT("Stun still stops decision while root is also active"), Brain->IsRunning());
		Fixture.Advance(.4f);
		TestFalse(TEXT("The short independent stun expires"), Enemy->IsStunned());
		TestTrue(TEXT("Stun expiration resumes decision even though root remains"), Brain->IsRunning());
		TestFalse(TEXT("Stun expiration cannot unlock the remaining root"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));

		Fixture.BlockMeleeLine(FVector(150, 0, 100));
		if (!TestTrue(TEXT("Rooted Sight still observes loss behind a real wall"), Fixture.WaitFor([&]
		{
			return !Blackboard->GetValueAsObject(CombatAIKeys::TargetActor)
				&& Blackboard->IsVectorValueSet(CombatAIKeys::LastKnownLocation);
		}))) return false;
		const FVector LastVisible = Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation);
		Player->SetActorLocation(FVector(800, 0, 100));
		Fixture.Advance(.35f);
		TestTrue(TEXT("Root keeps bounded search memory without tracking the hidden player's new position"),
			Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation).Equals(LastVisible));
		Enemy->GetAbilitySystemComponent()->RemoveActiveGameplayEffect(SecondRoot);
		TestFalse(TEXT("The last root removal clears the root state"), Enemy->IsRooted());
		TestTrue(TEXT("The last root removal permits movement again"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		TestEqual(TEXT("The last root removal restores the captured CMC mode"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
		TestTrue(TEXT("Root ending does not discard the existing search destination"), Blackboard->GetValueAsVector(CombatAIKeys::LastKnownLocation).Equals(LastVisible));

		const FActiveGameplayEffectHandle DetachedRoot = Enemy->ApplyRoot(2.f);
		AI->UnPossess();
		Enemy->GetAbilitySystemComponent()->RemoveActiveGameplayEffect(DetachedRoot);
		Fixture.Advance(.1f);
		TestFalse(TEXT("Old pawn root changes cannot restart an unpossessed controller"), Brain->IsRunning());
		TestFalse(TEXT("Old pawn root changes cannot reopen detached movement permission"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		AI->Possess(Enemy);
		TestTrue(TEXT("Repossess restores movement permission through the running service"), Fixture.WaitFor([&] { return Blackboard->GetValueAsBool(CombatAIKeys::CanMove); }));
		Enemy->ApplyRoot(.3f);
		TestFalse(TEXT("Repossessed controller subscribes to the new root change"), Blackboard->GetValueAsBool(CombatAIKeys::CanMove));
		TestTrue(TEXT("Root after repossess still leaves the tree running"), Brain->IsRunning());
	}
	{
		// 实际保存的小兵也必须能在禁锢中完成自己的近战，而不只是原生角色立即发弹。
		FCombatAIBoundaryWorld Fixture;
		auto* Enemy = Fixture.SpawnMinion();
		auto* Player = Fixture.SpawnPlayer();
		if (!TestTrue(TEXT("Rooted minion uses its actual melee target"), Fixture.SetMeleeTarget(Enemy, Player))) return false;
		const FmultiplayerCombatSkillRule Rule = Enemy->GetCombatConfig().Attack;
		const FActiveGameplayEffectHandle Root = Enemy->ApplyRoot(Rule.Windup + Rule.Recovery + 1.f);
		const float AttackStartedAt = Fixture.World->GetTimeSeconds();
		if (!TestTrue(TEXT("Root does not reject the minion's own melee ability"),
			Enemy->GetAbilitySystemComponent()->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass()))) return false;
		TestTrue(TEXT("The rooted minion really enters the melee windup"), Enemy->IsCasting() && Enemy->IsRooted() && Root.IsValid());
		Fixture.Advance(Rule.Windup * .5f);
		TestEqual(TEXT("Rooted melee does not settle damage before its release"), Player->GetHealth(), Player->GetMaxHealth());
		// 新世界的 Timer 先进入 Pending，且引擎在严格超过到期时刻后才执行；等待真实出手而非恰好采到边界。
		TestTrue(TEXT("Rooted melee reaches its real delayed release within the windup and bounded scheduling margin"),
			Fixture.WaitFor([&] { return Player->GetHealth() < Player->GetMaxHealth(); }, Rule.Windup * .5f + .08f));
		AddInfo(FString::Printf(TEXT("Rooted minion release observed at %.6fs after activation; configured windup %.6fs."),
			Fixture.World->GetTimeSeconds() - AttackStartedAt, Rule.Windup));
		TestEqual(TEXT("Rooted melee settles the configured damage once"), Player->GetHealth(), Player->GetMaxHealth() - Rule.Amount);
		Player->SetActorLocation(FVector(5000, 0, 100));
		Fixture.Advance(Rule.Recovery + .05f);
		TestTrue(TEXT("Melee cleanup keeps the separately owned root"), Enemy->IsRooted());
		TestEqual(TEXT("Rooted melee recovery cannot settle the same hit again"), Player->GetHealth(), Player->GetMaxHealth() - Rule.Amount);
		TestFalse(TEXT("Melee can finish while rooted"), Enemy->IsCasting());
		TestEqual(TEXT("Melee finishing cannot restore movement before root ends"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
		Enemy->GetAbilitySystemComponent()->RemoveActiveGameplayEffect(Root);
		TestEqual(TEXT("Rooted melee restores its original movement mode only after the last lock ends"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSpawnStaggeringTest, "Multiplayer.Combat.SpawnStaggering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 推进出生器自己的 Timer；不直接调用分批生成函数，也不把出生时间当作战斗攻击错峰的证明。
bool FCoopCombatSpawnStaggeringTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	constexpr double TickTolerance = .02 + KINDA_SMALL_NUMBER;
	FIntProperty* CountProperty = FindFProperty<FIntProperty>(AmultiplayerEnemySpawner::StaticClass(), TEXT("EnemyCount"));
	FFloatProperty* MinInterval = FindFProperty<FFloatProperty>(AmultiplayerEnemySpawner::StaticClass(), TEXT("MinSpawnInterval"));
	FFloatProperty* MaxInterval = FindFProperty<FFloatProperty>(AmultiplayerEnemySpawner::StaticClass(), TEXT("MaxSpawnInterval"));
	if (!TestNotNull(TEXT("Spawner count remains editable configuration"), CountProperty)
		|| !TestNotNull(TEXT("Spawner minimum interval remains editable configuration"), MinInterval)
		|| !TestNotNull(TEXT("Spawner maximum interval remains editable configuration"), MaxInterval)) return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerEnemySpawner* Spawner = Fixture.World->SpawnActor<AmultiplayerEnemySpawner>(
		FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("Default spawner starts in a real world"), Spawner)) return false;
	TestEqual(TEXT("First enemy is generated immediately by BeginPlay"), Fixture.EnemySpawnTimes.Num(), 1);
	Spawner->SpawnWave();
	TestEqual(TEXT("Repeated request cannot generate a second immediate enemy"), Fixture.EnemySpawnTimes.Num(), 1);
	const float DefaultMin = MinInterval->GetPropertyValue_InContainer(Spawner);
	const float DefaultMax = MaxInterval->GetPropertyValue_InContainer(Spawner);
	TestEqual(TEXT("Default earliest reinforcement is one second later"), DefaultMin, 1.f);
	TestEqual(TEXT("Default latest reinforcement is two and a half seconds later"), DefaultMax, 2.5f);
	Fixture.Advance(DefaultMin - .02f);
	TestEqual(TEXT("Before the default spawn interval there is still only one enemy"), Fixture.EnemySpawnTimes.Num(), 1);
	Fixture.Advance(DefaultMax * 2.f + .1f);
	TestEqual(TEXT("Default wave eventually contains exactly three enemies"), Fixture.EnemySpawnTimes.Num(), 3);
	if (Fixture.EnemySpawnTimes.Num() == 3)
	{
		for (int32 Index = 1; Index < 3; ++Index)
		{
			const double Gap = Fixture.EnemySpawnTimes[Index] - Fixture.EnemySpawnTimes[Index - 1];
			TestTrue(TEXT("Default reinforcements each wait within their configured range"),
				Gap >= DefaultMin - TickTolerance && Gap <= DefaultMax + TickTolerance);
		}
		AddInfo(FString::Printf(TEXT("SpawnStaggering: enemy times %.2f, %.2f, %.2f seconds"),
			Fixture.EnemySpawnTimes[0], Fixture.EnemySpawnTimes[1], Fixture.EnemySpawnTimes[2]));
	}
	Spawner->SpawnWave();
	Fixture.Advance(DefaultMax + .1f);
	TestEqual(TEXT("Completed wave never restarts after another request"), Fixture.EnemySpawnTimes.Num(), 3);

	// 出生器提前销毁时，不允许 Timer 继续生成剩余两个敌人。
	AmultiplayerEnemySpawner* CancelledSpawner = Fixture.World->SpawnActor<AmultiplayerEnemySpawner>(
		FVector(4000, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("Cancellation fixture creates its spawner"), CancelledSpawner)) return false;
	TestEqual(TEXT("Second spawner also generates its first enemy immediately"), Fixture.EnemySpawnTimes.Num(), 4);
	CancelledSpawner->Destroy();
	Fixture.Advance(DefaultMax + .1f);
	TestEqual(TEXT("Destroying spawner cancels all remaining births"), Fixture.EnemySpawnTimes.Num(), 4);

	// 编辑器元数据不能限制运行时赋值，超过配置上限也最多只安排十次生成。
	const FTransform LimitedTransform(FRotator::ZeroRotator, FVector(8000, 0, 100));
	AmultiplayerEnemySpawner* LimitedSpawner = Fixture.World->SpawnActorDeferred<AmultiplayerEnemySpawner>(
		AmultiplayerEnemySpawner::StaticClass(), LimitedTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Count limit fixture creates its deferred spawner"), LimitedSpawner)) return false;
	CountProperty->SetPropertyValue_InContainer(LimitedSpawner, 25);
	MinInterval->SetPropertyValue_InContainer(LimitedSpawner, .1f);
	MaxInterval->SetPropertyValue_InContainer(LimitedSpawner, .1f);
	LimitedSpawner->FinishSpawning(LimitedTransform);
	Fixture.Advance(1.50f);
	TestEqual(TEXT("Runtime count above the limit generates ten rather than twenty-five enemies"), Fixture.EnemySpawnTimes.Num(), 14);
	Fixture.Advance(0.50f);
	TestEqual(TEXT("Capped wave leaves no further births scheduled"), Fixture.EnemySpawnTimes.Num(), 14);

	// 九次真实间隔验证每只重新抽取等待时间；不改全局随机种子，也不只凭两个间隔判断随机。
	const int32 RandomStart = Fixture.EnemySpawnTimes.Num();
	const FTransform RandomTransform(FRotator::ZeroRotator, FVector(12000, 0, 100));
	auto* RandomSpawner = Fixture.World->SpawnActorDeferred<AmultiplayerEnemySpawner>(
		AmultiplayerEnemySpawner::StaticClass(), RandomTransform, nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Random interval fixture creates its deferred spawner"), RandomSpawner)) return false;
	CountProperty->SetPropertyValue_InContainer(RandomSpawner, 10);
	MinInterval->SetPropertyValue_InContainer(RandomSpawner, .1f);
	MaxInterval->SetPropertyValue_InContainer(RandomSpawner, .3f);
	RandomSpawner->FinishSpawning(RandomTransform);
	TestEqual(TEXT("Random wave also generates its first enemy immediately"), Fixture.EnemySpawnTimes.Num(), RandomStart + 1);
	Fixture.Advance(3.5f);
	if (TestEqual(TEXT("Random wave completes exactly ten real births"), Fixture.EnemySpawnTimes.Num(), RandomStart + 10))
	{
		double Smallest = MAX_dbl, Largest = 0;
		for (int32 Index = RandomStart + 1; Index < Fixture.EnemySpawnTimes.Num(); ++Index)
		{
			const double Gap = Fixture.EnemySpawnTimes[Index] - Fixture.EnemySpawnTimes[Index - 1];
			TestTrue(*FString::Printf(TEXT("Random reinforcement %d stays within 0.1-0.3s plus one tick"), Index - RandomStart),
				Gap >= .1 - TickTolerance && Gap <= .3 + TickTolerance);
			Smallest = FMath::Min(Smallest, Gap);
			Largest = FMath::Max(Largest, Gap);
		}
		TestTrue(TEXT("Per-enemy waits are not one fixed interval repeated for the entire wave"), Largest - Smallest > TickTolerance);
		AddInfo(FString::Printf(TEXT("Random spawn waits: min=%.3fs max=%.3fs samples=9"), Smallest, Largest));
	}
	RandomSpawner->SpawnWave();
	Fixture.Advance(.4f);
	TestEqual(TEXT("Random one-shot chain cannot restart its completed wave"), Fixture.EnemySpawnTimes.Num(), RandomStart + 10);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatEnemyRVOLifecycleTest, "Multiplayer.Combat.EnemyRVOLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 直接生成保存的敌人蓝图，确认旧组件默认值不会盖掉服务器避让；实际绕行另由 RVOCrossing 验证。
bool FCoopCombatEnemyRVOLifecycleTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	UClass* EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	if (!TestNotNull(TEXT("Saved combat enemy blueprint loads"), EnemyClass)) return false;
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AmultiplayerCombatEnemy* Enemy = Fixture.World->SpawnActor<AmultiplayerCombatEnemy>(
		EnemyClass, FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
	if (!TestNotNull(TEXT("Saved enemy blueprint begins play"), Enemy)) return false;
	UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
	UAvoidanceManager* Avoidance = Fixture.World->GetAvoidanceManager();
	if (!TestNotNull(TEXT("Real world owns the engine RVO manager"), Avoidance)) return false;
	TestTrue(TEXT("Authority BeginPlay enables RVO even for the existing enemy blueprint"), Movement->bUseRVOAvoidance);
	TestTrue(TEXT("RVO considers neighbours several capsule widths away"), Movement->AvoidanceConsiderationRadius >= 300.0f);
	const int32 AvoidanceUID = Movement->GetRVOAvoidanceUIDFast();
	TestNotNull(TEXT("Enemy is registered with the real RVO manager, not only a boolean flag"),
		Avoidance->GetAvoidanceObjectForUID(AvoidanceUID));

	// 使用实际 GAS 致死效果，验证死亡角色停止参与避让，旧记录由引擎 TTL 回收。
	Movement->DisableMovement();
	AmultiplayerCombatCharacter* Player = Fixture.SpawnPlayer();
	UAbilitySystemComponent* SourceASC = Player->GetAbilitySystemComponent();
	FGameplayEffectSpecHandle Damage = SourceASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f, SourceASC->MakeEffectContext());
	Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.0f);
	SourceASC->ApplyGameplayEffectSpecToTarget(*Damage.Data.Get(), Enemy->GetAbilitySystemComponent());
	TestFalse(TEXT("Actual lethal effect marks enemy dead"), Enemy->IsAlive());
	TestFalse(TEXT("Death disables RVO on the movement component"), Movement->bUseRVOAvoidance);
	Fixture.Advance(Avoidance->DefaultTimeToLive * 1.6f + 0.10f);
	const FNavAvoidanceData* OldEntry = Avoidance->GetAvoidanceObjectForUID(AvoidanceUID);
	TestTrue(TEXT("Dead enemy's old avoidance record expires instead of remaining an active obstacle"),
		OldEntry == nullptr || OldEntry->ShouldBeIgnored());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMinionSkillSetupTest, "Multiplayer.Combat.MinionSkillSetup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 验证实际保存的小兵蓝图，而非原生通用角色仍为兼容用例保留的投射物攻击默认值。
bool FCoopCombatMinionSkillSetupTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	auto* Enemy = Fixture.SpawnMinion();
	if (!TestNotNull(TEXT("Saved minion blueprint begins play"), Enemy)) return false;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	TestTrue(TEXT("Saved minion chooses its own melee GAS ability"),
		Enemy->GetAttackAbilityClass() == UmultiplayerEnemyMeleeAbility::StaticClass());
	FGameplayAbilitySpec* Melee = ASC->FindAbilitySpecFromClass(UmultiplayerEnemyMeleeAbility::StaticClass());
	if (!TestNotNull(TEXT("Authority grants the configured melee ability"), Melee)) return false;
	TestEqual(TEXT("Minion melee executes only on the authority"),
		Melee->Ability->GetNetExecutionPolicy(), EGameplayAbilityNetExecutionPolicy::ServerOnly);
	TestEqual(TEXT("Minion is granted only its one attack"), ASC->GetActivatableAbilities().Num(), 1);
	for (UClass* Unwanted : { UmultiplayerAttackAbility::StaticClass(), UmultiplayerDefendAbility::StaticClass(),
		UmultiplayerHealAbility::StaticClass(), UmultiplayerAreaAbility::StaticClass() })
		TestNull(*FString::Printf(TEXT("Minion does not receive player skill %s"), *Unwanted->GetName()),
			ASC->FindAbilitySpecFromClass(Unwanted));
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Saved player defaults still load separately"), PlayerClass)) return false;
	const auto* Player = GetDefault<AmultiplayerCombatCharacter>(PlayerClass);
	TestTrue(TEXT("Enemy damage and timing use a separate config rather than the player's rules"),
		Enemy->GetCombatConfigAsset() && Enemy->GetCombatConfigAsset() != Player->GetCombatConfigAsset());
	const FmultiplayerCombatSkillRule& Rule = Enemy->GetCombatConfig().Attack;
	TestTrue(TEXT("Minion config defines a short melee range and positive damage"),
		Rule.Range > 0.f && Rule.Range <= 200.f && Rule.Amount > 0.f && Rule.Amount < 100.f);
	TestTrue(TEXT("Minion config includes visible windup and recovery"), Rule.Windup > .1f && Rule.Recovery > .1f);
	TestEqual(TEXT("BT and GAS use the same configured melee range"), Enemy->GetAttackDistance(), Rule.Range);
	auto* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	TestTrue(TEXT("Minion retains our own AI controller and saved behavior tree"),
		AI && AI->GetBrainComponent() && AI->GetBrainComponent()->IsRunning() && Enemy->GetCombatBehaviorTree());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMinionMeleeHitTest, "Multiplayer.Combat.MinionMeleeHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 出手由服务器 GAS 延时任务结算，不依赖骨骼通知；实际动作仍应由 ASC 播放。
bool FCoopCombatMinionMeleeHitTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	auto* Enemy = Fixture.SpawnMinion();
	auto* Player = Fixture.SpawnPlayer();
	if (!TestTrue(TEXT("Actual minion has a real blackboard target"), Fixture.SetMeleeTarget(Enemy, Player))) return false;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	const FmultiplayerCombatSkillRule Rule = Enemy->GetCombatConfig().Attack;
	const float InitialHealth = Player->GetHealth();
	if (!TestTrue(TEXT("Configured melee ability activates against the selected player"),
		ASC->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass()))) return false;
	TestTrue(TEXT("Windup holds a GAS casting state"), Enemy->IsCasting());
	TestEqual(TEXT("Windup blocks CMC movement"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	UAnimMontage* Montage = Enemy->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	UAnimInstance* Animation = Enemy->GetMesh()->GetAnimInstance();
	TestTrue(TEXT("Actual melee GAS activation plays the connected sword montage"),
		Animation && Montage && Animation->Montage_IsPlaying(Montage));
	TestEqual(TEXT("Starting a swing does not immediately deal damage"), Player->GetHealth(), InitialHealth);
	Fixture.Advance(Rule.Windup * .5f);
	TestEqual(TEXT("The player takes no damage before the release boundary"), Player->GetHealth(), InitialHealth);
	TestTrue(TEXT("Connected melee montage advances during windup"),
		Animation && Montage && Animation->Montage_GetPosition(Montage) > .03f);
	Fixture.Advance(Rule.Windup * .5f + .02f);
	TestEqual(TEXT("A legal melee release applies its configured damage once"), Player->GetHealth(), InitialHealth - Rule.Amount);
	TestEqual(TEXT("Melee release never spawns a fireball"), Fixture.ProjectileCount, 0);
	Player->SetActorLocation(FVector(5000, 0, 100));
	Fixture.Advance(Rule.Recovery + Rule.Cooldown + .1f);
	TestEqual(TEXT("Recovery and later world ticks cannot settle the same swing again"), Player->GetHealth(), InitialHealth - Rule.Amount);
	TestFalse(TEXT("Completed melee removes its casting lock"), Enemy->IsCasting());
	TestEqual(TEXT("Completed melee restores the prior movement mode"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	const FGameplayAbilitySpec* Melee = ASC->FindAbilitySpecFromClass(Enemy->GetAttackAbilityClass());
	TestTrue(TEXT("Completed melee leaves no active ability instance"), Melee && !Melee->IsActive());
	auto* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	TestTrue(TEXT("Completed melee resumes our behavior tree after casting ends"),
		AI && AI->GetBrainComponent() && AI->GetBrainComponent()->IsRunning());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMinionMeleeBoundaryTest, "Multiplayer.Combat.MinionMeleeBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 起手合法不代表必定命中；延时出手前再次检查目标，取消后旧任务不能延迟补伤害。
bool FCoopCombatMinionMeleeBoundaryTest::RunTest(const FString& Parameters)
{
	enum class EBoundary { Range, Wall, Behind, Stun, Death, DestroyedTarget, UnreadyTarget, FriendlyTarget };
	for (const EBoundary Boundary : { EBoundary::Range, EBoundary::Wall, EBoundary::Behind, EBoundary::Stun,
		EBoundary::Death, EBoundary::DestroyedTarget, EBoundary::UnreadyTarget, EBoundary::FriendlyTarget })
	{
		FCombatAIBoundaryWorld Fixture;
		auto* Enemy = Fixture.SpawnMinion();
		auto* Player = Fixture.SpawnPlayer();
		if (!TestTrue(TEXT("Melee boundary fixture has its actual blackboard"), Fixture.SetMeleeTarget(Enemy, Player))) return false;
		UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
		const FmultiplayerCombatSkillRule Rule = Enemy->GetCombatConfig().Attack;
		const float InitialHealth = Player->GetHealth();
		const FString Case = FString::Printf(TEXT("Melee boundary %d"), static_cast<int32>(Boundary));
		if (!TestTrue(*FString::Printf(TEXT("%s starts legally"), *Case),
			ASC->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass()))) return false;
		Fixture.Advance(Rule.Windup * .25f);
		switch (Boundary)
		{
		case EBoundary::Range: Player->SetActorLocation(FVector(5000, 0, 100)); break;
		case EBoundary::Wall: Fixture.BlockMeleeLine(FVector(60, 0, 100)); break;
		case EBoundary::Behind: Player->SetActorLocation(FVector(-120, 0, 100)); break;
		case EBoundary::Stun: Enemy->ApplyStun(Rule.Windup + Rule.Recovery + .5f); break;
		case EBoundary::Death: Player->ApplyCombatDamage(Enemy, 1000.f); break;
		case EBoundary::DestroyedTarget: Player->Destroy(); break;
		case EBoundary::UnreadyTarget: Player->SetCombatSessionReady(false); break;
		case EBoundary::FriendlyTarget:
		{
			FEnumProperty* Team = FindFProperty<FEnumProperty>(Player->GetClass(), TEXT("CombatTeam"));
			if (!TestNotNull(TEXT("Team remains authoritative configuration"), Team)) return false;
			Team->GetUnderlyingProperty()->SetIntPropertyValue(Team->ContainerPtrToValuePtr<void>(Player),
				static_cast<int64>(EMultiplayerCombatTeam::Enemies));
			break;
		}
		}
		Fixture.Advance(Rule.Windup * .75f + .02f);
		if (Boundary != EBoundary::DestroyedTarget)
		{
			TestEqual(*FString::Printf(TEXT("%s cannot damage its invalid target"), *Case), Player->GetHealth(), InitialHealth);
			Player->SetActorLocation(FVector(5000, 0, 100));
		}
		Fixture.Advance(Rule.Recovery + .1f);
		TestFalse(*FString::Printf(TEXT("%s does not leak a casting lock"), *Case),
			ASC->HasMatchingGameplayTag(CombatTags::State_Casting));
		const FGameplayAbilitySpec* Melee = ASC->FindAbilitySpecFromClass(Enemy->GetAttackAbilityClass());
		TestTrue(*FString::Printf(TEXT("%s ends its delayed ability"), *Case), Melee && !Melee->IsActive());
		TestEqual(*FString::Printf(TEXT("%s never spawns a projectile"), *Case), Fixture.ProjectileCount, 0);
		if (Boundary == EBoundary::Stun)
		{
			TestTrue(TEXT("Cancelling melee does not remove the separately owned stun"), Enemy->IsStunned());
			Fixture.Advance(Rule.Windup + .6f);
			TestFalse(TEXT("The real stun later expires normally"), Enemy->IsStunned());
			TestEqual(TEXT("Last control ending restores the original movement mode"),
				Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
		}
		else if (Boundary == EBoundary::Death)
		{
			TestFalse(TEXT("A cancelled attack cannot revive a dead minion"), Enemy->IsAlive());
			TestEqual(TEXT("Dead minion remains immobile after the old windup"),
				Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMinionCastingOwnershipTest, "Multiplayer.Combat.MinionCastingOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 清理自己的动作只能移除自己的 GE 句柄，不能顺手解开其他效果仍持有的移动限制。
bool FCoopCombatMinionCastingOwnershipTest::RunTest(const FString& Parameters)
{
	FCombatAIBoundaryWorld Fixture;
	auto* Enemy = Fixture.SpawnMinion();
	auto* Player = Fixture.SpawnPlayer();
	if (!TestTrue(TEXT("Casting ownership fixture has a blackboard target"), Fixture.SetMeleeTarget(Enemy, Player))) return false;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	if (!TestTrue(TEXT("Melee starts before the independent restriction"),
		ASC->TryActivateAbilityByClass(Enemy->GetAttackAbilityClass()))) return false;
	FGameplayEffectSpecHandle Other = ASC->MakeOutgoingSpec(UmultiplayerCastingEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	const FActiveGameplayEffectHandle OtherHandle = ASC->ApplyGameplayEffectSpecToSelf(*Other.Data.Get());
	if (!TestTrue(TEXT("Independent restriction has its own real active-effect handle"), OtherHandle.IsValid())) return false;
	const FGameplayTagContainer AttackTags(CombatTags::Ability_Attack);
	ASC->CancelAbilities(&AttackTags);
	TestTrue(TEXT("Cancelling the melee does not remove the other casting effect"), ASC->GetActiveGameplayEffect(OtherHandle) != nullptr);
	TestTrue(TEXT("Another live restriction keeps the casting state"), Enemy->IsCasting());
	TestEqual(TEXT("Another live restriction keeps movement disabled"), Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	Player->SetActorLocation(FVector(5000, 0, 100));
	ASC->RemoveActiveGameplayEffect(OtherHandle);
	Fixture.Advance(Enemy->GetCombatConfig().Attack.Windup + .1f);
	TestFalse(TEXT("Removing the last restriction clears casting"), Enemy->IsCasting());
	TestEqual(TEXT("Removing the last restriction restores the captured movement mode"),
		Enemy->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	TestEqual(TEXT("Cancelled old delay cannot damage after both restrictions are removed"), Player->GetHealth(), Player->GetMaxHealth());
	TestEqual(TEXT("Minion restriction cleanup never emits a projectile"), Fixture.ProjectileCount, 0);
	return !HasAnyErrors();
}

#endif
