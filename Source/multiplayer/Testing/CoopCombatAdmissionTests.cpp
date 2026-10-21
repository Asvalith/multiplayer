// Copyright Epic Games, Inc. All Rights Reserved.

// 回归无副作用预检、真实 GAS 准入与提交时复核的一致性。
// 使用正式授予的 Spec／能力实例，不另写一份测试资格规则替代被测链路。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"

namespace
{
	// 使用真实角色、ASC 和已授予 Spec；不以另一份资格判断替代被测的 GAS 准入。
	struct FCombatAdmissionWorld
	{
		UWorld* World;

		// 建立仅支持角色与 GAS 的独立世界，排除导航和 AI 对准入结果的干扰。
		FCombatAdmissionWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
		}

		// 结束角色和世界上下文，避免委托或活动效果泄漏到下一用例。
		~FCombatAdmissionWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		// 创建有真实控制器的玩家并启用飞行，确保移动资格不因无地面夹具而被拒绝。
		AmultiplayerCombatCharacter* Player()
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Character = World->SpawnActor<AmultiplayerCombatCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator, Params);
			if (Character)
			{
				World->SpawnActor<APlayerController>()->Possess(Character);
				Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			}
			return Character;
		}
	};

	// 优先查询已授予的主实例，与运行时准入入口一致；无实例时才读取能力默认对象。
	UGameplayAbility* GrantedAbility(const FGameplayAbilitySpec& Spec)
	{
		UGameplayAbility* Instance = Spec.GetPrimaryInstance();
		return Instance ? Instance : Spec.Ability.Get();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionDerivedGateTest,
	"Multiplayer.Combat.Admission.PreviewUsesDerivedAbilityRequirements",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 小兵没有黑板目标时，公共角色资格可以满足，但真实近战能力仍必须拒绝；HUD 不能另行放行。
bool FCoopCombatAdmissionDerivedGateTest::RunTest(const FString& Parameters)
{
	FCombatAdmissionWorld Fixture;
	const FTransform Transform(FRotator::ZeroRotator, FVector(0, 0, 100));
	auto* Enemy = Fixture.World->SpawnActorDeferred<AmultiplayerCombatEnemy>(AmultiplayerCombatEnemy::StaticClass(),
		Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Real enemy spawned deferred"), Enemy)) return false;
	Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
	const auto* AttackProperty = FindFProperty<FClassProperty>(Enemy->GetClass(), TEXT("AttackAbilityClass"));
	if (!TestNotNull(TEXT("Enemy attack class can be configured before granting"), AttackProperty)) return false;
	AttackProperty->SetObjectPropertyValue_InContainer(Enemy, UmultiplayerEnemyMeleeAbility::StaticClass());
	Enemy->FinishSpawning(Transform);
	auto* ASC = Enemy->GetAbilitySystemComponent();
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerEnemyMeleeAbility::StaticClass());
	if (!TestTrue(TEXT("Enemy owns the real melee ability"), Spec && Spec->Ability)) return false;
	TestTrue(TEXT("Common session and life requirements alone admit this enemy"),
		Enemy->CheckSkillBusinessRequirements(EMultiplayerCombatSkill::Attack));
	TestFalse(TEXT("Derived melee CanActivate rejects its missing target"),
		GrantedAbility(*Spec)->CanActivateAbility(Spec->Handle, ASC->AbilityActorInfo.Get()));
	TestFalse(TEXT("Player-facing availability queries that same derived condition"),
		Enemy->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
	TestFalse(TEXT("Real GAS activation agrees with the preview rejection"), ASC->TryActivateAbility(Spec->Handle));
	TestEqual(TEXT("Rejected target cannot spend energy"), Enemy->GetEnergy(), Enemy->GetMaxEnergy());
	TestEqual(TEXT("Rejected target cannot create an attack cooldown"), Enemy->GetAttackCooldownRemaining(), 0.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionNativeInputGateTest,
	"Multiplayer.Combat.Admission.PreviewUsesNativeActivationInhibition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 原生输入禁用并非项目状态标签；查询真实 CanActivate 才能覆盖菜单等外部 GAS 限制。
bool FCoopCombatAdmissionNativeInputGateTest::RunTest(const FString& Parameters)
{
	FCombatAdmissionWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Controlled player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	if (!TestTrue(TEXT("Actual player and attack are locally initialized"),
		Spec && Spec->Ability && ASC->AbilityActorInfo->IsLocallyControlled())) return false;
	TestTrue(TEXT("Attack is initially available"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
	ASC->SetUserAbilityActivationInhibited(true);
	ON_SCOPE_EXIT { ASC->SetUserAbilityActivationInhibited(false); };
	TestTrue(TEXT("Common business eligibility does not invent a second input-inhibition rule"),
		Player->CheckSkillBusinessRequirements(EMultiplayerCombatSkill::Attack));
	TestFalse(TEXT("Native GAS CanActivate rejects inhibited input"),
		GrantedAbility(*Spec)->CanActivateAbility(Spec->Handle, ASC->AbilityActorInfo.Get()));
	TestFalse(TEXT("HUD availability observes the native rejection"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
	TestFalse(TEXT("A native rejection without a project tag still produces a useful failure hint"),
		Player->GetSkillFailureReason(EMultiplayerCombatSkill::Attack).IsEmpty());
	TestFalse(TEXT("Real activation is likewise rejected"), ASC->TryActivateAbility(Spec->Handle));
	TestFalse(TEXT("Rejection never activates the granted spec"), Spec->IsActive());
	TestEqual(TEXT("Rejection leaves resources unchanged"), Player->GetEnergy(), Player->GetMaxEnergy());
	TestEqual(TEXT("Rejection leaves no cooldown"), Player->GetAttackCooldownRemaining(), 0.f);
	ASC->SetUserAbilityActivationInhibited(false);
	TestTrue(TEXT("Removing only the native inhibition immediately restores availability"),
		Player->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionReadOnlyTest,
	"Multiplayer.Combat.Admission.RepeatedPreviewIsReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 高频界面查询不授予能力、不创建效果、不广播属性变化，也不激活任何技能实例。
bool FCoopCombatAdmissionReadOnlyTest::RunTest(const FString& Parameters)
{
	FCombatAdmissionWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 60.f);
	const int32 EffectCount = ASC->GetActiveEffects(FGameplayEffectQuery()).Num();
	const int32 AbilityCount = ASC->GetActivatableAbilities().Num();
	const float EnergyBefore = Player->GetEnergy(), HealthBefore = Player->GetHealth();
	int32 ResourceNotifications = 0;
	const FDelegateHandle EnergyObserver = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute())
		.AddLambda([&ResourceNotifications](const FOnAttributeChangeData&) { ++ResourceNotifications; });
	const FDelegateHandle HealthObserver = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute())
		.AddLambda([&ResourceNotifications](const FOnAttributeChangeData&) { ++ResourceNotifications; });
	ON_SCOPE_EXIT
	{
		ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(EnergyObserver);
		ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(HealthObserver);
	};
	for (int32 Repeat = 0; Repeat < 32; ++Repeat)
	{
		for (int32 Index = 0; Index < 5; ++Index)
		{
			const auto Skill = EMultiplayerCombatSkill(Index);
			const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetSkillAbilityClass(Skill));
			const bool bNativeAvailable = Spec && Spec->Ability
				&& GrantedAbility(*Spec)->CanActivateAbility(Spec->Handle, ASC->AbilityActorInfo.Get());
			TestEqual(TEXT("Preview and actual granted ability return the same eligibility"), Player->CheckSkillAvailable(Skill), bNativeAvailable);
			Player->GetSkillFailureReason(Skill);
		}
	}
	TestEqual(TEXT("Queries do not grant additional ability specs"), ASC->GetActivatableAbilities().Num(), AbilityCount);
	TestEqual(TEXT("Queries create no gameplay effects"), ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), EffectCount);
	TestEqual(TEXT("Queries never broadcast resource modifications"), ResourceNotifications, 0);
	TestEqual(TEXT("Queries preserve actual energy"), Player->GetEnergy(), EnergyBefore);
	TestEqual(TEXT("Queries preserve actual health"), Player->GetHealth(), HealthBefore);
	for (const auto& Spec : ASC->GetActivatableAbilities())
		TestFalse(TEXT("No queried ability became active"), Spec.IsActive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionCommitRecheckTest,
	"Multiplayer.Combat.Admission.CommitRechecksCurrentResourcesAndCooldown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 查询成功不是消费凭证：真正提交前仍由原生 CommitCheck 复核余额/CD，而非重跑全部激活资格。
bool FCoopCombatAdmissionCommitRecheckTest::RunTest(const FString& Parameters)
{
	auto* IgnoreCost = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCosts"));
	auto* IgnoreCooldown = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCooldowns"));
	if (!TestNotNull(TEXT("Native cost switch exists"), IgnoreCost)
		|| !TestNotNull(TEXT("Native cooldown switch exists"), IgnoreCooldown)) return false;
	const int32 PreviousCost = IgnoreCost->GetInt(), PreviousCooldown = IgnoreCooldown->GetInt();
	ON_SCOPE_EXIT
	{
		IgnoreCost->Set(PreviousCost, ECVF_SetByCode);
		IgnoreCooldown->Set(PreviousCooldown, ECVF_SetByCode);
	};
	IgnoreCost->Set(0, ECVF_SetByCode);
	IgnoreCooldown->Set(0, ECVF_SetByCode);
	FCombatAdmissionWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 60.f);
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
	if (!TestTrue(TEXT("Real heal spec exists"), Spec && Spec->Ability)) return false;
	UGameplayAbility* Ability = GrantedAbility(*Spec);
	const FGameplayAbilityActivationInfo ActivationInfo = Ability->GetCurrentActivationInfo();
	TestTrue(TEXT("Initial preview admits healing"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 0.f);
	FGameplayTagContainer Reasons;
	TestFalse(TEXT("CommitCheck rejects energy spent after the earlier preview"),
		Ability->CommitCheck(Spec->Handle, ASC->AbilityActorInfo.Get(), ActivationInfo, &Reasons));
	TestTrue(TEXT("The actual commit failure identifies insufficient energy"), Reasons.HasTagExact(CombatTags::Failure_InsufficientEnergy));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
	auto Cooldown = ASC->MakeOutgoingSpec(UmultiplayerHealCooldownEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	Cooldown.Data->SetDuration(2.f, true);
	const FActiveGameplayEffectHandle CooldownHandle = ASC->ApplyGameplayEffectSpecToSelf(*Cooldown.Data.Get());
	Reasons.Reset();
	TestFalse(TEXT("CommitCheck rejects a cooldown acquired after the earlier preview"),
		Ability->CommitCheck(Spec->Handle, ASC->AbilityActorInfo.Get(), ActivationInfo, &Reasons));
	TestTrue(TEXT("The actual commit failure identifies cooldown"), Reasons.HasTagExact(CombatTags::Failure_Cooldown));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 0.f);
	IgnoreCost->Set(1, ECVF_SetByCode);
	IgnoreCooldown->Set(1, ECVF_SetByCode);
	TestTrue(TEXT("Unlimited mode is honored by both preview and native commit recheck"),
		Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal)
		&& Ability->CommitCheck(Spec->Handle, ASC->AbilityActorInfo.Get(), ActivationInfo));
	ASC->SetUserAbilityActivationInhibited(true);
	TestFalse(TEXT("Unlimited costs/cooldowns do not bypass native activation inhibition"),
		Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	TestTrue(TEXT("CommitCheck deliberately does not re-run user-input eligibility after activation"),
		Ability->CommitCheck(Spec->Handle, ASC->AbilityActorInfo.Get(), ActivationInfo));
	ASC->SetUserAbilityActivationInhibited(false);
	TestEqual(TEXT("All commit checks are read-only, even under unlimited switches"), Player->GetEnergy(), 0.f);
	TestEqual(TEXT("Checking heal never changes the target's health"), Player->GetHealth(), 60.f);
	TestEqual(TEXT("Checks never create a second cooldown"), ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
	ASC->RemoveActiveGameplayEffect(CooldownHandle);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAdmissionCommitSnapshotTest,
	"Multiplayer.Combat.Admission.CommitKeepsRuleSnapshotAcrossCostNotification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 扣费事件允许同步进入外部逻辑；本次成本/CD 使用提交前规则，后续查询才读取改变后的等级。
bool FCoopCombatAdmissionCommitSnapshotTest::RunTest(const FString& Parameters)
{
	auto* IgnoreCost = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCosts"));
	auto* IgnoreCooldown = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCooldowns"));
	if (!TestNotNull(TEXT("Native cost switch exists"), IgnoreCost)
		|| !TestNotNull(TEXT("Native cooldown switch exists"), IgnoreCooldown)) return false;
	const int32 PreviousCost = IgnoreCost->GetInt(), PreviousCooldown = IgnoreCooldown->GetInt();
	ON_SCOPE_EXIT
	{
		IgnoreCost->Set(PreviousCost, ECVF_SetByCode);
		IgnoreCooldown->Set(PreviousCooldown, ECVF_SetByCode);
	};
	IgnoreCost->Set(0, ECVF_SetByCode);
	IgnoreCooldown->Set(0, ECVF_SetByCode);
	FCombatAdmissionWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
	if (!TestTrue(TEXT("The real initial heal spec is level one"), Spec && Spec->Ability && Spec->Level == 1)) return false;
	const FmultiplayerCombatSkillRule BeforeCommit = Player->GetSkillRule(EMultiplayerCombatSkill::Heal);
	const float EnergyBefore = Player->GetEnergy();
	int32 UpgradesDuringCostNotification = 0;
	const FDelegateHandle CostObserver = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute())
		.AddLambda([ASC, &UpgradesDuringCostNotification](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue >= Change.OldValue || UpgradesDuringCostNotification != 0) return;
			if (auto* HealSpec = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass()))
			{
				++UpgradesDuringCostNotification;
				// 故意从真实通知注入重入变化，不通过被活动能力禁止的正常配装入口伪造成功。
				HealSpec->Level = 3;
				ASC->MarkAbilitySpecDirty(*HealSpec);
			}
		});
	ON_SCOPE_EXIT
	{
		ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(CostObserver);
	};
	Player->Heal();
	TestEqual(TEXT("The real cost notification changes the granted spec exactly once"), UpgradesDuringCostNotification, 1);
	TestEqual(TEXT("This commit charges its original effective cost once"), Player->GetEnergy(), EnergyBefore - BeforeCommit.Cost);
	const FmultiplayerCombatSkillRule AfterCommit = Player->GetSkillRule(EMultiplayerCombatSkill::Heal);
	TestTrue(TEXT("Subsequent effective-rule queries see the upgraded cooldown"), AfterCommit.Cooldown < BeforeCommit.Cooldown);
	const auto Cooldowns = ASC->GetActiveEffects(FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::Cooldown_Heal)));
	if (!TestEqual(TEXT("Real commit creates exactly one heal cooldown GE"), Cooldowns.Num(), 1)) return false;
	const FActiveGameplayEffect* ActiveCooldown = ASC->GetActiveGameplayEffect(Cooldowns[0]);
	if (!TestNotNull(TEXT("Committed cooldown is owned by GAS"), ActiveCooldown)) return false;
	TestTrue(TEXT("The existing cooldown retains the pre-notification commit snapshot"),
		FMath::IsNearlyEqual(ActiveCooldown->Spec.GetDuration(), BeforeCommit.Cooldown));
	TestFalse(TEXT("The current commit does not silently adopt the newly queried cooldown"),
		FMath::IsNearlyEqual(ActiveCooldown->Spec.GetDuration(), AfterCommit.Cooldown));
	return !HasAnyErrors();
}

#endif
