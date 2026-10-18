// Copyright Epic Games, Inc. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Components/CapsuleComponent.h"
#include "Components/ProgressBar.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"

namespace
{
	// 验证真实 BP/组件/属性事件；朝向与遮挡材质契约不冒充有画面的 RT 验收。
	struct FWorldHealthFixture
	{
		UWorld* World;
		FWorldHealthFixture(ENetMode Mode = NM_Standalone)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->SetPlayInEditorInitialNetMode(Mode);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			if (Mode != NM_Standalone) World->WorldType = EWorldType::PIE;
		}
		~FWorldHealthFixture()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
		AmultiplayerCombatEnemy* Spawn(UClass* Class, bool bLegacy = false)
		{
			const FTransform Transform(FRotator(0.f, 35.f, 0.f), FVector(500.f, 0.f, 150.f), FVector(1.4f));
			auto* Enemy = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(Class, Transform, nullptr, nullptr,
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Enemy) return nullptr;
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			if (bLegacy)
			{
				auto* Bar = Enemy->FindComponentByClass<UmultiplayerEnemyHealthWidgetComponent>();
				if (!Bar) { Enemy->Destroy(); return nullptr; }
				Bar->SetWidgetSpace(EWidgetSpace::Screen);
				Bar->SetAbsolute(true, false, false);
				Bar->SetWorldLocationAndRotation(FVector(9999.f), FRotator(60.f, 40.f, 20.f));
				Bar->SetWorldScale3D(FVector(3.f));
			}
			Enemy->FinishSpawning(Transform);
			Enemy->GetCharacterMovement()->SetComponentTickEnabled(false);
			return Enemy;
		}
	};
	UClass* EnemyHealthBlueprint()
	{
		return LoadClass<AmultiplayerCombatEnemy>(nullptr,
			TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopWorldHealthAnchorTest, "Multiplayer.Combat.WorldHealth.AnchorAndView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopWorldHealthAnchorTest::RunTest(const FString&)
{
	FWorldHealthFixture Fixture;
	UClass* Blueprint = EnemyHealthBlueprint();
	if (!TestNotNull(TEXT("Real saved minion BP loads"), Blueprint)) return false;
	for (UClass* Class : {Blueprint, AmultiplayerCombatEnemy::StaticClass()})
	{
		auto* Enemy = Fixture.Spawn(Class, true);
		if (!TestNotNull(TEXT("Native and saved BP enemies spawn"), Enemy)) return false;
		auto* Bar = Enemy->FindComponentByClass<UmultiplayerEnemyHealthWidgetComponent>();
		if (!TestNotNull(TEXT("Saved BP uses the native world-health component"), Bar)) return false;
		TestTrue(TEXT("Legacy Screen defaults are repaired at instance initialization"), Bar->GetWidgetSpace() == EWidgetSpace::World);
		TestTrue(TEXT("Anchor follows the CMC-smoothed body rather than the authority capsule"), Bar->GetAttachParent() == Enemy->GetMesh());
		const FVector Expected = Enemy->GetCapsuleComponent()->GetComponentTransform().TransformPosition(
			FVector(0.f, 0.f, Enemy->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() + 28.f));
		TestTrue(TEXT("Scaled characters keep a capsule-height-aware anchor"), Bar->GetComponentLocation().Equals(Expected, .01f));
		TestTrue(TEXT("World display size remains 72x12cm independent of the source mesh scale"),
			Bar->GetDrawSize().Equals(FVector2D(180.f, 30.f)) && Bar->GetComponentScale().Equals(FVector(.4f), .001f));
		TestTrue(TEXT("Contents redraw only on demand, not every facing update"), Bar->GetManuallyRedraw() && !Bar->GetTickWhenOffscreen());
		TestFalse(TEXT("Per-view display transforms are not replicated"), Bar->GetIsReplicated());
		TestTrue(TEXT("World UI adds no collision, input focus, shadows or RT geometry"),
			Bar->GetCollisionEnabled() == ECollisionEnabled::NoCollision && !Bar->GetWindowFocusable()
			&& !Bar->CastShadow && !Bar->bVisibleInRayTracing);
		UMaterialInterface* Material = Bar->GetMaterial(0);
		if (!TestNotNull(TEXT("World health uses a real engine widget material"), Material)) return false;
		TestFalse(TEXT("World health honors scene depth instead of drawing through walls"), bool(Material->GetMaterial()->bDisableDepthTest));
		const FVector AnchorBefore = Bar->GetComponentLocation();
		const FTransform PawnBefore = Enemy->GetActorTransform();
		const uint32 ContentBefore = Bar->GetContentRevision();
		for (const FVector ViewOffset : {FVector(800.f, 500.f, 200.f), FVector(-800.f, -500.f, 100.f)})
		{
			const FVector View = AnchorBefore + ViewOffset;
			Bar->UpdateForView(View);
			TestTrue(TEXT("Each local view faces the health surface toward its own camera"),
				FVector::DotProduct(Bar->GetForwardVector(), ViewOffset.GetSafeNormal()) > .999f);
			TestTrue(TEXT("Camera motion cannot move the anchor or turn the gameplay pawn"),
				Bar->GetComponentLocation().Equals(AnchorBefore, .01f) && Enemy->GetActorTransform().Equals(PawnBefore));
		}
		TestEqual(TEXT("Facing-only changes do not request new health content"), Bar->GetContentRevision(), ContentBefore);
		Enemy->GetMesh()->AddLocalOffset(FVector(35.f, -20.f, 10.f));
		TestTrue(TEXT("Network smoothing moves the bar with the body"), Bar->GetComponentLocation().Equals(
			Enemy->GetMesh()->GetComponentTransform().TransformPosition(Bar->GetRelativeLocation()), .01f)
			&& !Bar->GetComponentLocation().Equals(AnchorBefore, .01f));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopWorldHealthEventsTest, "Multiplayer.Combat.WorldHealth.EventsAndCleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopWorldHealthEventsTest::RunTest(const FString&)
{
	FWorldHealthFixture Fixture;
	UClass* Blueprint = EnemyHealthBlueprint();
	if (!TestNotNull(TEXT("Real saved minion BP loads"), Blueprint)) return false;
	auto* Enemy = Fixture.Spawn(Blueprint);
	if (!TestNotNull(TEXT("Real BP enemy spawns"), Enemy)) return false;
	auto* Bar = Enemy->FindComponentByClass<UmultiplayerEnemyHealthWidgetComponent>();
	auto* Outer = Cast<UmultiplayerEnemyHealthWidget>(Bar->GetUserWidgetObject());
	if (!TestNotNull(TEXT("Production code creates the existing source health layout"), Outer)) return false;
	TSharedPtr<SWidget> Slate = Outer->TakeWidget();
	auto* Inner = Cast<UmultiplayerEnemyHealthWidget>(Outer->GetWidgetFromName(TEXT("Health")));
	auto* Progress = Inner ? Cast<UProgressBar>(Inner->GetWidgetFromName(TEXT("HealthBar"))) : nullptr;
	if (!TestNotNull(TEXT("Original nested health bar remains intact"), Progress)) return false;
	auto* ASC = Enemy->GetAbilitySystemComponent();
	const uint32 Before = Bar->GetContentRevision();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 3.f);
	TestEqual(TEXT("Energy-only events do not rewrite health content"), Bar->GetContentRevision(), Before);
	Bar->UpdateForView(Bar->GetComponentLocation() + FVector(4000.f, 0.f, 0.f));
	TestTrue(TEXT("Distant health geometry is hidden without mutating combat state"), Bar->bHiddenInGame && Enemy->IsAlive());
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 37.f);
	TestEqual(TEXT("One actual health change creates one content revision"), Bar->GetContentRevision(), Before + 1);
	Bar->UpdateForView(Bar->GetComponentLocation() + FVector(300.f, 0.f, 0.f));
	TestTrue(TEXT("Coming back in range displays the latest health snapshot"), !Bar->bHiddenInGame
		&& FMath::IsNearlyEqual(Progress->GetPercent(), 37.f / Enemy->GetMaxHealth()));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 0.f);
	TestTrue(TEXT("Death hides the surface and disables its component work immediately"), Bar->bHiddenInGame && !Bar->IsComponentTickEnabled());
	TestNull(TEXT("Death releases the actual widget instance"), Bar->GetUserWidgetObject());
	TestFalse(TEXT("Death removes the nested health subscriber"), Enemy->OnCombatStatsChanged.GetAllObjects().Contains(Inner));
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxEnergyAttribute(), 120.f);
	Bar->UpdateForView(Enemy->GetActorLocation());
	TestNull(TEXT("Late stats and camera changes cannot recreate a dead health widget"), Bar->GetUserWidgetObject());
	Enemy->Destroy();
	TestNull(TEXT("EndPlay leaves no component widget"), Bar->GetUserWidgetObject());
	Slate.Reset();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopWorldHealthDedicatedTest, "Multiplayer.Combat.WorldHealth.DedicatedServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopWorldHealthDedicatedTest::RunTest(const FString&)
{
	FWorldHealthFixture Fixture(NM_DedicatedServer);
	if (!TestTrue(TEXT("Fixture enters an actual DS world branch"), Fixture.World->GetNetMode() == NM_DedicatedServer)) return false;
	UClass* Blueprint = EnemyHealthBlueprint();
	if (!TestNotNull(TEXT("DS loads the actual saved minion BP"), Blueprint)) return false;
	auto* Enemy = Fixture.Spawn(Blueprint);
	if (!TestNotNull(TEXT("DS spawns its authoritative enemy"), Enemy)) return false;
	auto* Bar = Enemy->FindComponentByClass<UmultiplayerEnemyHealthWidgetComponent>();
	TestNull(TEXT("DS never creates the BP health UserWidget"), Bar->GetUserWidgetObject());
	TestTrue(TEXT("DS health display stays hidden and does not tick"), Bar->bHiddenInGame && !Bar->IsComponentTickEnabled());
	Bar->InitWidget();
	TestNull(TEXT("A later initialization request cannot create DS UI"), Bar->GetUserWidgetObject());
	return !HasAnyErrors();
}

#endif
