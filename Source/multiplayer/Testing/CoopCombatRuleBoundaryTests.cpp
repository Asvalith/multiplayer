// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/SphereComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"
#include <limits>

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAimEventBoundaryTest, "Multiplayer.Combat.AimEventBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 用真实事件激活、起手延迟、弹道和范围伤害验证方向快照；非法新请求不能污染正在执行的技能。
bool FCoopCombatAimEventBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Config = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), Fixture.World);
	Config->Attack.Cost = 13.f;
	Config->Attack.Cooldown = 2.f;
	Config->Attack.Windup = .35f;
	Config->Attack.Recovery = .15f;
	Config->Area.Windup = .25f;
	Config->Area.Recovery = .15f;
	auto* Caster = Fixture.Player(FVector(0, 0, 100), Config);
	auto* Controller = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Aim-event caster spawned"), Caster)
		|| !TestNotNull(TEXT("Aim-event local controller spawned"), Controller)) return false;
	Controller->Possess(Caster);
	Caster->GetCharacterMovement()->DisableMovement();
	Controller->SetControlRotation(FRotator(0, -90, 0));
	auto* ASC = Caster->GetAbilitySystemComponent();
	if (!TestTrue(TEXT("Actual possession admits local GameplayEvent activation"), ASC->AbilityActorInfo->IsLocallyControlled())) return false;
	// 使用已保存的攻击 Montage 进入生产起手分支；本用例验证发弹时机，不代替独立的骨骼动画测试。
	auto* Montage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/Combat/Presentation/ARPG/AM_ARPG_Attack.AM_ARPG_Attack"));
	const auto* MontageProperty = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("AttackMontage"));
	if (!TestNotNull(TEXT("Actual attack montage is available"), Montage)
		|| !TestNotNull(TEXT("Attack montage can be selected on the character"), MontageProperty)) return false;
	MontageProperty->SetObjectPropertyValue_InContainer(Caster, Montage);
	const auto MakeEvent = [Caster](FGameplayTag EventTag, float Yaw)
	{
		FGameplayEventData Event;
		Event.EventTag = EventTag;
		Event.Instigator = Caster;
		auto* Aim = new FGameplayAbilityTargetData_LocationInfo();
		Aim->TargetLocation.LocationType = EGameplayAbilityTargetingLocationType::LiteralTransform;
		Aim->TargetLocation.LiteralTransform = FTransform(FRotator(0, Yaw, 0));
		Event.TargetData.Add(Aim);
		return Event;
	};
	const auto* AttackAbility = GetDefault<UmultiplayerAttackAbility>();
	const auto RejectPayload = [this, ASC, AttackAbility](const TCHAR* Name, const FGameplayEventData& Event)
	{
		TestFalse(*FString::Printf(TEXT("%s is rejected before activation"), Name),
			AttackAbility->ShouldAbilityRespondToEvent(ASC->AbilityActorInfo.Get(), &Event));
		TestEqual(*FString::Printf(TEXT("%s cannot activate through the real event entry"), Name),
			ASC->HandleGameplayEvent(CombatTags::Event_Attack, &Event), 0);
	};
	FGameplayEventData WrongTag = MakeEvent(CombatTags::Event_Area, 45);
	TestFalse(TEXT("Attack accepts only its exact event tag"),
		AttackAbility->ShouldAbilityRespondToEvent(ASC->AbilityActorInfo.Get(), &WrongTag));
	FGameplayEventData Missing = MakeEvent(CombatTags::Event_Attack, 45);
	Missing.TargetData.Clear();
	RejectPayload(TEXT("Missing target data"), Missing);
	FGameplayEventData Multiple = MakeEvent(CombatTags::Event_Attack, 45);
	Multiple.TargetData.Add(new FGameplayAbilityTargetData_LocationInfo());
	RejectPayload(TEXT("Multiple aim entries"), Multiple);
	FGameplayEventData WrongType = MakeEvent(CombatTags::Event_Attack, 45);
	WrongType.TargetData.Clear();
	WrongType.TargetData.Add(new FGameplayAbilityTargetData_ActorArray());
	RejectPayload(TEXT("Client-supplied actor data"), WrongType);
	FGameplayEventData ActorLocation = MakeEvent(CombatTags::Event_Attack, 45);
	static_cast<FGameplayAbilityTargetData_LocationInfo*>(ActorLocation.TargetData.Get(0))->TargetLocation.LocationType
		= EGameplayAbilityTargetingLocationType::ActorTransform;
	RejectPayload(TEXT("Actor-based target transform"), ActorLocation);
	FGameplayEventData Unnormalized = MakeEvent(CombatTags::Event_Attack, 45);
	static_cast<FGameplayAbilityTargetData_LocationInfo*>(Unnormalized.TargetData.Get(0))->TargetLocation.LiteralTransform
		.SetRotation(FQuat(0, 0, 0, 2));
	RejectPayload(TEXT("Unnormalized aim rotation"), Unnormalized);
	FGameplayEventData Nonfinite = MakeEvent(CombatTags::Event_Attack, 45);
	// 原始 register 注入避免数学构造器提前清洗 NaN；必须由技能接纳边界拒绝，而不是日志报警后变成 Identity。
	static_cast<FGameplayAbilityTargetData_LocationInfo*>(Nonfinite.TargetData.Get(0))->TargetLocation.LiteralTransform
		.SetRotationRegister(MakeVectorRegisterDouble(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 1.0));
	RejectPayload(TEXT("Nonfinite aim rotation"), Nonfinite);
	TestEqual(TEXT("All rejected payloads leave energy untouched"), Caster->GetEnergy(), 100.f);
	TestFalse(TEXT("All rejected payloads leave no cooldown"), ASC->HasMatchingGameplayTag(CombatTags::Cooldown_Attack));

	const FVector Direction = FRotator(0, 45, 0).Vector();
	auto* AttackTarget = Fixture.Enemy(Direction * 900.f + FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Target on the submitted aim spawned"), AttackTarget)) return false;
	FGameplayEventData Attack = MakeEvent(CombatTags::Event_Attack, 45);
	const FRotator SubmittedAim(0, 45, 0);
	const auto SubmittedPreview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &SubmittedAim);
	const FVector SubmittedDirection = (SubmittedPreview.End - SubmittedPreview.Origin).GetSafeNormal();
	TestEqual(TEXT("Valid attack event activates exactly one skill"), ASC->HandleGameplayEvent(CombatTags::Event_Attack, &Attack), 1);
	const FGameplayAbilitySpec* AttackSpec = ASC->FindAbilitySpecFromClass(UmultiplayerAttackAbility::StaticClass());
	if (!TestTrue(TEXT("Real GAS attack instance exists"), AttackSpec && AttackSpec->GetPrimaryInstance())) return false;
	const FPredictionKey ActivationKey = AttackSpec->GetPrimaryInstance()->GetCurrentActivationInfo().GetActivationPredictionKey();
	FGameplayEventData Duplicate = MakeEvent(CombatTags::Event_Attack, 135);
	TestEqual(TEXT("Repeated active request cannot start or pay a second time"), ASC->HandleGameplayEvent(CombatTags::Event_Attack, &Duplicate), 0);
	TestTrue(TEXT("Rejected duplicate preserves the original activation key"),
		AttackSpec->GetPrimaryInstance()->GetCurrentActivationInfo().GetActivationPredictionKey() == ActivationKey);
	TestEqual(TEXT("Valid attack pays exactly once"), Caster->GetEnergy(), 87.f);
	const auto Cooldown = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::Cooldown_Attack));
	TestEqual(TEXT("Only one attack cooldown is created"), ASC->GetActiveEffects(Cooldown).Num(), 1);
	Controller->SetControlRotation(FRotator(0, -135, 0));
	Fixture.Advance(.15f);
	int32 BeforeRelease = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) ++BeforeRelease;
	TestEqual(TEXT("Real montage windup does not emit an early projectile"), BeforeRelease, 0);
	Fixture.Advance(.22f);
	AmultiplayerCombatProjectile* Projectile = nullptr;
	int32 Released = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) { Projectile = *It; ++Released; }
	if (!TestEqual(TEXT("Windup releases one actual authoritative projectile"), Released, 1)) return false;
	TestTrue(TEXT("Actual launch converges to the submitted aim preview, not old or changed control rotation"),
		FVector(Projectile->GetLaunchState().Velocity).GetSafeNormal().Equals(SubmittedDirection, .001f));
	Fixture.Advance(1.f);
	TestEqual(TEXT("Submitted aim hits the real target through GAS once"), AttackTarget->GetHealth(), 100.f - Config->Attack.Amount);

	Controller->SetControlRotation(FRotator(0, -90, 0));
	auto* AreaTarget = Fixture.Enemy(Direction * Config->Area.Range + FVector(0, 0, 100));
	auto* OldDirectionTarget = Fixture.Enemy(FVector(0, -Config->Area.Range, 100));
	if (!TestNotNull(TEXT("Area snapshot target spawned"), AreaTarget)
		|| !TestNotNull(TEXT("Old control-direction target spawned"), OldDirectionTarget)) return false;
	FGameplayEventData Area = MakeEvent(CombatTags::Event_Area, 45);
	TestEqual(TEXT("Valid area event activates exactly one skill"), ASC->HandleGameplayEvent(CombatTags::Event_Area, &Area), 1);
	Controller->SetControlRotation(FRotator(0, 180, 0));
	Fixture.Advance(Config->Area.Windup + .10f);
	TestEqual(TEXT("Area damage lands on the submitted aim snapshot"), AreaTarget->GetHealth(), 100.f - Config->Area.Amount);
	TestEqual(TEXT("Area does not hit the old server control direction"), OldDirectionTarget->GetHealth(), 100.f);
	TestEqual(TEXT("Attack and area each pay their configured cost once"), Caster->GetEnergy(), 87.f - Config->Area.Cost);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileSweepBoundariesTest, "Multiplayer.Combat.ProjectileSweepBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 一次 150ms 的真实服务器更新覆盖整段飞行，不用小步进或直接调用命中回调代替防穿透验证。
bool FCoopCombatProjectileSweepBoundariesTest::RunTest(const FString& Parameters)
{
	struct FCase { const TCHAR* Name; float NearX; float FarX; float WallX; bool bFriend; bool bExtraBody; };
	const FCase Cases[] = {
		{ TEXT("FastStaticTarget"), 550, 0, 0, false, false },
		{ TEXT("NearestEnemyOnly"), 300, 650, 0, false, false },
		{ TEXT("EnemyBeforeWall"), 300, 650, 450, false, false },
		{ TEXT("WallBeforeEnemy"), 550, 0, 300, false, false },
		{ TEXT("PassThroughFriend"), 550, 0, 0, true, false },
		{ TEXT("MultipleTargetComponents"), 550, 0, 0, false, true }
	};
	for (const FCase& Case : Cases)
	{
		FCombatRuleBoundaryWorld Fixture;
		auto* Caster = Fixture.Player();
		auto* Near = Fixture.Enemy(FVector(Case.NearX, 0, 100));
		auto* Far = Case.FarX > 0 ? Fixture.Enemy(FVector(Case.FarX, 0, 100)) : nullptr;
		auto* Friend = Case.bFriend ? Fixture.Player(FVector(250, 0, 100)) : nullptr;
		if (!TestNotNull(*FString::Printf(TEXT("%s caster"), Case.Name), Caster)
			|| !TestNotNull(*FString::Printf(TEXT("%s target"), Case.Name), Near)) return false;
		if (Case.WallX > 0) Fixture.Box(FVector(Case.WallX, 0, 140), FVector(3, 150, 200), ECC_WorldStatic);
		if (Case.bExtraBody)
		{
			// 同一敌人的两个查询组件都能产生 overlap，业务仍只能结算一次。
			auto* Extra = NewObject<USphereComponent>(Near);
			Extra->SetupAttachment(Near->GetRootComponent());
			Extra->InitSphereRadius(45.0f);
			Extra->SetRelativeLocation(FVector(0, 0, 40));
			Extra->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Extra->SetCollisionObjectType(ECC_Pawn);
			Extra->SetCollisionResponseToAllChannels(ECR_Ignore);
			Extra->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
			Extra->SetGenerateOverlapEvents(true);
			Extra->RegisterComponent();
		}
		int32 DamageNotifications = 0;
		auto* ASC = Near->GetAbilitySystemComponent();
		const FDelegateHandle DamageListener = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute())
			.AddLambda([&DamageNotifications](const FOnAttributeChangeData& Change)
			{
				if (Change.NewValue < Change.OldValue) ++DamageNotifications;
			});
		TestTrue(*FString::Printf(TEXT("%s launches through the production entry"), Case.Name), AmultiplayerCombatProjectile::FireFromSkill(*Caster, 25, 8000));
		++GFrameCounter;
		Fixture.World->Tick(LEVELTICK_All, .15f);
		const bool bBlocked = Case.WallX > 0 && Case.WallX < Case.NearX;
		TestEqual(*FString::Printf(TEXT("%s first hostile health"), Case.Name), Near->GetHealth(), bBlocked ? 100.f : 75.f);
		TestEqual(*FString::Printf(TEXT("%s one or zero real GAS settlements"), Case.Name), DamageNotifications, bBlocked ? 0 : 1);
		if (Far) TestEqual(*FString::Printf(TEXT("%s cannot damage the second hostile"), Case.Name), Far->GetHealth(), 100.f);
		if (Friend) TestEqual(TEXT("Friendly overlap neither takes damage nor consumes the projectile"), Friend->GetHealth(), 100.f);
		int32 Remaining = 0;
		for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) ++Remaining;
		TestEqual(*FString::Printf(TEXT("%s authoritative impact removes the projectile"), Case.Name), Remaining, 0);
		ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(DamageListener);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileCloseRangeTest, "Multiplayer.Combat.ProjectileCloseRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 在原 75cm 前移区间逐点发射，涵盖出生就在敌方胶囊内和出生后第一次扫掠命中。
bool FCoopCombatProjectileCloseRangeTest::RunTest(const FString& Parameters)
{
	for (float Distance : { 0.f, 30.f, 70.f })
	{
		FCombatRuleBoundaryWorld Fixture;
		const FVector Origin(1000, 500, 100);
		const FVector Muzzle = Origin + FVector(0, 0, 40);
		auto* Caster = Fixture.Player(Origin);
		auto* Target = Fixture.Enemy(Origin + FVector(Distance, 0, 0));
		if (!TestNotNull(TEXT("Close-range caster"), Caster) || !TestNotNull(TEXT("Close-range enemy"), Target)) return false;
		// 读取真实瞬时伤害 GE 的上下文；偏离世界原点布置，才能发现非扫掠重叠的默认零坐标。
		FHitResult DamageHit;
		bool bHasHitContext = false;
		int32 DamageEffects = 0;
		auto* ASC = Target->GetAbilitySystemComponent();
		const FDelegateHandle DamageListener = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
			[&DamageHit, &bHasHitContext, &DamageEffects](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
			{
				if (!Spec.Def || !Spec.Def->IsA<UmultiplayerDamageEffect>()) return;
				++DamageEffects;
				if (const FHitResult* Hit = Spec.GetContext().GetHitResult())
				{
					DamageHit = *Hit;
					bHasHitContext = true;
				}
			});
		TestTrue(*FString::Printf(TEXT("%.0fcm enemy does not become a rejected scene obstruction"), Distance), AmultiplayerCombatProjectile::FireFromSkill(*Caster, 25, 8000));
		Fixture.Advance(.20f);
		TestEqual(*FString::Printf(TEXT("%.0fcm enemy receives one hit, including an initial overlap"), Distance), Target->GetHealth(), 75.f);
		TestTrue(TEXT("Real damage GE carries a hit context"), bHasHitContext);
		TestTrue(TEXT("The real hit context identifies the hostile, not an empty overlap"), DamageHit.GetActor() == Target);
		TestTrue(TEXT("Impact presentation uses a nearby authoritative position, not world zero"),
			!DamageHit.ImpactPoint.IsNearlyZero() && FVector::DistSquared(DamageHit.ImpactPoint, Muzzle) <= FMath::Square(Distance + 12.f));
		if (Distance <= 30.f)
			TestTrue(TEXT("Non-swept initial overlap records the server projectile position"), DamageHit.ImpactPoint.Equals(Muzzle, .01f));
		Fixture.Advance(.20f);
		TestEqual(TEXT("Initial overlap and subsequent sweep cannot settle twice"), Target->GetHealth(), 75.f);
		TestEqual(TEXT("Initial overlap produces exactly one real damage GE"), DamageEffects, 1);
		ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(DamageListener);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTriggerOcclusionBoundaryTest, "Multiplayer.Combat.TriggerOcclusionBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 机关触发盒不是实体墙；只有真正阻挡 Visibility 的静态或动态场景组件才截断目标和伤害。
bool FCoopCombatTriggerOcclusionBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatRuleBoundaryWorld Fixture;
	auto* Caster = Fixture.Player();
	if (!TestNotNull(TEXT("Occlusion caster"), Caster)) return false;
	const FVector Baseline = Caster->GetAreaTarget();
	const float Range = Caster->GetCombatConfig().Area.Range;
	auto* Target = Fixture.Enemy(FVector(Range, 150, 100));
	if (!TestNotNull(TEXT("Area target beyond a nonblocking trigger"), Target)) return false;
	for (const FVector& Center : { FVector(200, 0, 100), FVector(Range, 75, 100) })
	{
		auto* Trigger = Fixture.Box(Center, FVector(30, 10, 150), ECC_WorldDynamic)->FindComponentByClass<UBoxComponent>();
		Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
		Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		Trigger->SetGenerateOverlapEvents(true);
	}
	TestTrue(TEXT("QueryOnly Pawn-overlap trigger does not change the real ground target"), Caster->GetAreaTarget().Equals(Baseline, .1f));
	Caster->CastArea();
	Fixture.Advance(Caster->GetCombatConfig().Area.Windup + .10f);
	TestEqual(TEXT("A Visibility-ignoring trigger does not hide a hostile from the area release"), Target->GetHealth(),
		100.f - Caster->GetCombatConfig().Area.Amount);
	TestTrue(TEXT("The same unblocked release applies its real stun effect"), Target->IsStunned());
	// 同样是 WorldDynamic，但实体门仍须遮挡；不能靠排除整个对象类型修复触发盒问题。
	Fixture.Box(FVector(250, 0, 100), FVector(5, 150, 200), ECC_WorldDynamic);
	TestTrue(TEXT("A new obstacle does not move the already confirmed casting circle"), Caster->GetAreaTarget().Equals(Baseline, .1f));
	Fixture.Advance(Caster->GetCombatConfig().Area.Recovery + .2f);
	TestFalse(TEXT("Completed recovery allows querying the next landing point"), Caster->IsCasting());
	const FVector BlockedTarget = Caster->GetAreaTarget();
	TestTrue(TEXT("An actual BlockAll dynamic door truncates the target before the original range"),
		BlockedTarget.X > 200.f && BlockedTarget.X < 300.f && BlockedTarget.X < Baseline.X);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDamageAttributeBoundaryTest, "Multiplayer.Combat.DamageAttributeBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 伤害经过来源捕获、目标护甲与属性集格挡；防御不能在 Execution 和属性集各算一遍。
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
	TestEqual(TEXT("Rear damage keeps the evaluated thirty damage while defense is active"), Target->GetHealth(), 40.0f);
	Target->SetActorRotation(FRotator(0, 180, 0));
	Source->ApplyCombatDamage(Target, 25.0f);
	TestEqual(TEXT("Turning the body toward the source fully blocks that same evaluated attack"), Target->GetHealth(), 40.0f);
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
