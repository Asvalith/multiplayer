// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace
{
	// 独立服务器世界推进真实 GAS 和 Timer；不创建客户端，不能冒充网络预测或画面验收。
	struct FCombatLifecycleWorld
	{
		UWorld* World = nullptr;
		uint64 PreviousFrame = GFrameCounter;

		FCombatLifecycleWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);

			// 施法仍走真实落点射线；不能直接传入一个客户端声称合法的命中坐标。
			AActor* Ground = World->SpawnActor<AActor>();
			UBoxComponent* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(5000.0f, 5000.0f, 10.0f));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0.0f, 0.0f, -10.0f));
		}

		// 销毁世界前结束角色和技能，销毁后不留下旧 Timer 或 GFrameCounter 改动。
		~FCombatLifecycleWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 小步推进避免跨过起手、释放和收招边界，所有断言读取真实效果结果。
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

		// 角色没有玩家输入；朝向和技能仍使用生产代码，不调用内部释放回调。
		AmultiplayerCombatCharacter* Player(const FVector& Location = FVector(0.0f, 0.0f, 100.0f))
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Character = World->SpawnActor<AmultiplayerCombatCharacter>(Location, FRotator::ZeroRotator, Spawn);
			// 无 Controller 的独立世界默认可能是 MOVE_None；本夹具明确模拟可行走的玩家。
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			return Character;
		}

		// 靶敌关闭自动决策，不让 BT 发出的普通攻击污染施法取消与死亡用例。
		AmultiplayerCombatEnemy* Enemy(const FVector& Location)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Character = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(AmultiplayerCombatEnemy::StaticClass(),
				Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Character->AutoPossessAI = EAutoPossessAI::Disabled;
			Character->FinishSpawning(Transform);
			return Character;
		}
	};

	// 测试伤害也通过生产 GameplayEffect，避免直接写 Health 跳过死亡与清理链路。
	void Damage(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target, float Amount)
	{
		UAbilitySystemComponent* ASC = Source->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f,
			ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
		ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaLifecycleTest, "Multiplayer.Combat.AreaLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 起手不结算，释放只命中合法敌人一次，收招结束后释放施法状态与移动限制。
bool FCoopCombatAreaLifecycleTest::RunTest(const FString& Parameters)
{
	FCombatLifecycleWorld Fixture;
	auto* Caster = Fixture.Player();
	const auto& Rule = Caster->GetCombatConfig().Area;
	const FVector Target(Rule.Range, 0.0f, 100.0f);
	auto* Enemy = Fixture.Enemy(Target);
	auto* Friend = Fixture.Player(Target + FVector(0.0f, 70.0f, 0.0f));
	auto* Outside = Fixture.Enemy(Target + FVector(0.0f, Caster->GetCombatConfig().AreaRadius + 200.0f, 0.0f));
	if (!TestNotNull(TEXT("Caster"), Caster) || !TestNotNull(TEXT("Enemy"), Enemy)
		|| !TestNotNull(TEXT("Friend"), Friend) || !TestNotNull(TEXT("Outside"), Outside)) return false;
	const float Health = Enemy->GetHealth();
	const float Energy = Caster->GetEnergy();
	Caster->CastArea();
	TestTrue(TEXT("Area keeps an active casting lifecycle"), Caster->IsCasting());
	TestEqual(TEXT("Cost committed exactly once on activation"), Caster->GetEnergy(), Energy - Rule.Cost);
	Caster->CastArea();
	TestEqual(TEXT("Repeated input does not commit a second cost"), Caster->GetEnergy(), Energy - Rule.Cost);
	Fixture.Advance(Rule.Windup * 0.6f);
	TestEqual(TEXT("Windup cannot deal early damage"), Enemy->GetHealth(), Health);
	TestTrue(TEXT("Cast progress reflects the active windup"), Caster->GetCastProgress() > 0.0f);
	Fixture.Advance(Rule.Windup * 0.4f + 0.08f);
	TestEqual(TEXT("Authoritative release damages the enemy once"), Enemy->GetHealth(), Health - Rule.Amount);
	TestEqual(TEXT("Area does not damage a same-team character"), Friend->GetHealth(), 100.0f);
	TestEqual(TEXT("Area does not damage outside its configured radius"), Outside->GetHealth(), 100.0f);
	Fixture.Advance(Rule.Recovery + 0.2f);
	TestFalse(TEXT("Recovery finishes the active casting state"), Caster->IsCasting());
	TestEqual(TEXT("Recovery never repeats damage"), Enemy->GetHealth(), Health - Rule.Amount);
	TestTrue(TEXT("Lifecycle exit restores movement"), Caster->GetCharacterMovement()->MovementMode != MOVE_None);
	TestTrue(TEXT("Cooldown outlives the casting lifecycle"), Caster->GetAreaCooldownRemaining() > 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCastInterruptionsTest, "Multiplayer.Combat.CastInterruptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 主动取消、眩晕、断线与销毁都必须撤销延时释放，不只把 UI 的施法条隐藏。
bool FCoopCombatCastInterruptionsTest::RunTest(const FString& Parameters)
{
	for (int32 Interruption = 0; Interruption < 4; ++Interruption)
	{
		FCombatLifecycleWorld Fixture;
		auto* Caster = Fixture.Player();
		const auto Rule = Caster->GetCombatConfig().Area;
		auto* Enemy = Fixture.Enemy(FVector(Rule.Range, 0.0f, 100.0f));
		Caster->CastArea();
		TestTrue(TEXT("Fixture entered real casting"), Caster->IsCasting());
		Fixture.Advance(Rule.Windup * 0.25f);
		if (Interruption == 0) Caster->CancelCast();
		else if (Interruption == 1) Caster->ApplyStun(0.3f);
		else if (Interruption == 2) Caster->SetCombatSessionReady(false);
		else Caster->Destroy();
		if (Interruption != 3)
		{
			TestFalse(TEXT("Interruption ends the active casting tag"), Caster->IsCasting());
			TestEqual(TEXT("Cancellation does not refund an already committed cost"), Caster->GetEnergy(), 100.0f - Rule.Cost);
		}
		Fixture.Advance(Rule.Windup + Rule.Recovery + 0.2f);
		TestEqual(TEXT("Old delayed release does not survive interruption"), Enemy->GetHealth(), 100.0f);
		if (Interruption < 2)
			TestTrue(TEXT("Cancel or expired stun restores movement"), Caster->GetCharacterMovement()->MovementMode != MOVE_None);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatStunLifecycleTest, "Multiplayer.Combat.StunLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 眩晕再次施加刷新一个效果；死亡统一清理效果、技能任务与移动，不留下可执行旧回调。
bool FCoopCombatStunLifecycleTest::RunTest(const FString& Parameters)
{
	FCombatLifecycleWorld Fixture;
	auto* Player = Fixture.Player();
	auto* Enemy = Fixture.Enemy(FVector(1500.0f, 0.0f, 100.0f));
	Player->ApplyStun(0.5f);
	TestTrue(TEXT("Stun state is granted by a real effect"), Player->IsStunned());
	Fixture.Advance(0.3f);
	Player->ApplyStun(0.8f);
	const FGameplayEffectQuery StunQuery = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(
		FGameplayTagContainer(CombatTags::State_Stunned));
	TestEqual(TEXT("Refresh does not create stacked stun effects"),
		Player->GetAbilitySystemComponent()->GetActiveEffects(StunQuery).Num(), 1);
	TestTrue(TEXT("Refresh uses the new duration"), Player->GetStunRemaining() > 0.7f);
	Player->CastArea();
	Player->Defend();
	TestFalse(TEXT("A stunned character cannot start casting"), Player->IsCasting());
	TestEqual(TEXT("Rejected skills do not spend energy while stunned"), Player->GetEnergy(), 100.0f);
	Fixture.Advance(0.9f);
	TestFalse(TEXT("Refreshed stun expires"), Player->IsStunned());
	TestEqual(TEXT("Expired stun has no remaining time"), Player->GetStunRemaining(), 0.0f);
	TestTrue(TEXT("Stun expiration restores movement"), Player->GetCharacterMovement()->MovementMode != MOVE_None);
	Player->CastArea();
	Player->ApplyStun(3.0f);
	Damage(Enemy, Player, 1000.0f);
	TestFalse(TEXT("Lethal damage commits death"), Player->IsAlive());
	TestFalse(TEXT("Death clears the casting state"), Player->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Casting));
	TestFalse(TEXT("Death clears the actual stun tag"), Player->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Stunned));
	TestFalse(TEXT("Death clears the actual area cooldown tag"), Player->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::Cooldown_Area));
	Fixture.Advance(4.0f);
	TestTrue(TEXT("Old stun expiry cannot restore dead movement"), Player->GetCharacterMovement()->MovementMode == MOVE_None);
	TestFalse(TEXT("Dead body remains outside mechanism occupancy"), Player->GetActorEnableCollision());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatControlResumeTest, "Multiplayer.Combat.ControlResume",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 重连恢复的是服务器剩余效果；扣除离线时间，不恢复半次施法，不把到期效果重新激活。
bool FCoopCombatControlResumeTest::RunTest(const FString& Parameters)
{
	FCombatLifecycleWorld Fixture;
	auto* Original = Fixture.Player();
	Original->CastArea();
	const FmultiplayerCombatResumeState Casting = Original->CaptureResumeState();
	TestTrue(TEXT("Casting snapshot retains committed cooldown"), Casting.AreaCooldown > 0.0f);
	Original->SetCombatSessionReady(false);
	auto* Fresh = Fixture.Player(FVector(0.0f, 1500.0f, 100.0f));
	Fresh->SetCombatSessionReady(false);
	Fresh->RestoreResumeState(Casting, 0.25f);
	Fresh->SetCombatSessionReady(true);
	TestFalse(TEXT("Reconnect never resumes an uncompleted cast"), Fresh->IsCasting());
	TestEqual(TEXT("Reconnect does not refund cast resource cost"), Fresh->GetEnergy(), Casting.Energy);
	TestTrue(TEXT("Area cooldown deducts offline time"), FMath::IsNearlyEqual(
		Fresh->GetAreaCooldownRemaining(), Casting.AreaCooldown - 0.25f, 0.05f));
	Fresh->ApplyStun(2.0f);
	const FmultiplayerCombatResumeState Controlled = Fresh->CaptureResumeState();
	TestTrue(TEXT("Snapshot stores actual stun remaining time"), Controlled.StunRemaining > 1.9f);
	auto* Resumed = Fixture.Player(FVector(0.0f, 3000.0f, 100.0f));
	Resumed->SetCombatSessionReady(false);
	Resumed->RestoreResumeState(Controlled, 0.6f);
	// 服务器握手接纳不允许覆盖恢复中的眩晕禁行。
	Resumed->SetCombatSessionReady(true);
	TestTrue(TEXT("Remaining stun survives re-admission"), Resumed->IsStunned());
	TestTrue(TEXT("Stun duration deducts offline time"), FMath::IsNearlyEqual(
		Resumed->GetStunRemaining(), Controlled.StunRemaining - 0.6f, 0.05f));
	TestTrue(TEXT("Re-admission preserves stun movement restriction"), Resumed->GetCharacterMovement()->MovementMode == MOVE_None);
	Resumed->CastArea();
	TestFalse(TEXT("Restored stun rejects a new cast"), Resumed->IsCasting());
	auto* Expired = Fixture.Player(FVector(0.0f, 4500.0f, 100.0f));
	Expired->SetCombatSessionReady(false);
	Expired->RestoreResumeState(Controlled, Controlled.AreaCooldown + Controlled.StunRemaining + 1.0f);
	Expired->SetCombatSessionReady(true);
	TestFalse(TEXT("Expired offline stun is not recreated"), Expired->IsStunned());
	TestEqual(TEXT("Expired offline cooldown is not recreated"), Expired->GetAreaCooldownRemaining(), 0.0f);
	TestTrue(TEXT("Expired offline restriction allows normal movement"), Expired->GetCharacterMovement()->MovementMode != MOVE_None);
	return true;
}

#endif
