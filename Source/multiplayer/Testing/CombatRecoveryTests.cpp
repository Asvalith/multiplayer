// Copyright Epic Games, Inc. All Rights Reserved.

// 回归同进程断线恢复的持续效果、离线结算及周期相位，不代替真实网络握手测试。
// 夹具使用真实 ASC／GE／Timer，恢复资格和断线期间结果由正式业务入口处理。

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatTags.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "TimerManager.h"

namespace
{
	// 真实 ASC/GE/Timer 的重连夹具，不创建导航、AI 决策或网络假回执。
	struct FCombatRecoveryWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		// 创建可推进 GAS 与计时器的独立世界，关闭与恢复规则无关的导航和 AI。
		FCombatRecoveryWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL()); World->SetBegunPlay(true);
		}
		// 对称结束测试世界并还原全局帧号，避免周期用例相互影响。
		~FCombatRecoveryWorld()
		{
			World->EndPlay(EEndPlayReason::Quit); GEngine->DestroyWorldContext(World); World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		// 小步推进正式周期计时器，避免测试时钟一次跨越多跳掩盖实际周期行为。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.01f, Seconds - Elapsed);
				++GFrameCounter; World->Tick(LEVELTICK_All, Step); Elapsed += Step;
			}
		}
		// 生成禁用决策和移动的真实角色，让恢复用例只观察状态与效果。
		AmultiplayerCombatCharacter* Character(bool bEnemy = false)
		{
			const FTransform Transform(FVector(bEnemy ? -1000 : 1000, 0, 100));
			auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(bEnemy ? AmultiplayerCombatEnemy::StaticClass()
				: AmultiplayerCombatCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled; Pawn->FinishSpawning(Transform);
			Pawn->GetCharacterMovement()->DisableMovement(); return Pawn;
		}
		// 先关闭接纳门再经正式入口恢复，不让重建过程临时开放战斗资格。
		AmultiplayerCombatCharacter* Restore(const FmultiplayerCombatResumeState& Snapshot, float Offline)
		{
			auto* Pawn = Character(); Pawn->SetCombatSessionReady(false); Pawn->RestoreResumeState(Snapshot, Offline); return Pawn;
		}
	};
	// 同一测试也通过正式 GE 创建不同来源的独立定身与限时防御。
	void AddTimedEffect(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target,
		TSubclassOf<UGameplayEffect> Class, float Duration)
	{
		auto* ASC = Source->GetAbilitySystemComponent();
		auto Spec = ASC->MakeOutgoingSpec(Class, 1.f, ASC->MakeEffectContext()); Spec.Data->SetDuration(Duration, true);
		ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
	}
	// 按真实效果定义统计活动句柄，检查独立来源没有被错误合并或重复恢复。
	int32 CountRecoveryEffects(AmultiplayerCombatCharacter* Pawn, TSubclassOf<UGameplayEffect> Class)
	{
		FGameplayEffectQuery Query; Query.EffectDefinition = Class;
		return Pawn->GetAbilitySystemComponent()->GetActiveEffects(Query).Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRecoveryPhaseTest, "Multiplayer.Combat.Recovery.PhaseAndIndependentRoots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 保存原来源的同源聚合/异源分离、每层定身和下一跳相位，恢复期间不提前接纳玩家。
bool FCombatRecoveryPhaseTest::RunTest(const FString&)
{
	FCombatRecoveryWorld Fixture;
	auto* First = Fixture.Character(true); auto* Second = Fixture.Character(true); auto* Old = Fixture.Character();
	First->ApplyBurning(Old, 10.f, 4.f); First->ApplyBurning(Old, 10.f, 4.f); Second->ApplyBurning(Old, 20.f, 4.f);
	AddTimedEffect(First, Old, UmultiplayerRootEffect::StaticClass(), 2.f);
	AddTimedEffect(Second, Old, UmultiplayerRootEffect::StaticClass(), 3.f);
	Fixture.Advance(.25f);
	const auto Snapshot = Old->CaptureResumeState(); Old->Destroy();
	TestEqual(TEXT("Snapshot retains two sources and two independent root effects"), Snapshot.PersistentEffects.Num(), 4);
	auto* Pawn = Fixture.Restore(Snapshot, .4f);
	TestFalse(TEXT("Restore never opens the combat-session admission gate"), Pawn->IsCombatReady());
	TestEqual(TEXT("Living sources retain two non-merged real burn effects"), CountRecoveryEffects(Pawn, UmultiplayerBurningEffect::StaticClass()), 2);
	TestEqual(TEXT("Each remaining root owns its own active effect"), CountRecoveryEffects(Pawn, UmultiplayerRootEffect::StaticClass()), 2);
	TestEqual(TEXT("Recovery does not apply another first hit"), Pawn->GetHealth(), 100.f);
	Pawn->SetCombatSessionReady(true);
	Fixture.Advance(.30f); TestEqual(TEXT("No tick arrives before the preserved phase"), Pawn->GetHealth(), 100.f);
	Fixture.Advance(.10f); TestEqual(TEXT("Both original sources tick after roughly .35 rather than a fresh second"), Pawn->GetHealth(), 70.f);
	Fixture.Advance(1.f);
	TestEqual(TEXT("First root expires without deleting the second"), CountRecoveryEffects(Pawn, UmultiplayerRootEffect::StaticClass()), 1);
	TestTrue(TEXT("The remaining root still restricts movement"), Pawn->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Rooted));
	Fixture.Advance(1.10f);
	TestFalse(TEXT("The final independent root expires normally"), Pawn->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Rooted));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRecoveryExpiredSourceTest, "Multiplayer.Combat.Recovery.ExpiredSourcesRemainIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 原施加者已销毁时，不把两个空来源合并，也不伪装成重连角色自己施加的灼烧。
bool FCombatRecoveryExpiredSourceTest::RunTest(const FString&)
{
	FCombatRecoveryWorld Fixture;
	auto* First = Fixture.Character(true); auto* Second = Fixture.Character(true); auto* Old = Fixture.Character();
	First->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetAttackPowerAttribute(), 7.f);
	Second->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetAttackPowerAttribute(), 3.f);
	First->ApplyBurning(Old, 10.f, 4.f); Second->ApplyBurning(Old, 20.f, 4.f); Fixture.Advance(.25f);
	const auto Snapshot = Old->CaptureResumeState(); Old->Destroy();
	First->GetAbilitySystemComponent()->DestroyComponent(); Second->GetAbilitySystemComponent()->DestroyComponent();
	First->Destroy(); Second->Destroy();
	auto* Pawn = Fixture.Restore(Snapshot, .4f);
	TestEqual(TEXT("Two expired sources retain independent effects with their original definition"), CountRecoveryEffects(Pawn, UmultiplayerBurningEffect::StaticClass()), 2);
	for (const auto& Effect : Pawn->CaptureResumeState().PersistentEffects)
		TestNull(TEXT("Recovery preserves the expired source instead of assigning self"), Effect.Spec.Data->GetContext().GetInstigatorAbilitySystemComponent());
	Pawn->SetCombatSessionReady(true); Fixture.Advance(.4f);
	TestEqual(TEXT("Expired sources retain their captured attack bonuses: 10+7 and 20+3"), Pawn->GetHealth(), 60.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRecoveryOfflineSettlementTest, "Multiplayer.Combat.Recovery.OfflineDamageExpiryAndDeath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 主业务用非整数寿命隔离到期竞态；整数到期另与真实连续运行对照，不假定几秒必定几跳。
bool FCombatRecoveryOfflineSettlementTest::RunTest(const FString&)
{
	FCombatRecoveryWorld Fixture;
	auto* Source = Fixture.Character(true); auto* Old = Fixture.Character();
	Old->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute(), 15.f);
	AddTimedEffect(Old, Old, UmultiplayerDefendingEffect::StaticClass(), 1.5f);
	Source->ApplyBurning(Old, 20.f, 3.2f); AddTimedEffect(Source, Old, UmultiplayerRootEffect::StaticClass(), 1.f);
	Fixture.Advance(.25f); auto Snapshot = Old->CaptureResumeState(); Old->Destroy();
	auto* Pawn = Fixture.Restore(Snapshot, 2.f);
	TestEqual(TEXT("Two full undirected offline burn ticks exhaust shield15 and cost health25 despite defense"), Pawn->GetHealth(), 75.f);
	TestEqual(TEXT("Offline damage consumes the saved shield rather than granting it anew"), Pawn->GetShield(), 0.f);
	TestFalse(TEXT("Expired defense is not refreshed"), Pawn->IsDefending());
	TestFalse(TEXT("Expired root is not restored"), Pawn->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Rooted));
	Pawn->SetCombatSessionReady(true); Fixture.Advance(.6f);
	TestEqual(TEXT("Final remaining burn does not tick immediately on recovery"), Pawn->GetHealth(), 75.f);
	Fixture.Advance(.2f); TestEqual(TEXT("The preserved next periodic tick deals twenty once"), Pawn->GetHealth(), 55.f);
	Fixture.Advance(.3f);
	TestFalse(TEXT("Burn ends at the original remaining lifetime"), Pawn->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Burning));

	Old = Fixture.Character(); Source->ApplyBurning(Old, 5.f, 2.2f); Fixture.Advance(.25f);
	Snapshot = Old->CaptureResumeState(); Old->Destroy(); Pawn = Fixture.Restore(Snapshot, 6.f);
	TestEqual(TEXT("Long offline time settles only the two scheduled ticks before expiration"), Pawn->GetHealth(), 90.f);
	TestFalse(TEXT("Fully elapsed burn is absent"), Pawn->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Burning));
	Pawn->SetCombatSessionReady(true); Fixture.Advance(1.1f); TestEqual(TEXT("Expired effects cannot tick again later"), Pawn->GetHealth(), 90.f);

	Old = Fixture.Character(); Source->ApplyBurning(Old, 60.f, 4.f); Fixture.Advance(.25f);
	Snapshot = Old->CaptureResumeState(); Old->Destroy(); Pawn = Fixture.Restore(Snapshot, 2.1f);
	TestFalse(TEXT("Offline lethal periodic damage restores a dead pawn"), Pawn->IsAlive());
	TestEqual(TEXT("Offline death has zero health"), Pawn->GetHealth(), 0.f);
	TestEqual(TEXT("Offline death has zero shield"), Pawn->GetShield(), 0.f);
	TestTrue(TEXT("Offline death restores no persistent effects"), Pawn->CaptureResumeState().PersistentEffects.IsEmpty());
	Pawn->SetCombatSessionReady(true); Fixture.Advance(1.1f);
	TestFalse(TEXT("Session admission does not revive an offline-dead character"), Pawn->IsCombatReady());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRecoveryIntegerExpiryComparisonTest, "Multiplayer.Combat.Recovery.IntegerExpiryMatchesContinuous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 两个真实目标同时受相同效果；一个持续在线，另一个在到期前或完全到期后恢复，用实测跳数作基准。
bool FCombatRecoveryIntegerExpiryComparisonTest::RunTest(const FString&)
{
	for (float Duration : { 2.f, 3.f })
		for (bool bResumeBeforeExpiry : { true, false })
		{
			FCombatRecoveryWorld Fixture;
			auto* Source = Fixture.Character(true); auto* Continuous = Fixture.Character(); auto* Old = Fixture.Character();
			Source->ApplyBurning(Continuous, 5.f, Duration); Source->ApplyBurning(Old, 5.f, Duration);
			Fixture.Advance(.25f); const auto Snapshot = Old->CaptureResumeState(); Old->Destroy();
			if (!TestEqual(TEXT("Integer boundary snapshot contains its real burn effect"), Snapshot.PersistentEffects.Num(), 1)) return false;
			const auto& Burn = Snapshot.PersistentEffects[0];
			AddInfo(FString::Printf(TEXT("IntegerExpiry duration=%.2f early=%d remaining=%.9g nextTick=%.9g period=%.9g span=%.17g"),
				Duration, bResumeBeforeExpiry, Burn.Remaining, Burn.NextTickDelay, Burn.Spec.Data->GetPeriod(),
				double(Burn.Remaining) - double(Burn.NextTickDelay)));
			// 比下一跳早 50 微秒仍不得结算，防止把仅用于到期的容差扩大成所有周期的提前量。
			auto* BeforeTick = Fixture.Restore(Snapshot, Burn.NextTickDelay - .00005f);
			TestEqual(TEXT("Recovery before a still-live periodic deadline does not deal early damage"), BeforeTick->GetHealth(), 100.f);
			BeforeTick->Destroy();
			const float Offline = bResumeBeforeExpiry ? .4f : Duration + 1.f;
			Fixture.Advance(Offline);
			auto* Restored = Fixture.Restore(Snapshot, Offline);
			TestEqual(TEXT("Offline settlement matches the actual uninterrupted target at reconnect time"), Restored->GetHealth(), Continuous->GetHealth());
			TestFalse(TEXT("Integer-boundary restore also leaves admission closed"), Restored->IsCombatReady());
			Restored->SetCombatSessionReady(true); Fixture.Advance(Duration + 1.f);
			TestEqual(TEXT("Remaining live ticks and final expiry match uninterrupted execution"), Restored->GetHealth(), Continuous->GetHealth());
			TestFalse(TEXT("Restored integer-lifetime effect finally expires"), Restored->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Burning));
			TestFalse(TEXT("Continuous comparison effect also expires"), Continuous->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Burning));
		}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCombatRecoveryPendingFinalTickTest, "Multiplayer.Combat.Recovery.PendingFinalTickMatchesContinuous",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 精确二进制步长落在计时器截止值上：最终跳仍排队，快照不能把零剩余误当作已经结算。
bool FCombatRecoveryPendingFinalTickTest::RunTest(const FString&)
{
	FCombatRecoveryWorld Fixture;
	auto* Source = Fixture.Character(true); auto* Continuous = Fixture.Character(); auto* Old = Fixture.Character();
	Source->ApplyBurning(Continuous, 5.f, 2.f); Source->ApplyBurning(Old, 5.f, 2.f);
	// 首次 Tick 使 Pending 计时器在 .25 起算，随后精确到 2.25，严格大于才执行的最后跳尚未触发。
	for (int32 Step = 0; Step < 9; ++Step) { ++GFrameCounter; Fixture.World->Tick(LEVELTICK_All, .25f); }
	FGameplayEffectQuery Query; Query.EffectDefinition = UmultiplayerBurningEffect::StaticClass();
	const auto Handles = Old->GetAbilitySystemComponent()->GetActiveEffects(Query);
	if (!TestEqual(TEXT("Exact deadline still owns its pending burn effect"), Handles.Num(), 1)) return false;
	const auto* Active = Old->GetAbilitySystemComponent()->GetActiveGameplayEffect(Handles[0]);
	TestEqual(TEXT("Duration timer is exactly due, not a rounded nearby value"), Fixture.World->GetTimerManager().GetTimerRemaining(Active->DurationHandle), 0.f);
	TestEqual(TEXT("Periodic timer is also due and has not executed its last tick"), Fixture.World->GetTimerManager().GetTimerRemaining(Active->PeriodHandle), 0.f);
	TestEqual(TEXT("The uninterrupted target has received only the first tick at capture"), Continuous->GetHealth(), 95.f);
	const auto Snapshot = Old->CaptureResumeState(); Old->Destroy();
	if (!TestEqual(TEXT("Snapshot retains the one due final tick"), Snapshot.PersistentEffects.Num(), 1)) return false;
	TestEqual(TEXT("The retained final tick has zero lifetime left"), Snapshot.PersistentEffects[0].Remaining, 0.f);
	TestEqual(TEXT("The retained final tick is due now, not another period later"), Snapshot.PersistentEffects[0].NextTickDelay, 0.f);
	auto* Restored = Fixture.Restore(Snapshot, 0.f);
	TestFalse(TEXT("Settling a due final tick does not admit the player"), Restored->IsCombatReady());
	TestFalse(TEXT("An already-ended lifetime does not create another live burn"), Restored->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Burning));
	Fixture.Advance(.25f);
	TestEqual(TEXT("The continuous target executes the real final tick on the next frame"), Continuous->GetHealth(), 90.f);
	TestEqual(TEXT("Zero-lifetime offline settlement matches the continuous final tick"), Restored->GetHealth(), Continuous->GetHealth());
	Restored->SetCombatSessionReady(true); Fixture.Advance(1.25f);
	TestEqual(TEXT("The settled final tick cannot execute twice after admission"), Restored->GetHealth(), Continuous->GetHealth());
	return !HasAnyErrors();
}
#endif
