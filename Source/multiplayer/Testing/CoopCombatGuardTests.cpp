// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "UObject/UnrealType.h"

namespace
{
	// 真正的 Q、伤害 GE 和弹体扫掠；敌人关闭自主 AI，避免额外攻击污染方向断言。
	struct FCombatGuardWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FCombatGuardWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			auto* Ground = World->SpawnActor<AActor>();
			auto* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(5000, 5000, 10));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0, 0, -10));
		}
		~FCombatGuardWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}
		void Advance(float Seconds)
		{
			for (float Time = 0.f; Time < Seconds;)
			{
				const float Step = FMath::Min(.01f, Seconds - Time);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Time += Step;
			}
		}
		AmultiplayerCombatCharacter* Character(bool bEnemy, const FVector& Location, UmultiplayerCombatConfig* Config = nullptr)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Pawn = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(bEnemy ? AmultiplayerCombatEnemy::StaticClass()
				: AmultiplayerCombatCharacter::StaticClass(), Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Pawn->AutoPossessAI = EAutoPossessAI::Disabled;
			if (Config)
				FindFProperty<FObjectPropertyBase>(Pawn->GetClass(), TEXT("CombatConfig"))->SetObjectPropertyValue_InContainer(Pawn, Config);
			Pawn->FinishSpawning(Transform);
			Pawn->GetCharacterMovement()->DisableMovement();
			return Pawn;
		}
		AmultiplayerCombatProjectile* Fire(AmultiplayerCombatCharacter* Source, const FVector& Location, const FVector& Direction)
		{
			const FTransform Transform(Direction.Rotation(), Location);
			auto* Projectile = World->SpawnActorDeferred<AmultiplayerCombatProjectile>(AmultiplayerCombatProjectile::StaticClass(),
				Transform, Source, Source, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Projectile->Initialize(Source, 50.f, 1000.f);
			Projectile->FinishSpawning(Transform);
			return Projectile;
		}
	};
	UmultiplayerCombatConfig* GuardConfig()
	{
		auto* Config = NewObject<UmultiplayerCombatConfig>();
		Config->DefenseShieldAmount = 35.f;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGuardFacingTest, "Multiplayer.Combat.Guard.FacingAndExpiry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopCombatGuardFacingTest::RunTest(const FString&)
{
	for (int32 Case = 0; Case < 7; ++Case)
	{
		FCombatGuardWorld Fixture;
		auto* Target = Fixture.Character(false, FVector(0, 0, 100), GuardConfig());
		const float Angle = Case == 1 ? 90.f : Case == 2 ? 180.f : Case == 3 ? 50.f : Case == 4 ? 70.f : 0.f;
		const FVector Offset = Case == 6 ? FVector::ZeroVector : FRotator(0, Angle, 0).Vector() * 500.f;
		auto* Source = Fixture.Character(true, Target->GetActorLocation() + Offset);
		Target->Defend();
		TestTrue(TEXT("Real Q grants a lasting defensive effect"), Target->IsDefending());
		TestEqual(TEXT("Real Q grants the configured shield once"), Target->GetShield(), 35.f);
		if (Case == 5) Fixture.Advance(3.1f);
		TestTrue(TEXT("The authoritative direct attack is accepted"), Source->ApplyCombatDamage(Target, 50.f));
		const bool bBlocked = Case == 0 || Case == 3;
		TestEqual(*FString::Printf(TEXT("Case %d: only an active frontal guard preserves health"), Case),
			Target->GetHealth(), bBlocked ? 100.f : 85.f);
		TestEqual(*FString::Printf(TEXT("Case %d: a full block consumes no shield; other hits use all fifty damage"), Case),
			Target->GetShield(), bBlocked ? 35.f : 0.f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGuardBodyFacingTest, "Multiplayer.Combat.Guard.BodyFacingIgnoresCamera",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopCombatGuardBodyFacingTest::RunTest(const FString&)
{
	FCombatGuardWorld Fixture;
	auto* Target = Fixture.Character(false, FVector(0, 0, 100), GuardConfig());
	auto* Source = Fixture.Character(true, FVector(500, 0, 100));
	auto* Controller = Fixture.World->SpawnActor<APlayerController>();
	Controller->SetPlayerState(Fixture.World->SpawnActor<APlayerState>());
	Controller->Possess(Target);
	Controller->SetControlRotation(FRotator(0, 180, 0));
	Target->SetActorRotation(FRotator::ZeroRotator);
	Target->Defend();
	TestTrue(TEXT("Camera and body point in opposite directions"),
		FVector::DotProduct(Controller->GetControlRotation().Vector(), Target->GetActorForwardVector()) < -.99f);
	Source->ApplyCombatDamage(Target, 50.f);
	TestEqual(TEXT("Body-front damage is fully blocked while the camera looks backward"), Target->GetHealth(), 100.f);
	TestEqual(TEXT("A body-front block preserves its shield"), Target->GetShield(), 35.f);
	Controller->SetControlRotation(FRotator::ZeroRotator);
	Target->SetActorRotation(FRotator(0, 180, 0));
	Source->ApplyCombatDamage(Target, 50.f);
	TestEqual(TEXT("A camera-front but body-rear attack deals normal overflow damage"), Target->GetHealth(), 85.f);
	TestEqual(TEXT("Body-rear damage consumes the shield normally"), Target->GetShield(), 0.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGuardProjectileDirectionTest, "Multiplayer.Combat.Guard.ProjectileDirectionSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopCombatGuardProjectileDirectionTest::RunTest(const FString&)
{
	for (bool bFront : {true, false})
	{
		FCombatGuardWorld Fixture;
		auto* Target = Fixture.Character(false, FVector(0, 0, 100), GuardConfig());
		const float Sign = bFront ? 1.f : -1.f;
		auto* Source = Fixture.Character(true, FVector(Sign * 600.f, 0, 100));
		Target->Defend();
		Fixture.Advance(.35f);
		TestTrue(TEXT("Guard remains active after the short parry window expires"), Target->IsDefending() && !Target->IsParrying());
		TWeakObjectPtr<AmultiplayerCombatProjectile> Projectile = Fixture.Fire(Source,
			FVector(Sign * 500.f, 0, 100), FVector(-Sign, 0, 0));
		Source->SetActorLocation(FVector(-Sign * 600.f, 0, 100));
		Fixture.Advance(.65f);
		TestFalse(TEXT("A real swept contact consumes the projectile exactly once"), Projectile.IsValid());
		TestEqual(TEXT("The incoming trajectory, not the moved shooter's location, decides the block"),
			Target->GetHealth(), bFront ? 100.f : 85.f);
		TestEqual(TEXT("Only a projectile arriving from the body front preserves the shield"),
			Target->GetShield(), bFront ? 35.f : 0.f);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGuardUndirectedDamageTest, "Multiplayer.Combat.Guard.UndirectedAndBurningDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopCombatGuardUndirectedDamageTest::RunTest(const FString&)
{
	FCombatGuardWorld Fixture;
	auto* Target = Fixture.Character(false, FVector(0, 0, 100), GuardConfig());
	auto* Source = Fixture.Character(true, FVector(-500, 0, 100));
	Target->Defend();
	TestTrue(TEXT("A real periodic burn is accepted"), Source->ApplyBurning(Target, 20.f, 2.2f).IsValid());
	Source->SetActorLocation(FVector(500, 0, 100));
	Fixture.Advance(1.05f);
	TestEqual(TEXT("Moving the burn source to the guarded front cannot block its first tick"), Target->GetShield(), 15.f);
	Source->SetActorLocation(FVector(-500, 0, 100));
	Fixture.Advance(1.05f);
	TestEqual(TEXT("Both periodic ticks deal full damage despite an active guard"), Target->GetHealth(), 95.f);
	TestEqual(TEXT("Undirected burn exhausts the shield before damaging health"), Target->GetShield(), 0.f);
	TestTrue(TEXT("Burn damage did not require the guard to expire"), Target->IsDefending());

	auto* Undirected = Fixture.Character(false, FVector(0, 1000, 100), GuardConfig());
	Source->SetActorLocation(FVector(500, 1000, 100));
	Undirected->Defend();
	auto* ASC = Source->GetAbilitySystemComponent();
	auto Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	TestFalse(TEXT("The legacy direct-effect fixture supplies no incoming direction"), Spec.Data->GetContext().HasOrigin());
	Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 50.f);
	ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Undirected->GetAbilitySystemComponent());
	TestEqual(TEXT("An undirected effect never infers invulnerability from its instigator's current location"), Undirected->GetHealth(), 85.f);
	TestEqual(TEXT("An undirected effect settles the entire shield cost"), Undirected->GetShield(), 0.f);

	auto* AreaTarget = Fixture.Character(false, FVector(0, 2000, 100), GuardConfig());
	Source->SetActorLocation(FVector(500, 2000, 100));
	AreaTarget->Defend();
	Source->ResolveArea(FVector(0, 2000, 0));
	TestEqual(TEXT("Frontal guard does not suppress a real area's undirected damage"), AreaTarget->GetShield(), 0.f);
	TestEqual(TEXT("The area's thirty-five damage is absorbed by its thirty-five shield"), AreaTarget->GetHealth(), 100.f);
	return !HasAnyErrors();
}

#endif
