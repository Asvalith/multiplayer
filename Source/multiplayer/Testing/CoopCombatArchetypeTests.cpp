// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Perception/AIPerceptionSystem.h"
#include "UObject/UnrealType.h"

namespace
{
	// 保存蓝图、真实感知/BT/ASC/投射物；无导航夹具只验射程内攻击，不声称导航追击已经验收。
	struct FCombatArchetypeWorld
	{
		UWorld* World;
		UClass* EnemyClass;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle Spawned;
		TArray<TWeakObjectPtr<AmultiplayerCombatEnemy>> Enemies;
		TArray<double> BirthTimes;
		TArray<TWeakObjectPtr<AmultiplayerCombatProjectile>> Projectiles;
		bool bSuppressSpawnAI = false;

		FCombatArchetypeWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(true);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr, TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
			auto* Floor = World->SpawnActor<AActor>();
			auto* Box = NewObject<UBoxComponent>(Floor);
			Floor->SetRootComponent(Box);
			Box->SetBoxExtent(FVector(16000, 16000, 10));
			Box->SetCollisionProfileName(TEXT("BlockAll"));
			Box->RegisterComponent();
			Floor->SetActorLocation(FVector(0, 0, -10));
			Spawned = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (auto* Enemy = Cast<AmultiplayerCombatEnemy>(Actor))
				{
					Enemies.Add(Enemy);
					BirthTimes.Add(World->GetTimeSeconds());
					if (bSuppressSpawnAI) Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
				}
				if (auto* Projectile = Cast<AmultiplayerCombatProjectile>(Actor)) Projectiles.Add(Projectile);
			}));
		}

		~FCombatArchetypeWorld()
		{
			World->RemoveOnActorSpawnedHandler(Spawned);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		void Advance(float Seconds)
		{
			for (float Elapsed = 0; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				if (auto* Perception = UAIPerceptionSystem::GetCurrent(World)) Perception->Tick(Step);
				Elapsed += Step;
			}
		}

		bool WaitFor(TFunctionRef<bool()> Condition, float Timeout = 1.5f)
		{
			for (float Time = 0.f; Time < Timeout && !Condition(); Time += .02f) Advance(.02f);
			return Condition();
		}

		AmultiplayerCombatEnemy* DeferredEnemy(const FVector& Location, bool bWithAI = false)
		{
			if (!EnemyClass) return nullptr;
			auto* Enemy = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(EnemyClass, FTransform(Location),
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Enemy->AutoPossessAI = bWithAI ? EAutoPossessAI::PlacedInWorldOrSpawned : EAutoPossessAI::Disabled;
			return Enemy;
		}

		AmultiplayerCombatCharacter* Player(const FVector& Location)
		{
			auto* Pawn = World->SpawnActor<AmultiplayerCombatCharacter>(Location, FRotator::ZeroRotator);
			auto* PC = World->SpawnActor<APlayerController>();
			PC->SetPlayerState(World->SpawnActor<APlayerState>());
			PC->Possess(Pawn);
			Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			return Pawn;
		}

		AmultiplayerEnemySpawner* Spawner(const FVector& Location, int32 Count = 3, bool bIncludeRanged = true)
		{
			const FTransform Transform(Location);
			auto* Result = World->SpawnActorDeferred<AmultiplayerEnemySpawner>(AmultiplayerEnemySpawner::StaticClass(), Transform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			UClass* Class = Result->GetClass();
			FindFProperty<FObjectPropertyBase>(Class, TEXT("EnemyClass"))->SetObjectPropertyValue_InContainer(Result, EnemyClass);
			FindFProperty<FIntProperty>(Class, TEXT("EnemyCount"))->SetPropertyValue_InContainer(Result, Count);
			FindFProperty<FBoolProperty>(Class, TEXT("bIncludeRangedMinion"))->SetPropertyValue_InContainer(Result, bIncludeRanged);
			FindFProperty<FFloatProperty>(Class, TEXT("MinSpawnInterval"))->SetPropertyValue_InContainer(Result, .1f);
			FindFProperty<FFloatProperty>(Class, TEXT("MaxSpawnInterval"))->SetPropertyValue_InContainer(Result, .15f);
			Result->FinishSpawning(Transform);
			return Result;
		}
	};

	UStaticMeshComponent* EnemyWeapon(AmultiplayerCombatEnemy* Enemy)
	{
		const auto* Property = FindFProperty<FObjectPropertyBase>(Enemy->GetClass(), TEXT("EnemyWeapon"));
		return Property ? Cast<UStaticMeshComponent>(Property->GetObjectPropertyValue_InContainer(Enemy)) : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRangedArchetypeTest, "Multiplayer.Combat.RangedArchetypeConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 同一个保存的小兵 BP，仅在出生前选择远程规则；共享资产和近战实例都不能被修改。
bool FCoopCombatRangedArchetypeTest::RunTest(const FString& Parameters)
{
	FCombatArchetypeWorld Fixture;
	if (!TestNotNull(TEXT("Actual saved minion blueprint exists"), Fixture.EnemyClass)) return false;
	auto* Melee = Fixture.DeferredEnemy(FVector(0, 0, 100));
	auto* Ranged = Fixture.DeferredEnemy(FVector(0, 1000, 100));
	const UmultiplayerCombatConfig* OriginalConfig = Melee->GetCombatConfigAsset();
	const FmultiplayerCombatSkillRule OriginalRule = OriginalConfig->Attack;
	if (!TestTrue(TEXT("Only an unfinished authority spawn selects its ranged rules"), Ranged->ConfigureRangedArchetype())) return false;
	Melee->FinishSpawning(Melee->GetActorTransform());
	Ranged->FinishSpawning(Ranged->GetActorTransform());
	TestFalse(TEXT("A live minion cannot change archetype underneath its existing GAS grants"), Ranged->ConfigureRangedArchetype());
	TestTrue(TEXT("First archetype remains the saved melee minion"), Melee->GetAttackAbilityClass() == UmultiplayerEnemyMeleeAbility::StaticClass());
	TestTrue(TEXT("Ranged archetype selects the existing real projectile ability"), Ranged->GetAttackAbilityClass() == UmultiplayerAttackAbility::StaticClass());
	TestTrue(TEXT("Ranged rules are an instance copy, not a modified shared data asset"), Ranged->GetCombatConfigAsset() != OriginalConfig
		&& Ranged->GetCombatConfigAsset()->GetOuter() == Ranged && Melee->GetCombatConfigAsset() == OriginalConfig);
	const auto& Rule = Ranged->GetCombatConfig().Attack;
	TestEqual(TEXT("Ranged BT distance uses the configured eleven-metre attack range"), Ranged->GetAttackDistance(), 1100.f);
	TestEqual(TEXT("Ranged GAS range matches BT"), Rule.Range, Ranged->GetAttackDistance());
	TestEqual(TEXT("Ranged minion damage is fifteen"), Rule.Amount, 15.f);
	TestEqual(TEXT("Ranged minion does not consume player resources"), Rule.Cost, 0.f);
	TestEqual(TEXT("Ranged minion cooldown retains the original readable attack cadence"), Rule.Cooldown, 1.8f);
	TestEqual(TEXT("Ranged startup uses the same source attack animation timing"), Rule.Windup, OriginalRule.Windup);
	TestEqual(TEXT("Ranged recovery uses the same source attack animation timing"), Rule.Recovery, OriginalRule.Recovery);
	TestEqual(TEXT("The shared melee asset still has its original short range"), OriginalConfig->Attack.Range, OriginalRule.Range);
	TestTrue(TEXT("Both types retain exactly the same behavior tree and controller class"), Ranged->GetCombatBehaviorTree() == Melee->GetCombatBehaviorTree()
		&& Ranged->AIControllerClass == Melee->AIControllerClass);
	TestTrue(TEXT("Both types retain the same skeletal mesh and animation class"), Ranged->GetMesh()->GetSkeletalMeshAsset() == Melee->GetMesh()->GetSkeletalMeshAsset()
		&& Ranged->GetMesh()->GetAnimClass() == Melee->GetMesh()->GetAnimClass());
	TestTrue(TEXT("Both types retain the same attack montage"), Ranged->GetActionMontage(EMultiplayerCombatAnimation::Attack)
		== Melee->GetActionMontage(EMultiplayerCombatAnimation::Attack));
	auto* MeleeWeapon = EnemyWeapon(Melee);
	auto* RangedWeapon = EnemyWeapon(Ranged);
	if (!TestNotNull(TEXT("Original minion retains its configured weapon component"), MeleeWeapon)
		|| !TestNotNull(TEXT("Ranged minion retains the same weapon component"), RangedWeapon)) return false;
	TestTrue(TEXT("Ranged does not replace the minion weapon or attachment with a mage asset"), RangedWeapon->GetStaticMesh() == MeleeWeapon->GetStaticMesh()
		&& RangedWeapon->GetAttachSocketName() == MeleeWeapon->GetAttachSocketName());
	TestTrue(TEXT("Other configured projectile and impact presentation remains unchanged"), Ranged->GetCombatConfig().ProjectileEffect == OriginalConfig->ProjectileEffect
		&& Ranged->GetCombatConfig().ProjectileCascadeEffect == OriginalConfig->ProjectileCascadeEffect
		&& Ranged->GetCombatConfig().ImpactFeedback.Effect == OriginalConfig->ImpactFeedback.Effect);
	auto* ASC = Ranged->GetAbilitySystemComponent();
	TestEqual(TEXT("Ranged minion is granted only its one attack"), ASC->GetActivatableAbilities().Num(), 1);
	TestNotNull(TEXT("Ranged has the actual existing projectile GAS spec"), ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass()));
	for (UClass* Unwanted : { UmultiplayerEnemyMeleeAbility::StaticClass(), UmultiplayerDefendAbility::StaticClass(),
		UmultiplayerHealAbility::StaticClass(), UmultiplayerAreaAbility::StaticClass() })
		TestNull(TEXT("Ranged never gains melee or player support skills"), ASC->FindAbilitySpecFromClass(Unwanted));
	TestEqual(TEXT("Each minion exposes its configured fifty-XP reward"), Ranged->GetExperienceReward(), 50);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRangedBehaviorTest, "Multiplayer.Combat.RangedArchetypeBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 不伪造感知或黑板；射程内真实 Sight→BT→GAS 起手→服务器弹体，眩晕后停止后续出手。
bool FCoopCombatRangedBehaviorTest::RunTest(const FString& Parameters)
{
	FCombatArchetypeWorld Fixture;
	if (!TestNotNull(TEXT("Actual saved minion blueprint exists"), Fixture.EnemyClass)) return false;
	auto* Enemy = Fixture.DeferredEnemy(FVector(0, 0, 100), true);
	Enemy->ConfigureRangedArchetype();
	Enemy->FinishSpawning(Enemy->GetActorTransform());
	Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	auto* Player = Fixture.Player(FVector(800, 0, 100));
	auto* PlayerASC = Player->GetAbilitySystemComponent();
	PlayerASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute(), 1000.f);
	PlayerASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 1000.f);
	auto* AI = Cast<AmultiplayerCombatAIController>(Enemy->GetController());
	if (!TestNotNull(TEXT("Ranged uses the same real combat AI controller"), AI)) return false;
	if (!TestTrue(TEXT("Real Sight and BT activate ranged GAS at eight metres without approaching melee range"), Fixture.WaitFor([&]
		{ return Enemy->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Attacking); }))) return false;
	TestTrue(TEXT("Actual tree selected this player in its blackboard"), AI->GetBlackboardComponent()->GetValueAsObject(CombatAIKeys::TargetActor) == Player);
	Fixture.Advance(Enemy->GetCombatConfig().Attack.Windup * .5f);
	TestEqual(TEXT("Visible startup does not emit a projectile early"), Fixture.Projectiles.Num(), 0);
	if (!TestTrue(TEXT("Real ranged startup eventually emits a server projectile"), Fixture.WaitFor([&] { return Fixture.Projectiles.Num() > 0; }, 1.f))) return false;
	auto* Projectile = Fixture.Projectiles[0].Get();
	TestTrue(TEXT("The projectile uses the existing initialized reflectable projectile class"), Projectile && Projectile->GetOwner() == Enemy
		&& Projectile->GetLaunchState().bInitialized && Projectile->GetLaunchState().Velocity.Size() > 100.f);
	Enemy->ApplyStun(2.f);
	TestFalse(TEXT("Stun cancels the currently executing ranged ability"), Enemy->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Attacking));
	TestFalse(TEXT("Stun stops the same behavior tree"), AI->GetBrainComponent()->IsRunning());
	const int32 ShotsBeforeStun = Fixture.Projectiles.Num();
	Fixture.Advance(.8f);
	TestEqual(TEXT("Stun prevents new server projectiles"), Fixture.Projectiles.Num(), ShotsBeforeStun);
	TestEqual(TEXT("Already released projectile still settles its configured damage once"), Player->GetHealth(), 985.f);
	TestTrue(TEXT("Stun does not silently kill or replace the ranged enemy"), Enemy->IsAlive() && Enemy->IsStunned());
	Fixture.Advance(1.4f);
	TestTrue(TEXT("Control expiry resumes the same decision tree"), AI->GetBrainComponent()->IsRunning());
	TestTrue(TEXT("The resumed tree can issue another actual ranged attack"), Fixture.WaitFor([&] { return Fixture.Projectiles.Num() > ShotsBeforeStun; }, 2.f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMixedArchetypeWaveTest, "Multiplayer.Combat.MixedArchetypeWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 出生器仍一只一只错峰；第三次出生才配置为远程，重复调用、可选开关与数量上限继续生效。
bool FCoopCombatMixedArchetypeWaveTest::RunTest(const FString& Parameters)
{
	FCombatArchetypeWorld Fixture;
	if (!TestNotNull(TEXT("Mixed wave uses the actual saved minion blueprint"), Fixture.EnemyClass)) return false;
	const auto* IncludeRanged = FindFProperty<FBoolProperty>(AmultiplayerEnemySpawner::StaticClass(), TEXT("bIncludeRangedMinion"));
	TestFalse(TEXT("Normal gameplay defaults to all melee minions; ranged is explicit opt-in"),
		IncludeRanged->GetPropertyValue_InContainer(GetDefault<AmultiplayerEnemySpawner>()));
	Fixture.bSuppressSpawnAI = true;
	// 首帧之前创建的 Timer 会先 Pending、首帧末才激活；先推进真实一帧，
	// 让此处测量已运行世界中的随机间隔，而非把首次 Timer 激活延迟误算成错峰退化。
	Fixture.Advance(.02f);
	auto* Spawner = Fixture.Spawner(FVector(0, 0, 100));
	TestEqual(TEXT("First minion still appears immediately"), Fixture.Enemies.Num(), 1);
	Spawner->SpawnWave();
	Fixture.Advance(.8f);
	if (!TestEqual(TEXT("Normal mixed wave spawns exactly three minions"), Fixture.Enemies.Num(), 3)) return false;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		auto* Enemy = Fixture.Enemies[Index].Get();
		TestTrue(TEXT("Every wave member retains the exact same saved BP class"), Enemy && Enemy->GetClass() == Fixture.EnemyClass);
		TestTrue(*FString::Printf(TEXT("Wave member %d has its intended attack archetype"), Index + 1), Enemy && Enemy->GetAttackAbilityClass()
			== (Index == 2 ? UmultiplayerAttackAbility::StaticClass() : UmultiplayerEnemyMeleeAbility::StaticClass()));
		if (Index > 0)
		{
			const double Gap = Fixture.BirthTimes[Index] - Fixture.BirthTimes[Index - 1];
			AddInfo(FString::Printf(TEXT("Mixed wave member %d actual stagger=%.6fs (birth=%.6f previous=%.6f)"), Index+1, Gap, Fixture.BirthTimes[Index], Fixture.BirthTimes[Index-1]));
			TestTrue(TEXT("Changing archetype preserves the configured per-minion stagger"), Gap >= .08 - KINDA_SMALL_NUMBER && Gap <= .17 + KINDA_SMALL_NUMBER);
		}
	}
	Spawner->SpawnWave();
	Fixture.Advance(.3f);
	TestEqual(TEXT("Completed mixed wave cannot restart from another request"), Fixture.Enemies.Num(), 3);
	Fixture.Spawner(FVector(4000, 0, 100), 25);
	Fixture.Advance(2.f);
	TestEqual(TEXT("Archetype selection preserves the ten-enemy runtime cap"), Fixture.Enemies.Num(), 13);
	Fixture.Spawner(FVector(8000, 0, 100), 3, false);
	Fixture.Advance(.8f);
	if (TestEqual(TEXT("A disabled mixed option still creates the normal three-member wave"), Fixture.Enemies.Num(), 16))
		for (int32 Index = 13; Index < 16; ++Index)
			TestTrue(TEXT("Disabling ranged inclusion preserves all original melee types"), Fixture.Enemies[Index]->GetAttackAbilityClass() == UmultiplayerEnemyMeleeAbility::StaticClass());
	auto* Cancelled = Fixture.Spawner(FVector(12000, 0, 100));
	const int32 BeforeDestroy = Fixture.Enemies.Num();
	Cancelled->Destroy();
	Fixture.Advance(.6f);
	TestEqual(TEXT("Destroying the mixed spawner cancels all later births"), Fixture.Enemies.Num(), BeforeDestroy);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatArchetypeRewardTest, "Multiplayer.Combat.ArchetypeKillRewards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实受控制玩家通过统一伤害入口击杀两种小兵；每个出生实体只能发一次经验奖励。
bool FCoopCombatArchetypeRewardTest::RunTest(const FString& Parameters)
{
	FCombatArchetypeWorld Fixture;
	if (!TestNotNull(TEXT("Reward fixture uses the saved minion blueprint"), Fixture.EnemyClass)) return false;
	auto* Player = Fixture.Player(FVector(-1000, 0, 100));
	auto* Progression = Player->GetProgressionComponent();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		auto* Enemy = Fixture.DeferredEnemy(FVector(500, Index * 1000, 100));
		if (Index == 1) Enemy->ConfigureRangedArchetype();
		Enemy->FinishSpawning(Enemy->GetActorTransform());
		TestEqual(TEXT("Both real archetypes expose the same fifty-XP reward"), Enemy->GetExperienceReward(), 50);
		TestTrue(TEXT("Player's actual damage commits the hostile kill"), Player->ApplyCombatDamage(Enemy, 1000.f));
		TestFalse(TEXT("Damage really kills the enemy"), Enemy->IsAlive());
		const auto AfterKill = Progression->CaptureState();
		TestFalse(TEXT("A duplicate hit cannot settle the already dead target"), Player->ApplyCombatDamage(Enemy, 1000.f));
		Enemy->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
		TestTrue(TEXT("Repeated dead-state notifications never pay the reward twice"),
			FmultiplayerCombatProgressionState::StaticStruct()->CompareScriptStruct(&AfterKill, &Progression->GetState(), 0));
		if (Index == 0)
		{
			TestEqual(TEXT("One minion pays fifty XP without prematurely leveling"), Progression->GetState().Experience, 50);
			TestEqual(TEXT("One minion keeps character level one"), Progression->GetState().Level, 1);
		}
	}
	TestEqual(TEXT("Two actual minion kills reach the first level threshold"), Progression->GetState().Level, 2);
	TestEqual(TEXT("The hundred XP is consumed at level two"), Progression->GetState().Experience, 0);
	TestEqual(TEXT("Each actual spawn owns a distinct, singly claimed death reward"), Progression->GetState().ClaimedRewards.Num(), 2);
	return !HasAnyErrors();
}

#endif
