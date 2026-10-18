// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatTargeting.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Core/multiplayerGameMode.h"
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "Player/multiplayerCharacter.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
	// 单世界只验生产查询与物理命中；联机结论仍需三进程两条独立连接的回执。
	struct FCombatTargetingWorld
	{
		UWorld* World = nullptr;
		ULocalPlayer* InputPlayer = nullptr;
		TFunction<void()> AfterTick;
		uint64 PreviousFrame = GFrameCounter;

		explicit FCombatTargetingWorld(bool bGround = true, bool bInput = false)
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			if (bInput)
			{
				auto* Instance = NewObject<UGameInstance>(GEngine);
				World->SetGameInstance(Instance);
				Context.OwningGameInstance = Instance;
				World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
				World->SetGameMode(FURL());
			}
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			if (bGround) Box(FVector(0, 0, -10), FVector(5000, 5000, 10));
		}

		~FCombatTargetingWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			if (InputPlayer) InputPlayer->PlayerRemoved();
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 真实场景碰撞而非伪造 FHitResult，墙和地面均参与生产射线与扫掠。
		AActor* Box(const FVector& Center, const FVector& Extent)
		{
			auto* Actor = World->SpawnActor<AActor>();
			auto* Collision = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Collision);
			Collision->SetBoxExtent(Extent);
			Collision->SetCollisionProfileName(TEXT("BlockAll"));
			Collision->RegisterComponent();
			Actor->SetActorLocation(Center);
			return Actor;
		}

		AmultiplayerCombatCharacter* Player(const FVector& Location = FVector(0, 0, 100), bool bThirdPerson = false)
		{
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Character = World->SpawnActor<AmultiplayerCombatCharacter>(
				bThirdPerson ? AmultiplayerCharacter::StaticClass() : AmultiplayerCombatCharacter::StaticClass(),
				Location, FRotator::ZeroRotator, Spawn);
			if (Character)
			{
				auto* Controller = World->SpawnActor<APlayerController>();
				Controller->Possess(Character);
				Character->GetCharacterMovement()->DisableMovement();
			}
			return Character;
		}

		AmultiplayerCombatEnemy* Enemy(const FVector& Location)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Enemy = World->SpawnActorDeferred<AmultiplayerCombatEnemy>(AmultiplayerCombatEnemy::StaticClass(),
				Transform, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Enemy->AutoPossessAI = EAutoPossessAI::Disabled;
			Enemy->FinishSpawning(Transform);
			Enemy->GetCharacterMovement()->DisableMovement();
			return Enemy;
		}

		// 与正常登录相同的正式玩家蓝图、控制器、接纳及输入映射；不直接调用预览/确认函数。
		AmultiplayerCombatCharacter* AdmitInputPlayer()
		{
			auto* Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
			UClass* Class = LoadClass<AmultiplayerCombatCharacter>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
			if (!Mode || !Class) return nullptr;
			Mode->DefaultPawnClass = Class;
			auto* Start = World->SpawnActor<APlayerStart>(FVector(0, 0, 110), FRotator::ZeroRotator);
			auto* PC = World->SpawnActor<AmultiplayerCoopPlayerController>();
			if (!Start || !PC) return nullptr;
			InputPlayer = NewObject<ULocalPlayer>(GEngine);
			World->GetGameInstance()->AddLocalPlayer(InputPlayer, FPlatformUserId::CreateFromInternalId(0));
			PC->SetPlayer(InputPlayer);
			PC->StartSpot = Start;
			Mode->RestartPlayer(PC);
			if (auto* Input = InputPlayer->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
			{
				FModifyContextOptions Options;
				Options.bForceImmediately = true;
				Input->RequestRebuildControlMappings(Options);
			}
			return Cast<AmultiplayerCombatCharacter>(PC->GetPawn());
		}

		void Advance(float Seconds)
		{
			for (float Elapsed = 0; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.01f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				if (AfterTick) AfterTick();
				Elapsed += Step;
			}
		}
	};

	// 准备受伤候选也走实际伤害 GE，不直接写目标 Health 或伪造治疗回调。
	void TargetingDamage(AmultiplayerCombatCharacter* Target, float Amount)
	{
		auto* ASC = Target->GetAbilitySystemComponent();
		auto Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}

	bool PreviewHas(const FmultiplayerCombatTargetPreview& Preview, AmultiplayerCombatCharacter* Target)
	{
		return Preview.AffectedTargets.Contains(TWeakObjectPtr<AmultiplayerCombatCharacter>(Target));
	}

	enum class EAreaInputCase { Release, ConfirmThenRelease, CancelThenRelease, SwappedSlot };

	// 所有结算来自保存的 BP 经真实输入栈进入技能；准备阶段只放置目标/预载其正式特效资产。
	bool RunAreaInputCase(FAutomationTestBase& Test, EAreaInputCase Case, bool bRequireParticles = false)
	{
		FCombatTargetingWorld Fixture(true, true);
		auto* Caster = Fixture.AdmitInputPlayer();
		if (!Test.TestNotNull(TEXT("Saved production combat player was admitted"), Caster)) return false;
		auto* PC = Cast<APlayerController>(Caster->GetController());
		if (!Test.TestNotNull(TEXT("Real local EnhancedPlayerInput is active"), Cast<UEnhancedPlayerInput>(PC->PlayerInput))
			|| !Test.TestNotNull(TEXT("Production pawn input bindings are installed"), Caster->InputComponent.Get())) return false;
		Fixture.Advance(.3f);
		if (!Test.TestTrue(TEXT("Normal admission permits combat without test-side input unlocking"), Caster->IsCombatReady())) return false;
		PC->SetControlRotation(FRotator(-20, 0, 0));
		Fixture.Advance(.02f);
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
		if (!Test.TestTrue(TEXT("Production third-person camera finds a real floor target"), Preview.bValid)) return false;
		auto* Target = Fixture.Enemy(Preview.End + FVector(0, 0, 100));
		const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
		const auto& Feedback = Caster->GetCombatConfig().AreaFeedback;
		UParticleSystem* Cascade = Feedback.CascadeEffect.IsNull() ? nullptr : Feedback.CascadeEffect.LoadSynchronous();
		UNiagaraSystem* Niagara = Cascade || Feedback.Effect.IsNull() ? nullptr : Feedback.Effect.LoadSynchronous();
		if (!Test.TestTrue(TEXT("Formal Area config names a loadable effect, not merely an optional file"), Cascade || Niagara)) return false;
		TSet<TWeakObjectPtr<USceneComponent>> AreaEffects;
		bool bWrongEffectLocation = false;
		Fixture.AfterTick = [&]
		{
			const auto Observe = [&](USceneComponent* Component)
			{
				if (!Component->IsRegistered() || Component->GetWorld() != Fixture.World) return;
				AreaEffects.Add(Component);
				bWrongEffectLocation |= !Component->GetComponentLocation().Equals(Preview.End, 1.f);
			};
			for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
				if (Cascade && It->Template == Cascade) Observe(*It);
			for (TObjectIterator<UNiagaraComponent> It; It; ++It)
				if (Niagara && It->GetAsset() == Niagara) Observe(*It);
		};
		int32 DamageTransitions = 0, CostTransitions = 0;
		const auto DamageHandle = Target->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Change)
			{ if (Change.NewValue < Change.OldValue) ++DamageTransitions; });
		const auto CostHandle = Caster->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerCombatAttributes::GetEnergyAttribute()).AddLambda([&](const FOnAttributeChangeData& Change)
			{ if (Change.NewValue < Change.OldValue) ++CostTransitions; });
		FKey Key = EKeys::R;
		if (Case == EAreaInputCase::SwappedSlot)
		{
			Caster->GetProgressionComponent()->RequestEquipSkill(1, EMultiplayerCombatSkill::Area);
			Test.TestEqual(TEXT("Real loadout request puts Area in Q slot"), Caster->GetProgressionComponent()->GetSlotSkill(1), EMultiplayerCombatSkill::Area);
			Key = EKeys::Q;
		}
		const float Energy = Caster->GetEnergy();
		const auto Input = [&](const FKey& InputKey, EInputEvent Event)
		{
			PC->InputKey(FInputKeyParams(InputKey, Event, Event == IE_Pressed ? 1. : 0.));
			Fixture.Advance(.02f);
		};
		Input(Key, IE_Pressed);
		Test.TestTrue(TEXT("Pressed mapped key enters Area targeting through real PlayerInput"),
			PC->IsInputKeyDown(Key) && Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Area);
		Test.TestTrue(TEXT("Holding preview starts no ability, cost or cooldown"),
			!Caster->IsCasting() && Caster->GetEnergy() == Energy && Caster->GetAreaCooldownRemaining() == 0 && CostTransitions == 0);
		if (Case == EAreaInputCase::ConfirmThenRelease) Input(EKeys::LeftMouseButton, IE_Pressed);
		if (Case == EAreaInputCase::CancelThenRelease) Input(EKeys::RightMouseButton, IE_Pressed);
		Input(Key, IE_Released);
		const bool bCancel = Case == EAreaInputCase::CancelThenRelease;
		Test.TestFalse(TEXT("Release always leaves editable Area targeting"), Caster->IsTargetingSkill());
		Test.TestEqual(TEXT("Cancel then release cannot commit; otherwise exactly one resource commitment"), CostTransitions, bCancel ? 0 : 1);
		Test.TestEqual(TEXT("Energy after real release agrees with cancel/confirm semantics"), Caster->GetEnergy(), Energy - (bCancel ? 0 : Rule.Cost));
		if (!bCancel) Test.TestTrue(TEXT("A normal key release actually starts GAS casting and cooldown"), Caster->IsCasting() && Caster->GetAreaCooldownRemaining() > 0);
		else Test.TestTrue(TEXT("RMB cancel then key-up cannot create an active ability or cooldown"), !Caster->IsCasting() && Caster->GetAreaCooldownRemaining() == 0);
		if (Case == EAreaInputCase::ConfirmThenRelease) Input(EKeys::LeftMouseButton, IE_Released);
		if (bCancel) Input(EKeys::RightMouseButton, IE_Released);
		Fixture.Advance(Rule.Windup + Rule.Recovery + .2f);
		// 超过原冷却再发送一个无对应按下的松开，排除仅靠 CD 偶然挡住重复施法。
		Fixture.Advance(Rule.Cooldown + .1f);
		Input(Key, IE_Released);
		Fixture.Advance(Rule.Windup + Rule.Recovery + .2f);
		Test.TestEqual(TEXT("One real input sequence cannot produce duplicate damage transitions"), DamageTransitions, bCancel ? 0 : 1);
		Test.TestEqual(TEXT("Final target HP matches exactly one configured hit or clean preview cancellation"), Target->GetHealth(), 100.f - (bCancel ? 0 : Rule.Amount));
		Test.TestEqual(TEXT("Even after cooldown expiry no stale key-up can repeat resource consumption"), CostTransitions, bCancel ? 0 : 1);
		Test.TestFalse(TEXT("No input sequence leaves casting active"), Caster->IsCasting());
		if (bRequireParticles)
		{
			Test.TestEqual(TEXT("Real Area execution produces exactly one matching particle component"), AreaEffects.Num(), bCancel ? 0 : 1);
			Test.TestFalse(TEXT("Real Area particle spawns at the same confirmed ground center, not the caster"), bWrongEffectLocation);
		}
		Fixture.AfterTick = nullptr;
		Target->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(DamageHandle);
		Caster->GetAbilitySystemComponent()->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(CostHandle);
		return !Test.HasAnyErrors();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaGroundPreviewTest, "Multiplayer.Combat.Targeting.AreaGroundPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 俯视改变的是地面落点，而非仍固定射程；预览与旧入口读取同一几何结果。
bool FCoopCombatAreaGroundPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player(FVector(0, 0, 150), true);
	if (!TestNotNull(TEXT("Third-person caster"), Caster)) return false;
	const FRotator Down(-35, 25, 0);
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Down);
	TestTrue(TEXT("Real walkable ground produces valid area preview"), Preview.bValid);
	TestTrue(TEXT("Preview lies on traced floor rather than the actor plane"), FMath::Abs(Preview.End.Z) < 1);
	TestTrue(TEXT("Preview normal matches actual ground"), Preview.Normal.Z > .99f);
	TestTrue(TEXT("Aim-down can select a point strictly before maximum horizontal range"),
		FVector::Dist2D(Caster->GetActorLocation(), Preview.End) < Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Range - 5);
	TestEqual(TEXT("Displayed radius equals configured authoritative radius"), Preview.Radius, Caster->GetCombatConfig().AreaRadius);
	FVector Normal;
	TestTrue(TEXT("Ability landing entry and preview have identical center"),
		Caster->GetAreaTarget(&Normal, &Down).Equals(Preview.End, .01f));
	const FRotator Flat(0, 25, 0);
	const auto Far = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Flat);
	TestTrue(TEXT("Flat camera uses a reachable ground fallback"), Far.bValid);
	TestTrue(TEXT("Camera setback cannot extend the configured horizontal cast range"),
		FVector::Dist2D(Caster->GetActorLocation(), Far.End) <= Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Range + .1f);
	const FRotator Sky(65, 25, 0);
	TestFalse(TEXT("A camera aimed at empty sky does not invent an offscreen valid ground cast"),
		Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Sky).bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaPreviewNoGroundTest, "Multiplayer.Combat.Targeting.AreaRequiresGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 无地面不能显示一个可释放的假圈，也不能提交消耗再生成悬空伤害。
bool FCoopCombatAreaPreviewNoGroundTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(false);
	auto* Caster = Fixture.Player(FVector(0, 0, 4000));
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	TestFalse(TEXT("Void has no valid ground target"), Preview.bValid);
	const float Energy = Caster->GetEnergy();
	Caster->CastArea();
	TestFalse(TEXT("Void area cannot enter casting"), Caster->IsCasting());
	TestEqual(TEXT("Invalid target does not consume energy"), Caster->GetEnergy(), Energy);
	TestEqual(TEXT("Invalid target does not start cooldown"), Caster->GetAreaCooldownRemaining(), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaBoundaryPreviewTest, "Multiplayer.Combat.Targeting.AreaBoundaryAndOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 圈内、圈外、隔墙、友军、楼上及重复组件同时存在，预览名单必须等于实际伤害名单。
bool FCoopCombatAreaBoundaryPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	const FRotator Aim(-20, 0, 0);
	const auto Empty = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Aim);
	if (!TestTrue(TEXT("Fixture has a real landing point"), Empty.bValid)) return false;
	const float Radius = Empty.Radius;
	const FVector Center = Empty.End;
	auto* Inside = Fixture.Enemy(Center + FVector(Radius - 1, 0, 100));
	auto* Outside = Fixture.Enemy(Center + FVector(0, -Radius - 1, 100));
	auto* Blocked = Fixture.Enemy(Center + FVector(0, 150, 100));
	auto* Upstairs = Fixture.Enemy(Center + FVector(-50, 0, 800));
	auto* Friendly = Fixture.Player(Center + FVector(-120, -100, 100));
	Fixture.Box(Center + FVector(0, 75, 100), FVector(30, 8, 100));
	auto* ExtraOverlap = NewObject<UCapsuleComponent>(Inside);
	ExtraOverlap->SetupAttachment(Inside->GetRootComponent());
	ExtraOverlap->InitCapsuleSize(35, 90);
	ExtraOverlap->SetCollisionObjectType(ECC_Pawn);
	ExtraOverlap->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ExtraOverlap->SetCollisionResponseToAllChannels(ECR_Overlap);
	ExtraOverlap->RegisterComponent();
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area, &Aim);
	TestTrue(TEXT("Enemy center just inside the ground circle is included despite capsule height"), PreviewHas(Preview, Inside));
	TestFalse(TEXT("Outside center is excluded even if the capsule overlaps the circle edge"), PreviewHas(Preview, Outside));
	TestFalse(TEXT("Occluded enemy is excluded from targeting highlight"), PreviewHas(Preview, Blocked));
	TestFalse(TEXT("A different floor is not part of the displayed ground circle"), PreviewHas(Preview, Upstairs));
	TestFalse(TEXT("Friendly inside circle is not a damage target"), PreviewHas(Preview, Friendly));
	TestEqual(TEXT("Two components cannot duplicate a highlighted target"), Preview.AffectedTargets.Num(), 1);
	Caster->ResolveArea(Preview.End);
	TestEqual(TEXT("Inside target receives exactly one authoritative hit"), Inside->GetHealth(), 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Amount);
	for (auto* Unaffected : { static_cast<AmultiplayerCombatCharacter*>(Outside), static_cast<AmultiplayerCombatCharacter*>(Blocked),
		static_cast<AmultiplayerCombatCharacter*>(Upstairs), Friendly })
		TestEqual(TEXT("Target absent from preview remains undamaged"), Unaffected->GetHealth(), 100.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileConvergenceTest, "Multiplayer.Combat.Targeting.NearProjectileConvergence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实第三人称相机斜俯视近身目标，比较显示终点、实际弹道和最终物理命中，而非只比较两个数学函数。
bool FCoopCombatProjectileConvergenceTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(false);
	auto* Caster = Fixture.Player(FVector(0, 0, 4000), true);
	const FRotator Aim(-24, 32, 0);
	const FVector ViewOrigin = Caster->GetCombatViewOrigin(Aim);
	auto* Target = Fixture.Enemy(ViewOrigin + Aim.Vector() * 700.f);
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
	TestTrue(TEXT("Near hostile capsule is the reticle target"), Preview.Target.Get() == Target);
	TestTrue(TEXT("Displayed projectile starts at authoritative muzzle, not camera"), Preview.Origin.Equals(Caster->GetCombatMuzzleLocation(), .01f));
	TestTrue(TEXT("Muzzle and camera differ in this actual third-person setup"), FVector::Distance(ViewOrigin, Preview.Origin) > 100);
	if (!TestTrue(TEXT("Production firing creates a real projectile"), Caster->FireProjectile(25, 1600, &Aim))) return false;
	AmultiplayerCombatProjectile* Projectile = nullptr;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) Projectile = *It;
	if (!TestNotNull(TEXT("Real projectile exists before physics"), Projectile)) return false;
	const FVector Expected = (Preview.End - Preview.Origin).GetSafeNormal();
	TestTrue(TEXT("Launch velocity converges to reticle hit, not parallel camera forward"),
		FVector(Projectile->GetLaunchState().Velocity).GetSafeNormal().Equals(Expected, .001f));
	Fixture.Advance(.8f);
	TestEqual(TEXT("Displayed near target is hit exactly once by real projectile sweep"), Target->GetHealth(), 75.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileWallPreviewTest, "Multiplayer.Combat.Targeting.ProjectileWallOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 准星能看见目标也不能让枪口跨过近墙；预览与真实弹体都由同一墙体截停。
bool FCoopCombatProjectileWallPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(false);
	auto* Caster = Fixture.Player(FVector(0, 0, 4000));
	auto* Target = Fixture.Enemy(FVector(400, 0, 4040));
	Fixture.Box(FVector(100, 0, 4040), FVector(10, 100, 150));
	const FRotator Aim = FRotator::ZeroRotator;
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
	TestTrue(TEXT("Displayed trajectory stops at near wall"), Preview.End.X < 115.f);
	TestTrue(TEXT("Occluded enemy is not highlighted as a hit"), Preview.Target.Get() != Target);
	Caster->FireProjectile(25, 1600, &Aim);
	Fixture.Advance(.6f);
	TestEqual(TEXT("Authoritative projectile cannot damage through the wall"), Target->GetHealth(), 100.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHealPreviewTest, "Multiplayer.Combat.Targeting.HealSelectionAndOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 目标提示与真正治疗共用血量比例、距离和遮挡选择，不显示治疗不到的队友。
bool FCoopCombatHealPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	auto* Near = Fixture.Player(FVector(150, 100, 100));
	auto* Selected = Fixture.Player(FVector(350, 100, 100));
	auto* Occluded = Fixture.Player(FVector(0, 350, 100));
	auto* TooFar = Fixture.Player(FVector(900, -100, 100));
	auto* Hostile = Fixture.Enemy(FVector(150, -100, 100));
	TargetingDamage(Near, 20);
	TargetingDamage(Selected, 60);
	TargetingDamage(Occluded, 90);
	TargetingDamage(TooFar, 95);
	TargetingDamage(Hostile, 95);
	Fixture.Box(FVector(0, 175, 100), FVector(60, 10, 200));
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Heal);
	TestTrue(TEXT("Preview chooses lowest visible friendly health fraction within range"), Preview.Target.Get() == Selected);
	TestTrue(TEXT("Public selector and displayed selector are identical"), Caster->GetHealTarget() == Preview.Target.Get());
	Caster->Heal();
	TestEqual(TEXT("Exactly the highlighted ally receives healing"), Selected->GetHealth(), 70.f);
	TestEqual(TEXT("Closer but healthier ally is not healed"), Near->GetHealth(), 80.f);
	TestEqual(TEXT("More wounded ally behind wall is not healed"), Occluded->GetHealth(), 10.f);
	TestEqual(TEXT("Out-of-range wounded ally is not healed"), TooFar->GetHealth(), 5.f);
	TestEqual(TEXT("Enemy never becomes a healing candidate"), Hostile->GetHealth(), 5.f);
	TestEqual(TEXT("Valid healing consumes one configured cost"), Caster->GetEnergy(), 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Heal).Cost);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashWallPreviewTest, "Multiplayer.Combat.Targeting.DashCapsuleWallPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 冲刺用角色完整胶囊而非脚底射线，终点预览必须留出墙前的碰撞空间。
bool FCoopCombatDashWallPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	Fixture.Box(FVector(250, 0, 100), FVector(10, 200, 150));
	const FRotator Aim(0, 0, 0);
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Dash, &Aim);
	const float CapsuleRadius = Caster->GetCapsuleComponent()->GetScaledCapsuleRadius();
	TestTrue(TEXT("Wall does not invalidate a shorter reachable dash"), Preview.bValid);
	TestTrue(TEXT("Preview endpoint keeps whole capsule on the near side of wall"), Preview.End.X <= 240.f - CapsuleRadius + 1.f);
	TestTrue(TEXT("Wall truncates nominal dash range"), Preview.End.X < Caster->GetSkillRule(EMultiplayerCombatSkill::Dash).Range - 50.f);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatTargetingDashEndpoint), false, Caster);
	TestFalse(TEXT("Displayed endpoint has no blocking geometry penetrating the capsule"),
		Fixture.World->OverlapBlockingTestByChannel(Preview.End + FVector(0, 0, 1), FQuat::Identity, ECC_Pawn,
			FCollisionShape::MakeCapsule(CapsuleRadius - .5f, Caster->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - .5f), Query));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDefensePreviewFacingTest, "Multiplayer.Combat.Targeting.DefenseDirectionSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 原地转镜头不会先转身体；Q确认后防御身体朝向必须与先前显示的方向一致。
bool FCoopCombatDefensePreviewFacingTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	Caster->SetActorRotation(FRotator(0, -110, 0));
	Caster->GetController()->SetControlRotation(FRotator(-20, 73, 0));
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Defend);
	const FVector ShownDirection = (Preview.End - Preview.Origin).GetSafeNormal2D();
	Caster->Defend();
	TestTrue(TEXT("Defense actually becomes active"), Caster->IsDefending());
	TestTrue(TEXT("Authoritative defense facing equals displayed direction, not previous body yaw"),
		Caster->GetActorForwardVector().Equals(ShownDirection, .001f));
	TestEqual(TEXT("Facing change does not apply pitch to character body"), Caster->GetActorRotation().Pitch, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTargetingConfirmationTest, "Multiplayer.Combat.Targeting.ConfirmCancelAndLockedCenter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 预览不扣费，取消不放技能；确认后起手转镜头不再把已显示的本轮落点偷偷改走。
bool FCoopCombatTargetingConfirmationTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	Caster->BeginSkillTargeting(EMultiplayerCombatSkill::Area);
	TestTrue(TEXT("Owned player can enter targeting before committing"), Caster->IsTargetingSkill());
	TestFalse(TEXT("Preview is not an active casting ability"), Caster->IsCasting());
	TestEqual(TEXT("Preview does not consume energy"), Caster->GetEnergy(), 100.f);
	Caster->CancelSkillTargeting();
	TestFalse(TEXT("Preview cancellation clears targeting state"), Caster->IsTargetingSkill());
	TestEqual(TEXT("Preview cancellation does not start cooldown"), Caster->GetAreaCooldownRemaining(), 0.f);
	Caster->BeginSkillTargeting(EMultiplayerCombatSkill::Area);
	const auto Before = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	auto* Target = Fixture.Enemy(Before.End + FVector(0, 0, 100));
	Caster->ConfirmSkillTargeting();
	TestFalse(TEXT("Confirmation exits editable preview"), Caster->IsTargetingSkill());
	TestTrue(TEXT("Confirmation starts the real GAS casting lifecycle"), Caster->IsCasting());
	TestEqual(TEXT("Confirmation commits energy once"), Caster->GetEnergy(), 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Cost);
	Caster->GetController()->SetControlRotation(FRotator(-30, 120, 0));
	const auto During = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	TestTrue(TEXT("Casting circle remains at confirmed center after mouse turns"), During.End.Equals(Before.End, .01f));
	TestEqual(TEXT("Windup has not resolved early damage"), Target->GetHealth(), 100.f);
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	Fixture.Advance(Rule.Windup + Rule.Recovery + .2f);
	TestEqual(TEXT("Actual release hits the target at shown center, once"), Target->GetHealth(), 100.f - Rule.Amount);
	TestFalse(TEXT("Completed cast leaves no casting state"), Caster->IsCasting());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDashActualEndpointTest, "Multiplayer.Combat.Targeting.DashActualWallEndpoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 正墙和斜墙都推进真实 CMC/RootMotionSource，不能只验证预览自己的扫掠函数。
bool FCoopCombatDashActualEndpointTest::RunTest(const FString& Parameters)
{
	for (const float WallYaw : { 0.f, 35.f })
	{
		FCombatTargetingWorld Fixture;
		auto* Caster = Fixture.Player();
		auto* Movement = Caster->GetCharacterMovement();
		Movement->bRunPhysicsWithNoController = true;
		Movement->SetMovementMode(MOVE_Walking);
		Fixture.Advance(.15f);
		auto* Wall = Fixture.Box(FVector(250, 0, 100), FVector(10, 500, 150));
		Wall->SetActorRotation(FRotator(0, WallYaw, 0));
		Caster->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
		Caster->GetController()->SetControlRotation(FRotator::ZeroRotator);
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Dash);
		if (!TestTrue(TEXT("Actual dash fixture has a reachable truncated endpoint"), Preview.bValid)) return false;
		Caster->Dash();
		TestTrue(TEXT("Dash activation installs the real GAS dashing state"),
			Caster->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing));
		Fixture.Advance(Caster->GetSkillRule(EMultiplayerCombatSkill::Dash).Duration + .25f);
		TestTrue(*FString::Printf(TEXT("Wall %.0f degrees: real CMC endpoint matches the preview within one simulation step"), WallYaw),
			FVector::Dist2D(Caster->GetActorLocation(), Preview.End) <= 6.f);
		TestFalse(TEXT("Completed dash releases root-motion ability state"),
			Caster->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Dashing));
		TestTrue(TEXT("Dash completion leaves no residual velocity"), Movement->Velocity.Size2D() < 1.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileConfiguredRangeTest, "Multiplayer.Combat.Targeting.ConfiguredProjectileRange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 配置短射程不只缩短一根 UI 线：真实弹体寿命也必须在该距离截止。
bool FCoopCombatProjectileConfiguredRangeTest::RunTest(const FString& Parameters)
{
	for (const bool bInside : { true, false })
	{
		FCombatTargetingWorld Fixture(false);
		auto* Caster = Fixture.Player(FVector(0, 0, 4000), true);
		auto* Config = DuplicateObject<UmultiplayerCombatConfig>(&Caster->GetCombatConfig(), Caster);
		Config->Attack.Range = 200.f;
		Config->ProjectileSpeed = 1000.f;
		const auto* Property = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("CombatConfig"));
		if (!TestNotNull(TEXT("Existing configurable combat asset property"), Property)) return false;
		Property->SetObjectPropertyValue_InContainer(Caster, Config);
		auto* Target = Fixture.Enemy(FVector(bInside ? 150.f : 400.f, 0, 4000));
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack);
		TestTrue(TEXT("Visible targeting length never exceeds configured projectile range"),
			FVector::Distance(Preview.Origin, Preview.End) <= 200.1f);
		TestEqual(TEXT("Reticle highlight respects actual configured distance"), Preview.Target.Get() == Target, bInside);
		if (!bInside)
			TestTrue(TEXT("Third-person camera setback does not shorten unobstructed nominal range"),
				FMath::IsNearlyEqual(FVector::Distance(Preview.Origin, Preview.End), 200.f, .1f));
		Caster->FireProjectile(25, Config->ProjectileSpeed);
		Fixture.Advance(1.f);
		TestEqual(TEXT("Only a target inside configured range takes real swept projectile damage"), Target->GetHealth(), bInside ? 75.f : 100.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaReleaseInputTest, "Multiplayer.Combat.Targeting.AreaReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopCombatAreaReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::Release);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaConfirmReleaseInputTest, "Multiplayer.Combat.Targeting.AreaConfirmThenReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopCombatAreaConfirmReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::ConfirmThenRelease);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaCancelReleaseInputTest, "Multiplayer.Combat.Targeting.AreaCancelThenReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopCombatAreaCancelReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::CancelThenRelease);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaSlotReleaseInputTest, "Multiplayer.Combat.Targeting.AreaSwappedSlotReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCoopCombatAreaSlotReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::SwappedSlot);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaReleaseParticleTest, "Multiplayer.Combat.Targeting.AreaReleaseInputParticle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
bool FCoopCombatAreaReleaseParticleTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::Release, true);
}

#endif
