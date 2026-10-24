// Copyright Epic Games, Inc. All Rights Reserved.

// 回归技能拒绝原因、真实冷却提示、演示豁免和治疗不播放蒙太奇的约定。
// 使用正式角色与 GAS 状态，不把单世界提示测试说成真实网络拒绝验证。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "UObject/UnrealType.h"

namespace
{
	// 轻量真实世界用于读写 ASC 和加载正式角色，避免界面测试引入无关 AI 行为。
	struct FSkillFeedbackWorld
	{
		UWorld* World;
		// 初始化真实角色可运行的独立世界，不创建导航或自主 AI。
		FSkillFeedbackWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
		}
		// 按正式 EndPlay 结束角色与效果，并清除世界上下文。
		~FSkillFeedbackWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		// 按指定正式类生成角色，支持原生规则与保存动画配置分别作为对照。
		AmultiplayerCombatCharacter* Character(UClass* Class = AmultiplayerCombatCharacter::StaticClass())
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			return World->SpawnActor<AmultiplayerCombatCharacter>(Class, FVector(0, 0, 100), FRotator::ZeroRotator, Params);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSkillFailureReasonTest, "Multiplayer.Combat.SkillFailureReasons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实角色/GAS逐项拒绝，区分未学习、无治疗目标、资源与连接；拒绝查询不能扣费或开冷却。
bool FCoopCombatSkillFailureReasonTest::RunTest(const FString& Parameters)
{
	FSkillFeedbackWorld Fixture;
	auto* Character = Fixture.Character();
	if (!TestNotNull(TEXT("Character initialized its real abilities"), Character)) return false;
	auto* ASC = Character->GetAbilitySystemComponent();
	const auto ExpectReason = [this, Character](EMultiplayerCombatSkill Skill, FGameplayTag Expected)
	{
		FGameplayTagContainer Reasons;
		TestFalse(TEXT("Unavailable skill is rejected"), Character->CheckSkillAvailable(Skill, &Reasons));
		TestTrue(*FString::Printf(TEXT("Exact reason %s is returned"), *Expected.ToString()), Reasons.HasTagExact(Expected));
		TestFalse(TEXT("Player-facing reason is nonempty"), Character->GetSkillFailureReason(Skill).IsEmpty());
	};
	TestTrue(TEXT("Attack is initially available"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
	ExpectReason(EMultiplayerCombatSkill::Dash, CombatTags::Failure_NotLearned);
	ExpectReason(EMultiplayerCombatSkill::Heal, CombatTags::Failure_NoHealTarget);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 60.f);
	TestTrue(TEXT("A hurt caster is a valid heal target"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 10.f);
	ExpectReason(EMultiplayerCombatSkill::Heal, CombatTags::Failure_InsufficientEnergy);
	TestTrue(TEXT("Cost hint states the resource requirement"), Character->GetSkillFailureReason(EMultiplayerCombatSkill::Heal).ToString().Contains(TEXT("25")));
	TestTrue(TEXT("Server refusal uses authoritative resource rather than a stale predicted balance"),
		Character->GetSkillFailureText(EMultiplayerCombatSkill::Heal, CombatTags::Failure_InsufficientEnergy, 7.f).ToString().Contains(TEXT("当前 7")));
	Character->SetCombatSessionReady(false);
	ExpectReason(EMultiplayerCombatSkill::Attack, CombatTags::Failure_NotReady);
	Character->SetCombatSessionReady(true);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
	for (const auto& State : TArray<TPair<FGameplayTag, FGameplayTag>>{
		{CombatTags::State_Downed, CombatTags::Failure_Downed},
		{CombatTags::State_Rescuing, CombatTags::Failure_Rescuing},
		{CombatTags::State_Stunned, CombatTags::Failure_Stunned},
		{CombatTags::State_Casting, CombatTags::Failure_Casting},
		{CombatTags::State_Dead, CombatTags::Failure_Dead}})
	{
		ASC->AddLooseGameplayTag(State.Key);
		ExpectReason(EMultiplayerCombatSkill::Attack, State.Value);
		ASC->RemoveLooseGameplayTag(State.Key);
	}
	TestEqual(TEXT("Qualification queries never charge resources"), Character->GetEnergy(), 100.f);
	TestEqual(TEXT("Qualification queries never start cooldown"), Character->GetAttackCooldownRemaining(), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSkillCooldownFeedbackTest, "Multiplayer.Combat.SkillCooldownFeedback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 技能提示读取真实 GE 剩余秒数；查询不会因普通攻击动作而错误禁止治疗/防御。
bool FCoopCombatSkillCooldownFeedbackTest::RunTest(const FString& Parameters)
{
	FSkillFeedbackWorld Fixture;
	auto* Character = Fixture.Character();
	if (!TestNotNull(TEXT("Character exists"), Character)) return false;
	auto* ASC = Character->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 60.f);
	ASC->AddLooseGameplayTag(CombatTags::State_Attacking);
	TestTrue(TEXT("Attack phase does not block healing"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	TestTrue(TEXT("Attack phase does not block defense"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Defend));
	ASC->RemoveLooseGameplayTag(CombatTags::State_Attacking);
	auto Spec = ASC->MakeOutgoingSpec(UmultiplayerHealCooldownEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	Spec.Data->SetDuration(2.5f, true);
	const auto Handle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	FGameplayTagContainer Reasons;
	TestFalse(TEXT("Cooldown blocks heal"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Heal, &Reasons));
	TestTrue(TEXT("Cooldown has its own reason"), Reasons.HasTagExact(CombatTags::Failure_Cooldown));
	TestTrue(TEXT("Hint includes actual seconds"), Character->GetSkillFailureReason(EMultiplayerCombatSkill::Heal).ToString().Contains(TEXT("2.5")));
	TestTrue(TEXT("Server cooldown correction is not replaced by stale local predicted duration"),
		Character->GetSkillFailureText(EMultiplayerCombatSkill::Heal, CombatTags::Failure_Cooldown, 1.2f).ToString().Contains(TEXT("1.2")));
	ASC->RemoveActiveGameplayEffect(Handle);
	TestTrue(TEXT("Removing cooldown restores qualification"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatSkillDemoCheatTest, "Multiplayer.Combat.SkillDemoIgnoreCostAndCooldown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 只豁免资源和冷却；运行后恢复进程开关，不能污染其他用例或正式动作边界。
bool FCoopCombatSkillDemoCheatTest::RunTest(const FString& Parameters)
{
	auto* IgnoreCost = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCosts"));
	auto* IgnoreCooldown = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.IgnoreCooldowns"));
	if (!TestNotNull(TEXT("Native cost cheat exists"), IgnoreCost) || !TestNotNull(TEXT("Native cooldown cheat exists"), IgnoreCooldown)) return false;
	const int32 PreviousCost = IgnoreCost->GetInt(), PreviousCooldown = IgnoreCooldown->GetInt();
	IgnoreCost->Set(1, ECVF_SetByCode); IgnoreCooldown->Set(1, ECVF_SetByCode);
	{
		FSkillFeedbackWorld Fixture;
		auto* Character = Fixture.Character();
		auto* ASC = Character->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 0.f);
		auto CD = ASC->MakeOutgoingSpec(UmultiplayerDefendCooldownEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
		const auto ExistingCooldown = ASC->ApplyGameplayEffectSpecToSelf(*CD.Data.Get());
		TestTrue(TEXT("Native cheats also bypass project preflight for zero energy and an old cooldown"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Defend));
		Character->Defend();
		TestTrue(TEXT("Real defense activates through normal GAS commit"), Character->IsDefending());
		TestEqual(TEXT("Custom commit does not subtract energy"), Character->GetEnergy(), 0.f);
		int32 Cooldowns = 0;
		for (const auto Handle : ASC->GetActiveEffects(FGameplayEffectQuery()))
			if (ASC->GetActiveGameplayEffect(Handle)->Spec.Def->IsA<UmultiplayerDefendCooldownEffect>()) ++Cooldowns;
		TestEqual(TEXT("No second cooldown or zero-duration cooldown is created"), Cooldowns, 1);
		ASC->RemoveActiveGameplayEffect(ExistingCooldown);
		TestEqual(TEXT("Removing the old cooldown leaves no hidden demo cooldown"), Character->GetDefenseCooldownRemaining(), 0.f);
		TestFalse(TEXT("Still-active defense retains normal repeat-input restriction"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Defend));
		ASC->AddLooseGameplayTag(CombatTags::State_Stunned);
		TestFalse(TEXT("Resource cheats do not bypass crowd control"), Character->CheckSkillAvailable(EMultiplayerCombatSkill::Attack));
		ASC->RemoveLooseGameplayTag(CombatTags::State_Stunned);
	}
	IgnoreCost->Set(PreviousCost, ECVF_SetByCode); IgnoreCooldown->Set(PreviousCooldown, ECVF_SetByCode);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHealWithoutMontageTest, "Multiplayer.Combat.HealWithoutMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 实例上配置可播放的治疗动作作为故障对照，真实 E 仍只结算治疗、消耗与冷却。
bool FCoopCombatHealWithoutMontageTest::RunTest(const FString& Parameters)
{
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Formal animated player class loads"), PlayerClass)) return false;
	FSkillFeedbackWorld Fixture;
	auto* Player = Fixture.Character(PlayerClass);
	if (!TestNotNull(TEXT("Formal animated player spawned"), Player)) return false;
	UAnimInstance* Animation = Player->GetMesh()->GetAnimInstance();
	UAnimMontage* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	const auto* HealMontageProperty = FindFProperty<FObjectPropertyBase>(Player->GetClass(), TEXT("HealMontage"));
	if (!TestNotNull(TEXT("Real animation instance is initialized"), Animation)
		|| !TestNotNull(TEXT("Compatible saved montage loads"), Montage)
		|| !TestNotNull(TEXT("Test instance can carry a healing montage"), HealMontageProperty)) return false;
	HealMontageProperty->SetObjectPropertyValue_InContainer(Player, Montage);
	if (!TestTrue(TEXT("Configured healing montage can actually play"), Animation->Montage_Play(Montage) > 0.f)) return false;
	Animation->StopAllMontages(0.f);
	TestFalse(TEXT("Baseline has no playing montage"), Animation->Montage_IsPlaying(nullptr));
	const int32 BaselineMontageInstances = Animation->MontageInstances.Num();
	auto* ASC = Player->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
	const FmultiplayerCombatSkillRule Rule = Player->GetCombatConfig().Heal;
	TestTrue(TEXT("Injured caster passes real heal qualification"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	Player->Heal();
	TestFalse(TEXT("Real heal starts no animation montage even when one is configured"), Animation->Montage_IsPlaying(nullptr));
	TestEqual(TEXT("Real heal creates no montage instance"), Animation->MontageInstances.Num(), BaselineMontageInstances);
	TestEqual(TEXT("Real heal settles its configured amount once"), Player->GetHealth(), FMath::Min(40.f + Rule.Amount, Player->GetMaxHealth()));
	TestEqual(TEXT("Real heal charges its configured energy once"), Player->GetEnergy(), 100.f - Rule.Cost);
	TestTrue(TEXT("Real heal starts its configured cooldown"), FMath::IsNearlyEqual(Player->GetHealCooldownRemaining(), Rule.Cooldown, .01f));
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerHealAbility::StaticClass());
	TestTrue(TEXT("Instant heal finishes without an animation task"), Spec && !Spec->IsActive());
	return !HasAnyErrors();
}

#endif
