// Copyright Epic Games, Inc. All Rights Reserved.
// 通过真实 ASC、延时任务与控制器交接验证收敛后的规则；无导航，不声称覆盖追击画面。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatTargeting.h"
#include "Testing/CoopCombatRelationshipTestProbe.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BrainComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/CoreRedirects.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
struct FCombatRefactorWorld
{
	UWorld* World = nullptr;
	uint64 PreviousFrame = GFrameCounter;
	FDelegateHandle SpawnObserver;
	int32 ProjectileCount = 0;
	TWeakObjectPtr<AmultiplayerCombatProjectile> LastProjectile;

	FCombatRefactorWorld()
	{
		UWorld::InitializationValues Values;
		Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
			.CreateNavigation(false).CreateAISystem(true);
		World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		World->SetBegunPlay(true);
		SpawnObserver = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
		{
			if (auto* Projectile = Cast<AmultiplayerCombatProjectile>(Actor))
			{
				++ProjectileCount;
				LastProjectile = Projectile;
			}
		}));
	}
	~FCombatRefactorWorld()
	{
		World->RemoveOnActorSpawnedHandler(SpawnObserver);
		World->EndPlay(EEndPlayReason::Quit);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		GFrameCounter = PreviousFrame;
	}
	void Advance(float Seconds)
	{
		for (float Elapsed = 0.f; Elapsed < Seconds;)
		{
			const float Step = FMath::Min(.01f, Seconds - Elapsed);
			++GFrameCounter;
			World->Tick(LEVELTICK_All, Step);
			Elapsed += Step;
		}
	}
	// 只在 BeginPlay 前配置原生实例，正式技能和属性初始化仍走角色已有入口。
	AmultiplayerCombatEnemy* Enemy(bool bMelee, float Range = 1100.f, float Speed = 1600.f)
	{
		auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(AmultiplayerCombatEnemy::StaticClass(),
			FTransform(FVector(0, 0, 100)), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Pawn) return nullptr;
		Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
		auto* Config = NewObject<UmultiplayerCombatConfig>(Pawn);
		Config->Attack = FmultiplayerCombatSkillRule(7.f, .05f, 10.f, Range, 0.f, .25f, .35f);
		Config->ProjectileSpeed = Speed;
		FindFProperty<FObjectPropertyBase>(Pawn->GetClass(), TEXT("CombatConfig"))->SetObjectPropertyValue_InContainer(Pawn, Config);
		FindFProperty<FClassProperty>(Pawn->GetClass(), TEXT("AttackAbilityClass"))->SetObjectPropertyValue_InContainer(Pawn,
			bMelee ? UmultiplayerEnemyMeleeAbility::StaticClass() : UmultiplayerAttackAbility::StaticClass());
		Pawn->FinishSpawning(FTransform(FVector(0, 0, 100)));
		Pawn->GetCharacterMovement()->DisableMovement();
		return Pawn;
	}
	AmultiplayerCombatAIController* Control(AmultiplayerCombatEnemy& Pawn)
	{
		auto* AI = World->SpawnActor<AmultiplayerCombatAIController>();
		if (!AI) return nullptr;
		AI->Possess(&Pawn);
		if (auto* Brain = AI->GetBrainComponent())
		{
			Brain->StopLogic(TEXT("Inspect one explicit GAS attack"));
			Brain->PrimaryComponentTick.UnRegisterTickFunction();
		}
		Pawn.GetCharacterMovement()->DisableMovement();
		return AI;
	}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorEnergyCostTest, "Multiplayer.Combat.Refactor.SharedEnergyCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorEnergyCostTest::RunTest(const FString& Parameters)
{
	for (UClass* AbilityClass : {UmultiplayerAttackAbility::StaticClass(), UmultiplayerEnemyMeleeAbility::StaticClass(),
		UmultiplayerDefendAbility::StaticClass(), UmultiplayerHealAbility::StaticClass(), UmultiplayerAreaAbility::StaticClass(),
		UmultiplayerDashAbility::StaticClass()})
	{
		UGameplayEffect* Cost = AbilityClass->GetDefaultObject<UGameplayAbility>()->GetCostGameplayEffect();
		if (!TestNotNull(TEXT("Each skill has its GAS cost definition"), Cost)) return false;
		TestTrue(TEXT("Every skill uses the same native energy cost class"), Cost->GetClass() == UmultiplayerEnergyCostEffect::StaticClass());
	}
	const auto* Cost = GetDefault<UmultiplayerEnergyCostEffect>();
	TestEqual(TEXT("Energy cost defines exactly one modifier"), Cost->Modifiers.Num(), 1);
	for (const TCHAR* OldName : {TEXT("multiplayerDefendCostEffect"), TEXT("multiplayerHealCostEffect"),
		TEXT("multiplayerAreaCostEffect"), TEXT("multiplayerDashCostEffect")})
	{
		const FString Path = FString(TEXT("/Script/multiplayer.")) + OldName;
		const FString ExpectedPath = UmultiplayerEnergyCostEffect::StaticClass()->GetPathName();
		// 与 LinkerLoad 的类导入使用同一个 Type_Class 解析契约；不把 raw LoadClass 当作重定向入口。
		const FCoreRedirectObjectName Redirected = FCoreRedirects::GetRedirectedName(
			ECoreRedirectFlags::Type_Class, FCoreRedirectObjectName(Path));
		TestEqual(TEXT("Saved class imports resolve each old name to the shared class"), Redirected.ToString(), ExpectedPath);
		// 原生软引用先经过引擎的 CoreRedirect 修正，再加载实际类，不保留旧兼容空类。
		FSoftObjectPath SoftPath(Path);
		TestTrue(TEXT("Native soft class reference receives the configured redirect"), SoftPath.FixupCoreRedirects());
		TestEqual(TEXT("Soft reference serializes the canonical native path"), SoftPath.ToString(), ExpectedPath);
		TestTrue(TEXT("The resolved soft reference loads the real shared class"),
			SoftPath.TryLoad() == UmultiplayerEnergyCostEffect::StaticClass());
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorRangeTest, "Multiplayer.Combat.Refactor.EffectiveAttackRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorRangeTest::RunTest(const FString& Parameters)
{
	FCombatRefactorWorld Fixture;
	auto* Melee = Fixture.Enemy(true, 160.f);
	auto* Ranged = Fixture.Enemy(false, 1100.f);
	auto* LifetimeLimited = Fixture.Enemy(false, 1100.f, 100.f);
	auto* NativeUnlimited = Fixture.Enemy(false, 0.f, 100.f);
	if (!TestNotNull(TEXT("Configured melee"), Melee) || !TestNotNull(TEXT("Configured ranged"), Ranged)
		|| !TestNotNull(TEXT("Lifetime limited ranged"), LifetimeLimited) || !TestNotNull(TEXT("Zero-range ranged"), NativeUnlimited)) return false;
	TestNull(TEXT("No second editable AI range survives"), FindFProperty<FFloatProperty>(Melee->GetClass(), TEXT("AttackDistance")));
	TestEqual(TEXT("Melee decision range derives from its skill"), Melee->GetAttackDistance(), 160.f);
	TestEqual(TEXT("Formal ranged range stays eleven metres"), Ranged->GetAttackDistance(), 1100.f);
	TestEqual(TEXT("Projectile lifetime also bounds AI firing range"), LifetimeLimited->GetAttackDistance(), 300.f);
	TestEqual(TEXT("Zero explicit range preserves projectile lifetime semantics"), NativeUnlimited->GetAttackDistance(), 300.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorAttackTimingTest, "Multiplayer.Combat.Refactor.AttackWithoutMontageUsesTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorAttackTimingTest::RunTest(const FString& Parameters)
{
	FCombatRefactorWorld Fixture;
	auto* Enemy = Fixture.Enemy(false);
	if (!TestNotNull(TEXT("Native attack owner"), Enemy)) return false;
	TestNull(TEXT("Timing fixture intentionally has no presentation montage"), Enemy->GetActionMontage(EMultiplayerCombatAnimation::Attack));
	const float Energy = Enemy->GetEnergy();
	if (!TestTrue(TEXT("Actual GAS attack starts"), Enemy->ActivateSkill(EMultiplayerCombatSkill::Attack))) return false;
	TestEqual(TEXT("Shared cost is committed exactly once"), Enemy->GetEnergy(), Energy - 7.f);
	TestEqual(TEXT("No instant projectile without a montage"), Fixture.ProjectileCount, 0);
	Fixture.Advance(.1f);
	TestEqual(TEXT("Configured windup has not elapsed"), Fixture.ProjectileCount, 0);
	Fixture.Advance(.2f);
	TestEqual(TEXT("Windup releases exactly one real projectile"), Fixture.ProjectileCount, 1);
	const auto* Spec = Enemy->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass());
	if (!TestNotNull(TEXT("Granted attack spec"), Spec)) return false;
	TestTrue(TEXT("No montage does not skip recovery"), Spec->IsActive());
	Fixture.Advance(.4f);
	TestFalse(TEXT("Actual recovery task ends the ability"), Spec->IsActive());
	TestEqual(TEXT("Recovery did not release a second projectile"), Fixture.ProjectileCount, 1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorUnpossessTest, "Multiplayer.Combat.Refactor.UnpossessCancelsAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorUnpossessTest::RunTest(const FString& Parameters)
{
	FCombatRefactorWorld Fixture;
	auto* Enemy = Fixture.Enemy(false);
	if (!TestNotNull(TEXT("Ranged enemy"), Enemy)) return false;
	auto* AI = Fixture.Control(*Enemy);
	if (!TestNotNull(TEXT("Actual combat controller"), AI)) return false;
	if (!TestTrue(TEXT("Ranged windup starts through GAS"), Enemy->ActivateSkill(EMultiplayerCombatSkill::Attack))) return false;
	Fixture.Advance(.1f);
	AI->UnPossess();
	Fixture.Advance(.6f);
	TestEqual(TEXT("An unpossessed windup cannot emit a delayed projectile"), Fixture.ProjectileCount, 0);
	const auto* Spec = Enemy->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass());
	if (!TestNotNull(TEXT("Attack spec remains granted"), Spec)) return false;
	TestFalse(TEXT("Unpossess cleans the active GAS transaction"), Spec->IsActive());
	AI->Possess(Enemy);
	AI->GetBrainComponent()->StopLogic(TEXT("Inspect recovery handoff"));
	Enemy->GetCharacterMovement()->DisableMovement();
	if (!TestTrue(TEXT("Repossessed enemy can start a fresh attack"), Enemy->ActivateSkill(EMultiplayerCombatSkill::Attack))) return false;
	Fixture.Advance(.3f);
	if (!TestEqual(TEXT("Fresh attack releases exactly once"), Fixture.ProjectileCount, 1)) return false;
	AI->UnPossess();
	TestTrue(TEXT("Cancelling recovery does not revoke an already released projectile"), Fixture.LastProjectile.IsValid());
	TestFalse(TEXT("Recovery transaction also ends on loss of control"), Spec->IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorFailurePresentationTest, "Multiplayer.Combat.Refactor.FailurePresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorFailurePresentationTest::RunTest(const FString& Parameters)
{
	FCombatRefactorWorld Fixture;
	auto* Character = Fixture.World->SpawnActor<AmultiplayerCombatCharacter>();
	auto* Controller = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Failure owner"), Character) || !TestNotNull(TEXT("Local player"), Controller)) return false;
	Controller->Possess(Character);
	auto* Probe = NewObject<UCoopCombatRelationshipTestProbe>(Character);
	Character->OnCombatMessage.AddDynamic(Probe, &UCoopCombatRelationshipTestProbe::HandleMessage);
	FGameplayTagContainer Reasons(CombatTags::Failure_InvalidAim);
	Character->ReportSkillFailure(EMultiplayerCombatSkill::Attack, &Reasons);
	Character->ClientSkillRejected(EMultiplayerCombatSkill::Heal, CombatTags::Failure_InsufficientEnergy, 1.f);
	if (!TestEqual(TEXT("Both sources emit one failure through the shared sink"), Probe->Messages.Num(), 2)) return false;
	TestTrue(TEXT("Local failure preserves its supplied reason"), Probe->Messages[0].EqualTo(
		Character->GetSkillFailureText(EMultiplayerCombatSkill::Attack, CombatTags::Failure_InvalidAim)));
	TestTrue(TEXT("Authority failure preserves its authoritative value rather than querying again"), Probe->Messages[1].EqualTo(
		Character->GetSkillFailureText(EMultiplayerCombatSkill::Heal, CombatTags::Failure_InsufficientEnergy, 1.f)));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRefactorMeleeUnpossessTest, "Multiplayer.Combat.Refactor.UnpossessCancelsMelee",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCombatRefactorMeleeUnpossessTest::RunTest(const FString& Parameters)
{
	FCombatRefactorWorld Fixture;
	auto* Enemy = Fixture.Enemy(true, 160.f);
	if (!TestNotNull(TEXT("Melee enemy"), Enemy)) return false;
	auto* AI = Fixture.Control(*Enemy);
	auto* Target = Fixture.World->SpawnActor<AmultiplayerCombatCharacter>(FVector(120, 0, 100), FRotator::ZeroRotator);
	auto* Player = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Melee controller"), AI) || !TestNotNull(TEXT("Melee target"), Target)
		|| !TestNotNull(TEXT("Target controller"), Player) || !TestNotNull(TEXT("Actual combat blackboard"), AI->GetBlackboardComponent())) return false;
	// UE5.5 的 IsPlayerControlled 读取 PlayerState，不仅检查是否被 PlayerController Possess。
	Player->SetPlayerState(Fixture.World->SpawnActor<APlayerState>());
	Player->Possess(Target);
	Target->GetCharacterMovement()->DisableMovement();
	AI->GetBlackboardComponent()->SetValueAsObject(CombatAIKeys::TargetActor, Target);
	if (!TestTrue(TEXT("Melee target has the engine's real player-controlled qualification"), Target->IsPlayerControlled())
		|| !TestTrue(TEXT("Melee target starts within real range and unobstructed"), CombatTargeting::CanMeleeHit(*Enemy, *Target, false))) return false;
	const float Health = Target->GetHealth();
	if (!TestTrue(TEXT("Real melee windup starts"), Enemy->ActivateSkill(EMultiplayerCombatSkill::Attack))) return false;
	TestTrue(TEXT("Melee owns its GAS casting lock"), Enemy->IsCasting());
	AI->UnPossess();
	TestFalse(TEXT("Unpossess removes the cancelled melee's casting lock"), Enemy->IsCasting());
	Fixture.Advance(.8f);
	TestEqual(TEXT("Cancelled melee cannot damage its captured target later"), Target->GetHealth(), Health);
	const auto* Spec = Enemy->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerEnemyMeleeAbility::StaticClass());
	if (!TestNotNull(TEXT("Melee spec remains granted"), Spec)) return false;
	TestFalse(TEXT("Melee tasks finish through normal cancellation cleanup"), Spec->IsActive());
	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
