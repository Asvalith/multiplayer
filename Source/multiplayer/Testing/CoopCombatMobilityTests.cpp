// Copyright Epic Games, Inc. All Rights Reserved.

// 回归 GAS 冲刺的提交、方向快照、CMC 根运动及中断清理。
// 本组单世界检查真实移动与生命周期，客户端预测纠正由双端场景另行验证。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Abilities/GameplayAbilityTargetTypes.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatMobility.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/RootMotionSource.h"

namespace
{
	// 真实权威 ASC、任务、CMC 与碰撞；单世界只能验证生命周期，不能冒充丢包下的预测纠正验收。
	struct FCombatMobilityWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;

		// 创建可行走和碰撞的真实地面，避免用手动改坐标代替 CMC 冲刺。
		FCombatMobilityWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			Box(FVector(0, 0, -10), FVector(10000, 10000, 10));
		}

		// 结束任务和移动组件，销毁世界并恢复全局帧号。
		~FCombatMobilityWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 小步推进正式 CMC、根运动任务和冷却，观察自然结束或中断后的残留。
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

		// 创建真实地面或墙体碰撞，让距离与阻挡断言经 CMC 实际扫掠产生。
		AActor* Box(const FVector& Location, const FVector& Extent)
		{
			auto* Actor = World->SpawnActor<AActor>();
			auto* Component = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Component);
			Component->SetBoxExtent(Extent);
			Component->SetCollisionProfileName(TEXT("BlockAll"));
			Component->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}

		// 创建受控玩家并经正式成长入口学习冲刺，不绕过默认未学习规则授予能力。
		AmultiplayerCombatCharacter* Character(const FVector& Location = FVector(0, 0, 110))
		{
			auto* Player = World->SpawnActor<AmultiplayerCombatCharacter>(Location, FRotator::ZeroRotator);
			auto* Controller = World->SpawnActor<APlayerController>();
			Controller->SetPlayerState(World->SpawnActor<APlayerState>());
			Controller->Possess(Player);
			Player->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
			return Player;
		}
	};

	// 查询正式冲刺冷却 GE，用句柄数量与剩余时间检查重复扣费和冷却重置。
	FGameplayEffectQuery DashCooldownQuery()
	{
		return FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(FGameplayTagContainer(CombatTags::Cooldown_Dash));
	}

	// 通过真实定向事件提交方向，并故意附带无关远坐标验证服务器不信任客户端位置。
	void SendDashAim(AmultiplayerCombatCharacter* Character, const FRotator& Rotation)
	{
		auto* Aim = new FGameplayAbilityTargetData_LocationInfo();
		Aim->TargetLocation.LocationType = EGameplayAbilityTargetingLocationType::LiteralTransform;
		Aim->TargetLocation.LiteralTransform = FTransform(Rotation, FVector(100000, 100000, 100000));
		FGameplayEventData Event;
		Event.EventTag = CombatTags::Event_Dash;
		Event.Instigator = Character;
		Event.TargetData.Add(Aim);
		Character->GetAbilitySystemComponent()->HandleGameplayEvent(Event.EventTag, &Event);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashMovementTest, "Multiplayer.Combat.DashMovement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 不用手调位置或 LaunchCharacter；实际输入入口授予的冲刺必须产生接近配置距离的 CMC 位移。
bool FCoopCombatDashMovementTest::RunTest(const FString& Parameters)
{
	FCombatMobilityWorld Fixture;
	auto* Player = Fixture.Character();
	if (!TestNotNull(TEXT("Mobility fixture creates a possessed character"), Player)) return false;
	Fixture.Advance(.3f);
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
	const FmultiplayerCombatSkillRule& Rule = Player->GetCombatConfig().Dash;
	TestEqual(TEXT("Dash uses native local prediction"), GetDefault<UmultiplayerDashAbility>()->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	if (!TestNotNull(TEXT("Character grants its real dash ability"), ASC->FindAbilitySpecFromClass(UmultiplayerDashAbility::StaticClass()))
		|| !TestTrue(TEXT("Possession and real floor settle the player into walking"), Movement->IsMovingOnGround())) return false;
	Player->GetController()->SetControlRotation(FRotator(0, 30, 0));
	const FVector Direction = FRotator(0, 30, 0).Vector();
	const FVector Before = Player->GetActorLocation();
	const float EnergyBefore = Player->GetEnergy();
	Player->Dash();
	if (!TestTrue(TEXT("Actual dash activation owns its temporary state"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing))) return false;
	TestTrue(TEXT("Dash creates an actual CMC root motion source"), Movement->GetRootMotionSource(TEXT("CombatDash")).IsValid());
	TestEqual(TEXT("One dash commits exactly one configured cost"), Player->GetEnergy(), EnergyBefore - Rule.Cost);
	TestEqual(TEXT("One dash creates one real cooldown effect"), ASC->GetActiveEffects(DashCooldownQuery()).Num(), 1);
	Fixture.Advance(.1f);
	const TArray<float> CooldownsBeforeRetry = ASC->GetActiveEffectsTimeRemaining(DashCooldownQuery());
	Player->Dash();
	TestEqual(TEXT("A duplicate request does not charge again"), Player->GetEnergy(), EnergyBefore - Rule.Cost);
	TestEqual(TEXT("A duplicate request does not add another cooldown"), ASC->GetActiveEffects(DashCooldownQuery()).Num(), 1);
	const TArray<float> CooldownsAfterRetry = ASC->GetActiveEffectsTimeRemaining(DashCooldownQuery());
	if (CooldownsBeforeRetry.Num() == 1 && CooldownsAfterRetry.Num() == 1)
		TestTrue(TEXT("A rejected request does not restart the running cooldown"), FMath::IsNearlyEqual(CooldownsBeforeRetry[0], CooldownsAfterRetry[0], .001f));
	TestTrue(TEXT("Root motion really moves the capsule during the dash"), FVector::Dist2D(Before, Player->GetActorLocation()) > 80.f);
	Player->GetController()->SetControlRotation(FRotator(0, -90, 0));
	Fixture.Advance(Rule.Duration + .1f);
	const FVector Displacement = Player->GetActorLocation() - Before;
	TestTrue(TEXT("Unobstructed dash distance matches the configured range"), FMath::IsNearlyEqual(float(Displacement.Size2D()), Rule.Range, 25.f));
	TestTrue(TEXT("Later camera turns cannot change the accepted dash direction"), FVector::DotProduct(Displacement.GetSafeNormal2D(), Direction) > .999f);
	TestTrue(TEXT("Grounded dash cannot lift the capsule"), FMath::Abs(Displacement.Z) < .1f);
	TestFalse(TEXT("Normal completion removes its owned dash state"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing));
	TestFalse(TEXT("Normal completion removes its root motion source"), Movement->HasRootMotionSources());
	const FVector After = Player->GetActorLocation();
	Fixture.Advance(.2f);
	TestTrue(TEXT("Completed dash leaves no residual horizontal acceleration"), FVector::Dist2D(After, Player->GetActorLocation()) < .1f);
	TestEqual(TEXT("Completion does not charge a second time"), Player->GetEnergy(), EnergyBefore - Rule.Cost);
	AddInfo(FString::Printf(TEXT("Real dash distance %.2fcm; configured %.2fcm/%.2fs"), Displacement.Size2D(), Rule.Range, Rule.Duration));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashCollisionTest, "Multiplayer.Combat.DashCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 高速位移依然由 CMC Sweep 处理墙体；不能用目标位置相等来掩盖穿墙再拉回。
bool FCoopCombatDashCollisionTest::RunTest(const FString& Parameters)
{
	FCombatMobilityWorld Fixture;
	auto* Player = Fixture.Character();
	Fixture.Box(FVector(220, 0, 200), FVector(20, 500, 200));
	Fixture.Advance(.3f);
	const FVector Before = Player->GetActorLocation();
	Player->Dash();
	if (!TestTrue(TEXT("Wall probe starts a real dash"), Player->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing))) return false;
	float FurthestX = Player->GetActorLocation().X;
	for (float Time = 0.f; Time < Player->GetCombatConfig().Dash.Duration + .2f; Time += .01f)
	{
		Fixture.Advance(.01f);
		FurthestX = FMath::Max(FurthestX, float(Player->GetActorLocation().X));
	}
	const float WallContactX = 200.f - Player->GetCapsuleComponent()->GetScaledCapsuleRadius();
	TestTrue(TEXT("Dash reaches the actual wall rather than failing to activate"), Player->GetActorLocation().X - Before.X > 100.f);
	TestTrue(TEXT("No intermediate CMC step crosses the wall face"), FurthestX <= WallContactX + 1.f);
	TestTrue(TEXT("Collision is resolved at capsule radius from the wall"), FMath::IsNearlyEqual(float(Player->GetActorLocation().X), WallContactX, 2.f));
	TestFalse(TEXT("Blocked dash still completes its ability"), Player->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing));
	TestFalse(TEXT("Collision does not leave a root motion source"), Player->GetCharacterMovement()->HasRootMotionSources());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashHorizontalGravityTest, "Multiplayer.Combat.DashHorizontalGravity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 网络格式的事件只提供水平朝向；地面不会因抬头飞起，空中则继续受真实 CMC 重力影响。
bool FCoopCombatDashHorizontalGravityTest::RunTest(const FString& Parameters)
{
	FCombatMobilityWorld Fixture;
	auto* Player = Fixture.Character();
	Fixture.Advance(.3f);
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	FGameplayEventData Empty;
	Empty.EventTag = CombatTags::Event_Dash;
	const float EnergyBefore = Player->GetEnergy();
	ASC->HandleGameplayEvent(Empty.EventTag, &Empty);
	TestFalse(TEXT("A directional dash event without an aim snapshot is rejected"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing));
	TestEqual(TEXT("Malformed aim is rejected before cost"), Player->GetEnergy(), EnergyBefore);
	const FVector Before = Player->GetActorLocation();
	SendDashAim(Player, FRotator(80, 90, 35));
	if (!TestTrue(TEXT("A valid tilted aim snapshot activates through GAS events"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing))) return false;
	Fixture.Advance(Player->GetCombatConfig().Dash.Duration + .15f);
	const FVector Delta = Player->GetActorLocation() - Before;
	TestTrue(TEXT("Only snapshot yaw, not server controller yaw or payload location, determines travel"),
		Delta.Y > Player->GetCombatConfig().Dash.Range - 25.f && FMath::Abs(Delta.X) < .1f);
	TestTrue(TEXT("Pitch and roll cannot inject vertical dash movement"), FMath::Abs(Delta.Z) < .1f);

	auto* Falling = Fixture.Character(FVector(0, 2000, 1500));
	Fixture.Advance(.05f);
	if (!TestTrue(TEXT("Airborne probe is really falling before dash"), Falling->GetCharacterMovement()->IsFalling())) return false;
	const FVector BeforeFall = Falling->GetActorLocation();
	const float InitialDownwardSpeed = Falling->GetVelocity().Z;
	SendDashAim(Falling, FRotator(-80, 0, 0));
	if (!TestTrue(TEXT("Falling character starts the real dash"), Falling->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing))) return false;
	Fixture.Advance(Falling->GetCombatConfig().Dash.Duration + .1f);
	TestTrue(TEXT("Dash still travels horizontally while falling"), Falling->GetActorLocation().X - BeforeFall.X > 300.f);
	TestTrue(TEXT("Dash and its finish mode do not suspend gravity or reset downward velocity"),
		Falling->GetActorLocation().Z < BeforeFall.Z - 30.f && Falling->GetVelocity().Z < InitialDownwardSpeed - 100.f);
	TestTrue(TEXT("Airborne completion retains the pre-existing falling mode"), Falling->GetCharacterMovement()->IsFalling());
	TestFalse(TEXT("Airborne completion removes the force source"), Falling->GetCharacterMovement()->HasRootMotionSources());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashControlBoundaryTest, "Multiplayer.Combat.DashControlBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 控制、死亡与连接接纳走现有角色入口；取消后只清自己的位移任务，不额外回退位置或写速度。
bool FCoopCombatDashControlBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatMobilityWorld Fixture;
	for (int32 Exit = 0; Exit < 5; ++Exit)
	{
		auto* Player = Fixture.Character(FVector(0, Exit * 1800.f, 110));
		Fixture.Advance(.3f);
		UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
		Player->Dash();
		if (!TestTrue(TEXT("Each interruption starts a real dash"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing))) return false;
		Fixture.Advance(.08f);
		TestTrue(TEXT("Each cancellation probe moves before interruption"), Player->GetActorLocation().X > 50.f);
		if (Exit == 0)
		{
			FGameplayTagContainer DashTags(CombatTags::Ability_Dash);
			ASC->CancelAbilities(&DashTags);
		}
		else if (Exit == 1) Player->ApplyRoot(.2f);
		else if (Exit == 2) Player->ApplyStun(.2f);
		else if (Exit == 3) Player->SetCombatSessionReady(false);
		else
		{
			auto Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
			Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 1000.f);
			ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
			TestFalse(TEXT("Real lethal damage commits death during dash"), Player->IsAlive());
		}
		TestFalse(*FString::Printf(TEXT("Exit %d immediately cancels dash and releases its state"), Exit), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing));
		const TSharedPtr<FRootMotionSource> Source = Player->GetCharacterMovement()->GetRootMotionSource(TEXT("CombatDash"));
		TestTrue(*FString::Printf(TEXT("Exit %d destroys its task and marks only its force for removal"), Exit),
			!Source.IsValid() || Source->Status.HasFlag(ERootMotionSourceStatusFlags::MarkedForRemoval));
		Fixture.Advance(.03f);
		const FVector AfterCancel = Player->GetActorLocation();
		Fixture.Advance(Player->GetCombatConfig().Dash.Duration + .3f);
		TestTrue(*FString::Printf(TEXT("Exit %d has no delayed or residual horizontal dash displacement"), Exit),
			FVector::Dist2D(AfterCancel, Player->GetActorLocation()) < .1f);
		TestFalse(*FString::Printf(TEXT("Exit %d leaves no CMC force source"), Exit), Player->GetCharacterMovement()->HasRootMotionSources());
	}

	// 起手前已有控制或未接纳时，不先扣费再取消；拒绝请求不能留下冷却和待应用的位移源。
	for (int32 Restriction = 0; Restriction < 3; ++Restriction)
	{
		auto* Player = Fixture.Character(FVector(-2000, Restriction * 1800.f, 110));
		Fixture.Advance(.3f);
		if (Restriction == 0) Player->ApplyRoot(.3f);
		else if (Restriction == 1) Player->ApplyStun(.3f);
		else Player->SetCombatSessionReady(false);
		const float BeforeEnergy = Player->GetEnergy();
		Player->Dash();
		UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
		TestFalse(TEXT("Root, stun or unadmitted session rejects dash before activation"), ASC->HasMatchingGameplayTag(CombatTags::State_Dashing));
		TestEqual(TEXT("Rejected controlled dash does not spend energy"), Player->GetEnergy(), BeforeEnergy);
		TestEqual(TEXT("Rejected controlled dash does not add cooldown"), ASC->GetActiveEffects(DashCooldownQuery()).Num(), 0);
		TestFalse(TEXT("Rejected controlled dash cannot create pending root motion"), Player->GetCharacterMovement()->HasRootMotionSources());
	}
	return !HasAnyErrors();
}

#endif
