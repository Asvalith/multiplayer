// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"

namespace
{
	// 边界用例使用真实世界、碰撞和 GAS；靶敌不启动 AI，避免额外攻击污染断言。
	struct FCombatRuleBoundaryWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;

		FCombatRuleBoundaryWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			Box(FVector(0, 0, -10), FVector(5000, 5000, 10), ECC_WorldStatic);
		}

		~FCombatRuleBoundaryWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		void Advance(float Seconds)
		{
			for (float Elapsed = 0.0f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(0.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Elapsed += Step;
			}
		}

		// 延迟生成时设置现有配置属性，不为测试给核心类增加公开修改接口。
		AmultiplayerCombatCharacter* Player(const FVector& Location = FVector(0, 0, 100), UmultiplayerCombatConfig* Config = nullptr)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Character = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(AmultiplayerCombatCharacter::StaticClass(),
				Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (Config)
			{
				const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(AmultiplayerCombatCharacter::StaticClass(), TEXT("CombatConfig"));
				if (!Property) { Character->Destroy(); return nullptr; }
				Property->SetObjectPropertyValue_InContainer(Character, Config);
			}
			Character->FinishSpawning(Transform);
			Character->GetCharacterMovement()->DisableMovement();
			return Character;
		}

		AmultiplayerCombatEnemy* Enemy(const FVector& Location)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Character = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(AmultiplayerCombatEnemy::StaticClass(),
				Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Character->AutoPossessAI = EAutoPossessAI::Disabled;
			Character->FinishSpawning(Transform);
			Character->GetCharacterMovement()->DisableMovement();
			return Character;
		}

		AActor* Box(const FVector& Location, const FVector& Extent, ECollisionChannel ObjectType)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UBoxComponent* Shape = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Shape);
			Shape->SetBoxExtent(Extent);
			Shape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Shape->SetCollisionObjectType(ObjectType);
			Shape->SetCollisionResponseToAllChannels(ECR_Block);
			Shape->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatConfiguredAttackCostTest, "Multiplayer.Combat.ConfiguredAttackCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 配置非零普通攻击消耗，必须同时参与资格检查与实际扣费，而不是只拦截低余额。
bool FCoopCombatConfiguredAttackCostTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Config = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), Fixture.World);
	Config->Attack.Cost = 13.0f;
	Config->Attack.Cooldown = 1.25f;
	auto* Caster = Fixture.Player(FVector(0, 0, 100), Config);
	if (!TestNotNull(TEXT("Deferred character received its own config"), Caster)) return false;
	Caster->Attack();
	TestEqual(TEXT("Configured attack cost is applied"), Caster->GetEnergy(), 87.0f);
	TestTrue(TEXT("Configured attack cooldown is applied"), Caster->GetAttackCooldownRemaining() > 1.20f);
	int32 ProjectileCount = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) ++ProjectileCount;
	TestEqual(TEXT("Successful request emits the real authoritative projectile"), ProjectileCount, 1);
	Caster->Attack();
	TestEqual(TEXT("Cooldown rejection does not charge again"), Caster->GetEnergy(), 87.0f);
	Fixture.Advance(1.35f);
	Caster->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 12.0f);
	Caster->Attack();
	TestEqual(TEXT("Cost check uses the same configured amount as payment"), Caster->GetEnergy(), 12.0f);
	TestEqual(TEXT("Insufficient energy cannot start cooldown"), Caster->GetAttackCooldownRemaining(), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCommitReentrancyBoundaryTest, "Multiplayer.Combat.CommitReentrancyBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 冷却通知中的同步施法只能看到扣费后的余额，不允许两次 Commit 消费同一份旧余额。
bool FCoopCombatCommitReentrancyBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Caster = Fixture.Player();
	auto* Friend = Fixture.Player(FVector(300, 0, 100));
	auto* Source = Fixture.Enemy(FVector(-1500, 0, 100));
	Source->ApplyCombatDamage(Friend, 25.0f);
	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 25.0f);
	int32 NestedHealRequests = 0;
	FDelegateHandle Listener = ASC->RegisterGameplayTagEvent(CombatTags::Cooldown_Defend, EGameplayTagEventType::NewOrRemoved)
		.AddLambda([Caster, &NestedHealRequests](FGameplayTag Tag, int32 NewCount)
		{
			if (NewCount > 0) { ++NestedHealRequests; Caster->Heal(); }
		});
	Caster->Defend();
	ASC->RegisterGameplayTagEvent(CombatTags::Cooldown_Defend, EGameplayTagEventType::NewOrRemoved).Remove(Listener);
	TestEqual(TEXT("The real cooldown callback attempted a nested heal"), NestedHealRequests, 1);
	TestTrue(TEXT("Original defense still resolves"), Caster->IsDefending());
	TestEqual(TEXT("Defense pays twenty before the callback observes energy"), Caster->GetEnergy(), 5.0f);
	TestEqual(TEXT("Nested heal cannot spend the already consumed balance"), Friend->GetHealth(), 75.0f);
	TestFalse(TEXT("Rejected nested heal does not acquire a cooldown"), ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Heal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatActivationABABoundaryTest, "Multiplayer.Combat.ActivationABABoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 同一个 GAS 实例在扣费通知中取消并立即重用，旧调用栈不能覆盖新句柄或结束新技能。
bool FCoopCombatActivationABABoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Caster = Fixture.Player();
	auto* Target = Fixture.Enemy(FVector(Caster->GetCombatConfig().Area.Range, 0, 100));
	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	int32 NestedActivations = 0;
	FDelegateHandle Listener = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute())
		.AddLambda([Caster, &NestedActivations](const FOnAttributeChangeData& Change)
		{
			if (NestedActivations != 0 || Change.NewValue >= Change.OldValue) return;
			++NestedActivations;
			Caster->CancelCast();
			Caster->CastArea();
		});
	Caster->CastArea();
	ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(Listener);
	TestEqual(TEXT("Cost delegate performed exactly one cancel-and-recast"), NestedActivations, 1);
	TestEqual(TEXT("Both real activation attempts paid once without a cancellation refund"), Caster->GetEnergy(),
		100.0f - 2.0f * Caster->GetCombatConfig().Area.Cost);
	TestTrue(TEXT("Old activation did not end the newly reused instance"), Caster->IsCasting());
	const auto Casting = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::State_Casting));
	const auto Cooldown = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::Cooldown_Area));
	TestEqual(TEXT("Only the current activation owns a casting effect"), ASC->GetActiveEffects(Casting).Num(), 1);
	TestEqual(TEXT("Old commit does not apply its cooldown to the new activation"), ASC->GetActiveEffects(Cooldown).Num(), 1);
	Fixture.Advance(Caster->GetCombatConfig().Area.Windup + 0.10f);
	TestEqual(TEXT("Current activation releases damage exactly once"), Target->GetHealth(),
		100.0f - Caster->GetCombatConfig().Area.Amount);
	Fixture.Advance(Caster->GetCombatConfig().Area.Recovery + 0.20f);
	TestFalse(TEXT("Current recovery ends its own casting state"), Caster->IsCasting());
	TestEqual(TEXT("No orphaned infinite casting effect survives recovery"), ASC->GetActiveEffects(Casting).Num(), 0);
	TestEqual(TEXT("Old callbacks cannot release a second hit"), Target->GetHealth(),
		100.0f - Caster->GetCombatConfig().Area.Amount);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaDynamicOcclusionTest, "Multiplayer.Combat.AreaDynamicOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 从真正施法入口等待释放；动态挡板位于爆点与靶敌之间，不遮挡施法者的中心落点。
bool FCoopCombatAreaDynamicOcclusionTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Caster = Fixture.Player();
	const float Range = Caster->GetCombatConfig().Area.Range;
	auto* Blocked = Fixture.Enemy(FVector(Range, 150, 100));
	auto* Visible = Fixture.Enemy(FVector(Range, -150, 100));
	Fixture.Box(FVector(Range, 75, 100), FVector(30, 10, 150), ECC_WorldDynamic);
	Caster->CastArea();
	Fixture.Advance(Caster->GetCombatConfig().Area.Windup + 0.10f);
	TestEqual(TEXT("WorldDynamic barrier blocks actual area damage"), Blocked->GetHealth(), 100.0f);
	TestFalse(TEXT("Blocked area target receives no stun"), Blocked->IsStunned());
	TestEqual(TEXT("Unblocked target proves that the real release executed"), Visible->GetHealth(),
		100.0f - Caster->GetCombatConfig().Area.Amount);
	TestTrue(TEXT("Unblocked target receives the real stun effect"), Visible->IsStunned());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDamageAttributeBoundaryTest, "Multiplayer.Combat.DamageAttributeBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 伤害经过来源捕获、目标护甲与属性集减伤；防御不能在 Execution 和属性集各算一遍。
bool FCoopCombatDamageAttributeBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Source = Fixture.Enemy(FVector(-500, 0, 100));
	auto* Target = Fixture.Player();
	Source->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetAttackPowerAttribute(), 8.0f);
	Target->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetArmorAttribute(), 3.0f);
	TestTrue(TEXT("Damage entry accepts a living hostile target"), Source->ApplyCombatDamage(Target, 25.0f));
	TestEqual(TEXT("Execution evaluates base damage plus source attack minus target armor"), Target->GetHealth(), 70.0f);
	Target->Defend();
	TestTrue(TEXT("Defense effect is active before the second hit"), Target->IsDefending());
	Source->ApplyCombatDamage(Target, 25.0f);
	TestEqual(TEXT("Defense halves the evaluated thirty damage exactly once"), Target->GetHealth(), 55.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatStunMovementModeBoundaryTest, "Multiplayer.Combat.StunMovementModeBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 受控前已由其他系统禁行的角色，不应因眩晕结束被重新开启默认移动模式。
bool FCoopCombatStunMovementModeBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Character = Fixture.Player();
	TestEqual(TEXT("Fixture was explicitly forbidden to move before stun"), int32(Character->GetCharacterMovement()->MovementMode), int32(MOVE_None));
	Character->ApplyStun(0.25f);
	TestTrue(TEXT("Real stun effect was applied"), Character->IsStunned());
	Fixture.Advance(0.40f);
	TestFalse(TEXT("Actual GameplayEffect expiration removed the stun"), Character->IsStunned());
	TestEqual(TEXT("Stun expiration preserves the prior MOVE_None"), int32(Character->GetCharacterMovement()->MovementMode), int32(MOVE_None));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCommitDeathBoundaryTest, "Multiplayer.Combat.CommitDeathBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 扣费通知中死亡会取消旧能力；旧提交栈返回后不能重新添加冷却或防御状态。
bool FCoopCombatCommitDeathBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Caster = Fixture.Player();
	auto* Source = Fixture.Enemy(FVector(-1500, 0, 100));
	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	int32 DeathCallbacks = 0;
	FDelegateHandle Listener = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute())
		.AddLambda([Caster, Source, &DeathCallbacks](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue < Change.OldValue) { ++DeathCallbacks; Source->ApplyCombatDamage(Caster, 1000); }
		});
	Caster->Defend();
	ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(Listener);
	TestEqual(TEXT("Real resource notification caused one lethal callback"), DeathCallbacks, 1);
	TestFalse(TEXT("Lethal callback ended the caster"), Caster->IsAlive());
	TestFalse(TEXT("Old commit stack cannot recreate a defense cooldown after death"), ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Defend));
	TestFalse(TEXT("Old activation stack cannot grant defense after death"), ASC->HasMatchingGameplayTag(CombatTags::State_Defending));
	return true;
}

#endif
