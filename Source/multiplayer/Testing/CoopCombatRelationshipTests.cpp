// Copyright Epic Games, Inc. All Rights Reserved.

// 回归集中标签关系的继承合并、原生阻断计数和攻击被治疗／范围／救援替换后的清理。
// 使用保存玩家与真实起手任务，不用只读关系表的结果代替实际技能生命周期。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatAbilitySystem.h"
#include "Misc/ScopeExit.h"
#include "Testing/CoopCombatRelationshipTestProbe.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	// 克隆配置在 BeginPlay 前挂到真实玩家蓝图；保留其 Montage，让替换发生在真实起手任务中。
	struct FCombatRelationshipWorld
	{
		UWorld* World;
		UClass* PlayerClass;
		UmultiplayerCombatConfig* Config;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle ProjectileSpawnObserver;
		TArray<TWeakObjectPtr<AmultiplayerCombatProjectile>> SpawnedProjectiles;

		// 建立施法地面与真实弹体观察，并克隆短时规则而不修改共享资产。
		FCombatRelationshipWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			ProjectileSpawnObserver = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (auto* Projectile = Cast<AmultiplayerCombatProjectile>(Actor)) SpawnedProjectiles.Add(Projectile);
			}));
			auto* Ground = World->SpawnActor<AActor>();
			auto* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(5000, 5000, 10));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0, 0, -10));
			PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
			Config = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), World);
			Config->Attack.Windup = .35f;
			Config->Attack.Recovery = .2f;
			Config->Area.Windup = .35f;
			Config->Area.Recovery = .2f;
			Config->BleedoutDuration = 3.f;
			Config->RescueDuration = .4f;
		}

		// 移除生成监听再结束真实世界，使旧任务与回调不能影响下一用例。
		~FCombatRelationshipWorld()
		{
			World->RemoveOnActorSpawnedHandler(ProjectileSpawnObserver);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 在保存蓝图 BeginPlay 前挂测试规则，再以真实控制器接管和授予能力。
		AmultiplayerCombatCharacter* Player(const FVector& Location = FVector(0, 0, 100), bool bEnableDowned = false)
		{
			if (!PlayerClass) return nullptr;
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Character = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(PlayerClass, Transform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Character) return nullptr;
			const auto* Property = FindFProperty<FObjectPropertyBase>(Character->GetClass(), TEXT("CombatConfig"));
			if (!Property) { Character->Destroy(); return nullptr; }
			Property->SetObjectPropertyValue_InContainer(Character, Config);
			Character->FinishSpawning(Transform);
			Character->GetCoopComponent()->bEnableDowned = bEnableDowned;
			auto* Controller = World->SpawnActor<APlayerController>();
			Controller->SetPlayerState(World->SpawnActor<APlayerState>());
			Controller->Possess(Character);
			Controller->SetControlRotation(FRotator(-35, 0, 0));
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			return Character;
		}

		// 推进真正的能力延迟任务及冷却，让替换后的旧出手有机会暴露残留。
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

		// 返回累计实际出生数，而不是当前存活弹体数，避免错误发弹后销毁造成假通过。
		int32 Projectiles() const
		{
			// 保留真实出生记录；错误发弹即使随后撞墙销毁，也不能让取消用例假通过。
			return SpawnedProjectiles.Num();
		}
	};

	// 从真实角色读取项目 ASC，以正式集中关系和阻断容器作为被测对象。
	UmultiplayerCombatAbilitySystem* CombatASC(AmultiplayerCombatCharacter* Character)
	{
		return Character ? Cast<UmultiplayerCombatAbilitySystem>(Character->GetAbilitySystemComponent()) : nullptr;
	}

	// 查询已授予 Spec 的活动状态，不以角色表现标签推断能力是否仍运行。
	bool AbilityActive(AmultiplayerCombatCharacter* Character, TSubclassOf<UGameplayAbility> Class)
	{
		const auto* Spec = Character->GetAbilitySystemComponent()->FindAbilitySpecFromClass(Class);
		return Spec && Spec->IsActive();
	}

	// 查询角色实际配置的攻击能力，兼容保存蓝图的派生类。
	bool AttackActive(AmultiplayerCombatCharacter* Character)
	{
		return AbilityActive(Character, Character->GetAttackAbilityClass());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipDefaultMatrixTest, "Multiplayer.Combat.Relationships.DefaultMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 核对默认及旧资产继承的准入矩阵，并通过真实角色查询确认阻断生效。
bool FCoopCombatRelationshipDefaultMatrixTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved player blueprint is available"), Player)) return false;
	auto* ASC = CombatASC(Player);
	if (!TestNotNull(TEXT("Player owns the relationship-aware ASC"), ASC)) return false;
	const FGameplayTag Skills[] = {CombatTags::Ability_Attack, CombatTags::Ability_Defend, CombatTags::Ability_Heal,
		CombatTags::Ability_Area, CombatTags::Ability_Dash, CombatTags::Ability_Rescue};
	const FGameplayTag CommonStates[] = {CombatTags::State_Dead, CombatTags::State_Downed, CombatTags::State_Stunned,
		CombatTags::State_Casting, CombatTags::State_Rescuing};
	// 旧资产没有序列化新字段时，也必须继承原生默认关系；这里只查询，不改资产。
	for (const TCHAR* Path : {TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig"),
		TEXT("/Game/Combat/Escape/DA_EscapeCombat.DA_EscapeCombat")})
	{
		const auto* SavedRules = LoadObject<UmultiplayerCombatConfig>(nullptr, Path);
		if (!TestNotNull(*FString::Printf(TEXT("Saved config loads: %s"), Path), SavedRules)) return false;
		for (FGameplayTag Skill : Skills)
		{
			FGameplayTagContainer Required, Blocked, Block, Cancel;
			SavedRules->GetAbilityActivationTagRequirements(FGameplayTagContainer(Skill), Required, Blocked);
			SavedRules->GetAbilityBlockAndCancelTags(FGameplayTagContainer(Skill), Block, Cancel);
			TestTrue(TEXT("Saved default rules do not introduce an additional required state"), Required.IsEmpty());
			for (FGameplayTag State : CommonStates)
				TestTrue(*FString::Printf(TEXT("%s: %s inherits %s"), Path, *Skill.ToString(), *State.ToString()), Blocked.HasTagExact(State));
			if (Skill == CombatTags::Ability_Defend)
				TestTrue(TEXT("Saved Q retains its guard restriction"), Blocked.HasTagExact(CombatTags::State_Defending));
			if (Skill == CombatTags::Ability_Dash || Skill == CombatTags::Ability_Rescue)
				for (FGameplayTag State : {CombatTags::State_Dashing.GetTag(), CombatTags::State_MovementBlocked.GetTag(), CombatTags::State_Rooted.GetTag()})
					TestTrue(TEXT("Saved movement and rescue restrictions are inherited"), Blocked.HasTagExact(State));
			if (Skill == CombatTags::Ability_Heal || Skill == CombatTags::Ability_Area || Skill == CombatTags::Ability_Rescue)
				TestTrue(TEXT("Saved replacement rules cancel attack"), Cancel.HasTagExact(CombatTags::Ability_Attack));
			if (Skill == CombatTags::Ability_Rescue)
				TestTrue(TEXT("Saved rescue blocks the combat parent"), Block.HasTagExact(CombatTags::Ability_Combat));
		}
	}
	for (FGameplayTag State : CommonStates)
	{
		ASC->AddLooseGameplayTag(State);
		for (FGameplayTag Skill : Skills)
		{
			FGameplayTagContainer Required, Blocked;
			Fixture.Config->GetAbilityActivationTagRequirements(FGameplayTagContainer(Skill), Required, Blocked);
			TestTrue(*FString::Printf(TEXT("%s inherits %s"), *Skill.ToString(), *State.ToString()), Blocked.HasTagExact(State));
			TestFalse(TEXT("The actual ASC rejects the inherited blocked state"), ASC->CheckAbilityTagRelationships(FGameplayTagContainer(Skill)));
		}
		ASC->RemoveLooseGameplayTag(State);
	}
	FGameplayTagContainer Required, Blocked, Block, Cancel;
	Fixture.Config->GetAbilityActivationTagRequirements(FGameplayTagContainer(CombatTags::Ability_Defend), Required, Blocked);
	TestTrue(TEXT("Q cannot stack an existing guard"), Blocked.HasTagExact(CombatTags::State_Defending));
	for (FGameplayTag Skill : {CombatTags::Ability_Dash.GetTag(), CombatTags::Ability_Rescue.GetTag()})
	{
		Required.Reset(); Blocked.Reset();
		Fixture.Config->GetAbilityActivationTagRequirements(FGameplayTagContainer(Skill), Required, Blocked);
		for (FGameplayTag State : {CombatTags::State_Dashing.GetTag(), CombatTags::State_MovementBlocked.GetTag(), CombatTags::State_Rooted.GetTag()})
			TestTrue(TEXT("Dash and rescue retain their movement-only restrictions"), Blocked.HasTagExact(State));
	}
	Fixture.Config->GetAbilityBlockAndCancelTags(FGameplayTagContainer(CombatTags::Ability_Rescue), Block, Cancel);
	TestTrue(TEXT("Rescue blocks the combat ability parent"), Block.HasTagExact(CombatTags::Ability_Combat));
	TestTrue(TEXT("Rescue replaces an in-flight attack"), Cancel.HasTagExact(CombatTags::Ability_Attack));
	TestEqual(TEXT("Relationship queries never spend energy"), Player->GetEnergy(), 100.f);
	Player->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
	if (!TestTrue(TEXT("Dash is really learned before querying its granted ability"),
		Player->GetProgressionComponent()->GetSkillLevel(EMultiplayerCombatSkill::Dash) > 0)) return false;
	const auto* Dash = ASC->FindAbilitySpecFromClass(UmultiplayerDashAbility::StaticClass());
	if (!TestNotNull(TEXT("Learning dash grants its real GAS spec"), Dash)) return false;
	FGameplayTagContainer DashReasons;
	Player->GetCharacterMovement()->SetMovementMode(MOVE_None);
	TestFalse(TEXT("Actual dash CanActivate rejects a non-tag CMC movement lock"),
		Dash->Ability->CanActivateAbility(Dash->Handle, ASC->AbilityActorInfo.Get(), nullptr, nullptr, &DashReasons));
	TestTrue(TEXT("The common query preserves the precise movement failure reason"),
		DashReasons.HasTagExact(CombatTags::Failure_MovementBlocked));
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	DashReasons.Reset();
	TestTrue(TEXT("Actual dash CanActivate permits movement again after walking is restored"),
		Dash->Ability->CanActivateAbility(Dash->Handle, ASC->AbilityActorInfo.Get(), nullptr, nullptr, &DashReasons));
	Player->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	Player->Defend();
	if (!TestTrue(TEXT("Real Q leaves its independent guard effect"), Player->IsDefending())) return false;
	Player->ApplyRoot(.8f);
	Player->Attack();
	if (!TestTrue(TEXT("Root and guard still permit a real delayed attack"), AttackActive(Player)
		&& ASC->HasMatchingGameplayTag(CombatTags::State_Attacking) && Player->GetActionMontage(EMultiplayerCombatAnimation::Attack))) return false;
	TestTrue(TEXT("The attack does not replace the guard GE"), Player->IsDefending());
	Fixture.Advance(Fixture.Config->Attack.Windup + Fixture.Config->Attack.Recovery + .1f);
	TestEqual(TEXT("Rooted attack actually releases one authoritative projectile"), Fixture.Projectiles(), 1);
	TestTrue(TEXT("Guard outlives the completed attack ability"), Player->IsDefending());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipHierarchyTest, "Multiplayer.Combat.Relationships.HierarchyAndRequirements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 父分类和具体技能关系共同追加，保留调用方原生要求及阻断／取消参数。
bool FCoopCombatRelationshipHierarchyTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	Fixture.Config->AbilityRelationships.Reset();
	auto& Parent = Fixture.Config->AbilityRelationships.AddDefaulted_GetRef();
	Parent.AbilityTag = CombatTags::Ability_Combat;
	Parent.ActivationRequiredTags.AddTag(CombatTags::State_Burning);
	Parent.ActivationBlockedTags.AddTag(CombatTags::State_Stunned);
	Parent.AbilityTagsToBlock.AddTag(CombatTags::Ability_Dash);
	Parent.AbilityTagsToCancel.AddTag(CombatTags::Ability_Area);
	auto& Child = Fixture.Config->AbilityRelationships.AddDefaulted_GetRef();
	Child.AbilityTag = CombatTags::Ability_Heal;
	Child.ActivationRequiredTags.AddTag(CombatTags::State_Defending);
	Child.ActivationBlockedTags.AddTag(CombatTags::State_Rooted);
	Child.AbilityTagsToBlock.AddTag(CombatTags::Ability_Attack);
	Child.AbilityTagsToCancel.AddTag(CombatTags::Ability_Defend);
	auto& AdditionalParent = Fixture.Config->AbilityRelationships.AddDefaulted_GetRef();
	AdditionalParent.AbilityTag = CombatTags::Ability_Combat;
	AdditionalParent.ActivationRequiredTags.AddTag(CombatTags::State_Parrying);
	AdditionalParent.ActivationBlockedTags.AddTag(CombatTags::State_Dashing);
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Hierarchy pawn uses its cloned rules"), Player)) return false;
	auto* ASC = CombatASC(Player);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	const FGameplayTagContainer HealTags(CombatTags::Ability_Heal);
	FGameplayTagContainer Required, Blocked, Block, Cancel;
	Fixture.Config->GetAbilityActivationTagRequirements(HealTags, Required, Blocked);
	Fixture.Config->GetAbilityBlockAndCancelTags(HealTags, Block, Cancel);
	TestEqual(TEXT("Child receives required tags from both parent rows and its own row"), Required.Num(), 3);
	TestEqual(TEXT("All three blocked conditions accumulate"), Blocked.Num(), 3);
	TestTrue(TEXT("Parent and child block lists merge"), Block.HasTagExact(CombatTags::Ability_Dash) && Block.HasTagExact(CombatTags::Ability_Attack));
	TestTrue(TEXT("Parent and child cancel lists merge"), Cancel.HasTagExact(CombatTags::Ability_Area) && Cancel.HasTagExact(CombatTags::Ability_Defend));
	FGameplayTagContainer ParentRequired, ParentBlocked;
	Fixture.Config->GetAbilityActivationTagRequirements(FGameplayTagContainer(CombatTags::Ability_Combat), ParentRequired, ParentBlocked);
	TestFalse(TEXT("A child row does not apply backwards to the parent"), ParentRequired.HasTagExact(CombatTags::State_Defending));
	const auto* GrantedHeal = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
	if (!TestTrue(TEXT("Hierarchy checks use the actually granted heal ability"), GrantedHeal && GrantedHeal->Ability)) return false;
	FGameplayTagContainer RequirementReasons;
	TestFalse(TEXT("Native tag filtering rejects the granted ability when requirements are missing"),
		GrantedHeal->Ability->DoesAbilitySatisfyTagRequirements(*ASC, nullptr, nullptr, &RequirementReasons));
	TestTrue(TEXT("Native filtering preserves the central missing-requirements reason"), RequirementReasons.HasTagExact(CombatTags::Failure_Requirements));
	TestFalse(TEXT("Missing requirements reject a real GAS heal"), ASC->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()));
	TestEqual(TEXT("Rejected activation has no cost"), Player->GetEnergy(), 100.f);
	TestEqual(TEXT("Rejected activation has no cooldown"), Player->GetHealCooldownRemaining(), 0.f);
	for (FGameplayTag State : {CombatTags::State_Burning.GetTag(), CombatTags::State_Defending.GetTag()}) ASC->AddLooseGameplayTag(State);
	TestFalse(TEXT("All required tags are necessary, not merely one"), ASC->CheckAbilityTagRelationships(HealTags));
	ASC->AddLooseGameplayTag(CombatTags::State_Parrying);
	TestTrue(TEXT("Meeting every requirement admits the actual ASC"), ASC->CheckAbilityTagRelationships(HealTags));
	TestTrue(TEXT("Native tag filtering admits the granted ability after every requirement is met"),
		GrantedHeal->Ability->DoesAbilitySatisfyTagRequirements(*ASC));
	for (FGameplayTag State : {CombatTags::State_Stunned.GetTag(), CombatTags::State_Rooted.GetTag(), CombatTags::State_Dashing.GetTag()})
	{
		ASC->AddLooseGameplayTag(State);
		TestFalse(TEXT("Each accumulated blocked tag independently rejects activation"), ASC->CheckAbilityTagRelationships(HealTags));
		ASC->RemoveLooseGameplayTag(State);
	}
	TestTrue(TEXT("A real heal activates after satisfying the accumulated rules"), ASC->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()));
	TestEqual(TEXT("The admitted heal settles its real configured amount"), Player->GetHealth(), 40.f + Fixture.Config->Heal.Amount);
	TestEqual(TEXT("The admitted heal spends its real configured energy"), Player->GetEnergy(), 100.f - Fixture.Config->Heal.Cost);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipBlockCountsTest, "Multiplayer.Combat.Relationships.NativeBlockReferenceCounts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 多个能力来源阻断同一分类时沿用 GAS 引用计数，结束一方不能解开另一方限制。
bool FCoopCombatRelationshipBlockCountsTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	for (FGameplayTag Source : {CombatTags::Ability_Defend.GetTag(), CombatTags::Ability_Area.GetTag()})
	{
		auto& Row = Fixture.Config->AbilityRelationships.AddDefaulted_GetRef();
		Row.AbilityTag = Source;
		Row.AbilityTagsToBlock.AddTag(CombatTags::Ability_Heal);
	}
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Reference-count pawn spawned"), Player)) return false;
	auto* ASC = CombatASC(Player);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	const FGameplayTagContainer HealTags(CombatTags::Ability_Heal), NativeBlock(CombatTags::Ability_Dash), Empty;
	const auto SetSource = [ASC, &NativeBlock, &Empty](FGameplayTag Source, bool bEnable)
	{
		ASC->ApplyAbilityBlockAndCancelTags(FGameplayTagContainer(Source), nullptr, bEnable, NativeBlock, false, Empty);
	};
	SetSource(CombatTags::Ability_Defend, true);
	SetSource(CombatTags::Ability_Area, true);
	TestTrue(TEXT("Relationship blocking uses the native GAS block container"), ASC->AreAbilityTagsBlocked(HealTags));
	TestTrue(TEXT("Incoming native block tags are preserved alongside relationship tags"), ASC->AreAbilityTagsBlocked(NativeBlock));
	TestFalse(TEXT("Two real block contributions reject the actual heal"), ASC->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()));
	SetSource(CombatTags::Ability_Defend, false);
	TestTrue(TEXT("Removing one source leaves the other source's heal block"), ASC->AreAbilityTagsBlocked(HealTags));
	TestTrue(TEXT("Original native block tags also retain their remaining contribution"), ASC->AreAbilityTagsBlocked(NativeBlock));
	TestFalse(TEXT("The actual heal remains blocked after just one removal"), ASC->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()));
	TestEqual(TEXT("Blocked attempts never consume energy"), Player->GetEnergy(), 100.f);
	TestEqual(TEXT("Blocked attempts never start a cooldown"), Player->GetHealCooldownRemaining(), 0.f);
	SetSource(CombatTags::Ability_Area, false);
	TestFalse(TEXT("The last removal releases the relationship block"), ASC->AreAbilityTagsBlocked(HealTags));
	TestFalse(TEXT("The last removal also releases the original native block"), ASC->AreAbilityTagsBlocked(NativeBlock));
	TestTrue(TEXT("The now-unblocked heal activates through real GAS"), ASC->TryActivateAbilityByClass(UmultiplayerHealAbility::StaticClass()));
	TestEqual(TEXT("The unblocked heal settles exactly once"), Player->GetHealth(), 40.f + Fixture.Config->Heal.Amount);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipHealReplacementTest, "Multiplayer.Combat.Relationships.HealReplacesAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 治疗替换未出手攻击后，旧任务不发弹，已提交攻击成本／冷却也不被退款或重置。
bool FCoopCombatRelationshipHealReplacementTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Replacement uses the actual player blueprint"), Player)) return false;
	auto* ASC = CombatASC(Player);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	Player->Attack();
	if (!TestTrue(TEXT("Actual montage-backed attack is still in its windup"), AttackActive(Player)
		&& Player->GetActionMontage(EMultiplayerCombatAnimation::Attack) && ASC->HasMatchingGameplayTag(CombatTags::State_Attacking))) return false;
	const float BeforeHeal = Player->GetEnergy();
	Player->Heal();
	TestFalse(TEXT("Heal cancels the real active attack spec"), AttackActive(Player));
	TestFalse(TEXT("The canceled attack releases its owned state"), ASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
	TestEqual(TEXT("Replacement heal settles its configured amount"), Player->GetHealth(), 40.f + Fixture.Config->Heal.Amount);
	TestEqual(TEXT("Replacement heal pays only its own additional cost"), Player->GetEnergy(), BeforeHeal - Fixture.Config->Heal.Cost);
	TestTrue(TEXT("Replacement retains the real heal cooldown"), Player->GetHealCooldownRemaining() > 0.f);
	Fixture.Advance(Fixture.Config->Attack.Windup + Fixture.Config->Attack.Recovery + .1f);
	TestEqual(TEXT("The canceled attack task never releases a delayed projectile"), Fixture.Projectiles(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipAreaReplacementTest, "Multiplayer.Combat.Relationships.AreaReplacesAttackAndKeepsGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// R 替换普攻时保留独立防御；显式右键则同时结束 R 与 Q，旧普攻不能迟到发弹。
bool FCoopCombatRelationshipAreaReplacementTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Area replacement uses the actual player blueprint"), Player)) return false;
	Player->Attack();
	if (!TestTrue(TEXT("The attack is genuinely in progress before Q"), AttackActive(Player))) return false;
	Player->Defend();
	TestTrue(TEXT("Q coexists with the existing attack"), Player->IsDefending() && AttackActive(Player));
	const float BeforeArea = Player->GetEnergy();
	Player->CastArea();
	if (!TestTrue(TEXT("Real ground targeting starts the actual area ability"),
		AbilityActive(Player, UmultiplayerAreaAbility::StaticClass()) && Player->IsCasting())) return false;
	TestFalse(TEXT("Area replaces the old attack spec"), AttackActive(Player));
	TestTrue(TEXT("Area replacement preserves the independent guard GE"), Player->IsDefending());
	TestEqual(TEXT("Area pays its configured cost once"), Player->GetEnergy(), BeforeArea - Fixture.Config->Area.Cost);
	Player->CancelCast();
	TestFalse(TEXT("Cancel ends the real area spec"), AbilityActive(Player, UmultiplayerAreaAbility::StaticClass()));
	TestFalse(TEXT("Right-click clears the area casting lock"), Player->IsCasting());
	TestFalse(TEXT("Explicit right-click also ends the lasting guard effect"), Player->IsDefending());
	TestTrue(TEXT("A committed cancellation preserves the area cooldown"), Player->GetAreaCooldownRemaining() > 0.f);
	TestEqual(TEXT("A committed cancellation does not refund energy"), Player->GetEnergy(), BeforeArea - Fixture.Config->Area.Cost);
	Fixture.Advance(Fixture.Config->Attack.Windup + Fixture.Config->Attack.Recovery + .1f);
	TestEqual(TEXT("Neither replacement nor cancellation revives the old attack task"), Fixture.Projectiles(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipRescueReplacementTest, "Multiplayer.Combat.Relationships.RescueReplacesAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 救援替换攻击并持有战斗阻断，取消或正常完成后都只释放自己的状态。
bool FCoopCombatRelationshipRescueReplacementTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	auto* Rescuer = Fixture.Player();
	auto* Target = Fixture.Player(FVector(150, 0, 100), true);
	if (!TestNotNull(TEXT("Controlled rescuer exists"), Rescuer) || !TestNotNull(TEXT("Controlled rescue target exists"), Target)) return false;
	auto* ASC = CombatASC(Rescuer);
	auto* TargetASC = Target->GetAbilitySystemComponent();
	auto Damage = TargetASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, TargetASC->MakeEffectContext());
	Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.f);
	TargetASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
	if (!TestTrue(TEXT("Real lethal damage creates the actual downed state"), Target->GetCoopComponent()->IsDowned())) return false;
	Rescuer->Attack();
	if (!TestTrue(TEXT("Rescue begins with a real delayed attack still active"), AttackActive(Rescuer)
		&& Rescuer->GetActionMontage(EMultiplayerCombatAnimation::Attack) && ASC->HasMatchingGameplayTag(CombatTags::State_Attacking))) return false;
	Rescuer->Rescue();
	if (!TestTrue(TEXT("Real rescue task reserves the downed teammate"), AbilityActive(Rescuer, UmultiplayerRescueAbility::StaticClass())
		&& Rescuer->GetCoopComponent()->IsRescuing() && ASC->HasMatchingGameplayTag(CombatTags::State_Rescuing))) return false;
	TestFalse(TEXT("Rescue replaces the in-flight attack"), AttackActive(Rescuer));
	TestFalse(TEXT("Rescue replacement releases the old attack state"), ASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
	const FGameplayTagContainer AttackTags(CombatTags::Ability_Attack), HealTags(CombatTags::Ability_Heal);
	TestTrue(TEXT("Active rescue blocks child attack through the native parent block"), ASC->AreAbilityTagsBlocked(AttackTags));
	TestTrue(TEXT("The same parent block covers healing"), ASC->AreAbilityTagsBlocked(HealTags));
	int32 CleanupNotifications = 0, NestedCancelAttempts = 0;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> ReentrantProbe(NewObject<UCoopCombatRelationshipTestProbe>());
	ReentrantProbe->StateCallback = [&]
	{
		// 只观察 Super 收尾前的真实清理通知；owned tag 移除后的正常状态通知不计作重复清理。
		if (!Rescuer->GetCoopComponent()->IsRescuing() && AbilityActive(Rescuer, UmultiplayerRescueAbility::StaticClass())
			&& ASC->HasMatchingGameplayTag(CombatTags::State_Rescuing))
		{
			++CleanupNotifications;
			if (NestedCancelAttempts == 0)
			{
				++NestedCancelAttempts;
				Rescuer->CancelRescue();
			}
		}
	};
	{
		Rescuer->OnCombatStateChanged.AddDynamic(ReentrantProbe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
		ON_SCOPE_EXIT
		{
			Rescuer->OnCombatStateChanged.RemoveDynamic(ReentrantProbe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
			ReentrantProbe->StateCallback = nullptr;
		};
		Rescuer->CancelRescue();
	}
	TestEqual(TEXT("Actual cleanup notification attempts one synchronous re-cancel"), NestedCancelAttempts, 1);
	TestEqual(TEXT("Synchronous re-cancel cannot repeat reservation cleanup"), CleanupNotifications, 1);
	TestFalse(TEXT("Canceled rescue releases its actual task and reservation"),
		AbilityActive(Rescuer, UmultiplayerRescueAbility::StaticClass()) || Rescuer->GetCoopComponent()->IsRescuing());
	TestFalse(TEXT("Canceled rescue removes its owned state"), ASC->HasMatchingGameplayTag(CombatTags::State_Rescuing));
	TestFalse(TEXT("Canceled rescue releases the native combat block"), ASC->AreAbilityTagsBlocked(AttackTags));
	Fixture.Advance(Fixture.Config->Attack.Windup + Fixture.Config->Attack.Recovery + .1f);
	TestEqual(TEXT("Canceled attack cannot fire during or after rescue"), Fixture.Projectiles(), 0);
	TestEqual(TEXT("Canceled rescue cannot produce a delayed heal"), Target->GetHealth(), 1.f);
	Rescuer->Rescue();
	if (!TestTrue(TEXT("The released reservation permits a new real rescue"), AbilityActive(Rescuer, UmultiplayerRescueAbility::StaticClass()))) return false;
	Fixture.Advance(Fixture.Config->RescueDuration + .1f);
	TestTrue(TEXT("The new rescue completes normally"), Target->IsCombatReady() && !Target->GetCoopComponent()->IsDowned());
	TestEqual(TEXT("Only the completed rescue restores thirty percent"), Target->GetHealth(), Target->GetMaxHealth() * .3f);
	TestFalse(TEXT("Completed rescue releases its task, target and owned state"),
		AbilityActive(Rescuer, UmultiplayerRescueAbility::StaticClass()) || Rescuer->GetCoopComponent()->IsRescuing()
		|| ASC->HasMatchingGameplayTag(CombatTags::State_Rescuing));
	TestFalse(TEXT("Normal completion also releases the combat block"), ASC->AreAbilityTagsBlocked(HealTags));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatRelationshipMissingSpecFeedbackTest, "Multiplayer.Combat.Relationships.MissingSpecFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 已学习却缺失真实 Spec 时必须明确拒绝和提示，不扣费、不施效，也不谎称技能可用。
bool FCoopCombatRelationshipMissingSpecFeedbackTest::RunTest(const FString& Parameters)
{
	FCombatRelationshipWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Missing-spec feedback uses the actual player blueprint"), Player)) return false;
	auto* ASC = CombatASC(Player);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	const auto* Heal = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
	if (!TestNotNull(TEXT("Heal is really granted before its spec is cleared"), Heal)) return false;
	const int32 LearnedLevel = Player->GetProgressionComponent()->GetSkillLevel(EMultiplayerCombatSkill::Heal);
	if (!TestTrue(TEXT("The missing spec is independent of an already learned skill"), LearnedLevel > 0)) return false;
	const FGameplayAbilitySpecHandle HealHandle = Heal->Handle;
	ASC->ClearAbility(HealHandle);
	TestNull(TEXT("The actual heal spec was removed"), ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass()));
	TestEqual(TEXT("Clearing the GAS spec does not rewrite progression"),
		Player->GetProgressionComponent()->GetSkillLevel(EMultiplayerCombatSkill::Heal), LearnedLevel);
	const float EnergyBefore = Player->GetEnergy(), HealthBefore = Player->GetHealth(), CooldownBefore = Player->GetHealCooldownRemaining();
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> MessageProbe(NewObject<UCoopCombatRelationshipTestProbe>());
	bool bStarted = false;
	{
		Player->OnCombatMessage.AddDynamic(MessageProbe.Get(), &UCoopCombatRelationshipTestProbe::HandleMessage);
		ON_SCOPE_EXIT
		{
			Player->OnCombatMessage.RemoveDynamic(MessageProbe.Get(), &UCoopCombatRelationshipTestProbe::HandleMessage);
		};
		bStarted = Player->ActivateSkill(EMultiplayerCombatSkill::Heal);
	}
	TestFalse(TEXT("The public skill entry rejects a missing spec before GAS activation"), bStarted);
	TestEqual(TEXT("Missing-spec rejection publishes exactly one real HUD message"), MessageProbe->Messages.Num(), 1);
	if (MessageProbe->Messages.Num() == 1)
		TestTrue(TEXT("The message explains that the skill is not ready"), MessageProbe->Messages[0].ToString().Contains(TEXT("尚未就绪")));
	TestEqual(TEXT("Missing-spec rejection never spends energy"), Player->GetEnergy(), EnergyBefore);
	TestEqual(TEXT("Missing-spec rejection never heals"), Player->GetHealth(), HealthBefore);
	TestEqual(TEXT("Missing-spec rejection never starts a cooldown"), Player->GetHealCooldownRemaining(), CooldownBefore);
	return !HasAnyErrors();
}

#endif
