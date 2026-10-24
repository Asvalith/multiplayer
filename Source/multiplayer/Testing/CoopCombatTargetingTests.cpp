// Copyright Epic Games, Inc. All Rights Reserved.

// 回归预览几何与实际命中的一致性、真实按键确认／取消以及通知重入后的技能切换。
// 单世界使用正式查询、输入和碰撞；双端权威一致性仍由网络场景验证。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Testing/CoopCombatRelationshipTestProbe.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimMontage.h"
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
#include "GameplayCueManager.h"
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
#include "UObject/StrongObjectPtr.h"

namespace
{
	// 单世界只验生产查询与物理命中；联机结论仍需三进程两条独立连接的回执。
	struct FCombatTargetingWorld
	{
		UWorld* World = nullptr;
		ULocalPlayer* InputPlayer = nullptr;
		TFunction<void()> AfterTick;
		uint64 PreviousFrame = GFrameCounter;

		// 按用例准备真实地面和输入所需 GameMode，未提供地面时保留正式落点拒绝。
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

		// 正常结束角色与输入玩家，销毁世界并恢复帧号，避免残留确认或动画任务。
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

		// 创建真实控制角色，按需选择第三人称类以验证镜头退距不改变技能有效射程。
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

		// 生成关闭 AI 与移动的真实靶敌，让预览集合及实际伤害只由被测技能改变。
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

		// 小步推进真实输入、技能与弹体，可在每帧之后采样实际预览或粒子状态。
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

	// 查询预览实际收集的弱目标集合，不重新计算另一套距离或阵营规则。
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
	UmultiplayerAreaAbility::ResolveTargets(*Caster, Preview.End);
	TestEqual(TEXT("Inside target receives exactly one authoritative hit"), Inside->GetHealth(), 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Amount);
	for (auto* Unaffected : { static_cast<AmultiplayerCombatCharacter*>(Outside), static_cast<AmultiplayerCombatCharacter*>(Blocked),
		static_cast<AmultiplayerCombatCharacter*>(Upstairs), Friendly })
		TestEqual(TEXT("Target absent from preview remains undamaged"), Unaffected->GetHealth(), 100.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileConvergenceTest, "Multiplayer.Combat.Targeting.PlayerForwardProjectile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 镜头俯仰不决定普攻高度；实际技能转身、发射、Cue和物理命中都采用同一次水平出手方向。
bool FCoopCombatProjectileConvergenceTest::RunTest(const FString& Parameters)
{
	auto* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
#if WITH_EDITOR
	CueManager->LoadNotifyForEditorPreview(CombatTags::Cue_Attack);
#endif
	for (const float Pitch : { -55.f, -24.f, 0.f, 30.f, 60.f })
	{
		FCombatTargetingWorld Fixture(false);
		auto* Caster = Fixture.Player(FVector(0, 0, 4000), true);
		Caster->SetActorRotation(FRotator(0, -110, 0));
		const FRotator Aim(Pitch, 32, 0);
		Caster->GetController()->SetControlRotation(Aim);
		const FVector Forward = FRotator(0, Aim.Yaw, 0).Vector();
		auto* Target = Fixture.Enemy(Caster->GetActorLocation() + Forward * 360.f);
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
		TestTrue(*FString::Printf(TEXT("Pitch %.0f: preview selects the target in horizontal attack direction"), Pitch), Preview.Target.Get() == Target);
		TestTrue(TEXT("Preview starts at the same physical muzzle"), Preview.Origin.Equals(Caster->GetCombatMuzzleLocation(&Aim), .01f));
		TestTrue(TEXT("Preview trajectory remains horizontal even when the camera looks up or down"),
			(Preview.End - Preview.Origin).GetSafeNormal().Equals(Forward, .001f));
		int32 AttackCues = 0;
		FVector CueDirection = FVector::ZeroVector, CueOrigin = FVector::ZeroVector;
		const auto Listener = CueManager->OnGameplayCueRouted().AddLambda(
			[&](AActor* Recipient, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters& Feedback, EGameplayCueExecutionOptions)
			{
				if (Recipient == Caster && Tag == CombatTags::Cue_Attack && Event == EGameplayCueEvent::Executed)
				{ ++AttackCues; CueDirection = Feedback.Normal; CueOrigin = Feedback.Location; }
			});
		Caster->Attack();
		CueManager->OnGameplayCueRouted().Remove(Listener);
		AmultiplayerCombatProjectile* Projectile = nullptr;
		for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) Projectile = *It;
		if (!TestNotNull(TEXT("Actual attack ability creates its authoritative projectile"), Projectile)) return false;
		TestTrue(TEXT("Accepted attack turns the body once to the submitted camera yaw"), Caster->GetActorForwardVector().Equals(Forward, .001f));
		TestTrue(TEXT("Actual launch follows the current attack body forward, without camera pitch"),
			FVector(Projectile->GetLaunchState().Velocity).GetSafeNormal().Equals(Caster->GetActorForwardVector(), .001f));
		TestTrue(TEXT("Actual launch origin equals preview origin"), FVector(Projectile->GetLaunchState().Origin).Equals(Preview.Origin, .1f));
		TestEqual(TEXT("One actual attack publishes one start Cue"), AttackCues, 1);
		TestTrue(TEXT("Start particle Cue receives the same horizontal direction and origin as the physical projectile"),
			CueDirection.Equals(Forward, .001f) && CueOrigin.Equals(Preview.Origin, .1f));
		Fixture.Advance(.8f);
		TestEqual(TEXT("Horizontal forward target receives one real swept hit"), Target->GetHealth(), 75.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatEnemyProjectilePitchTest, "Multiplayer.Combat.Targeting.EnemyProjectileKeepsVerticalAim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 水平前射只属于玩家普攻；敌人的显式三维目标方向继续进入原弹道查询。
bool FCoopCombatEnemyProjectilePitchTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(false);
	auto* Source = Fixture.Enemy(FVector(0, 0, 4000));
	const FRotator Aim(35, 37, 0);
	const auto Preview = Source->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
	const FVector Expected = (Preview.End - Preview.Origin).GetSafeNormal();
	TestTrue(TEXT("Enemy trajectory retains meaningful upward aim"), Expected.Z > .3f);
	if (!TestTrue(TEXT("Enemy production firing entry still accepts its three-dimensional snapshot"), AmultiplayerCombatProjectile::FireFromSkill(*Source, 25, 1600, &Aim))) return false;
	AmultiplayerCombatProjectile* Projectile = nullptr;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) Projectile = *It;
	if (!TestNotNull(TEXT("Enemy real projectile exists"), Projectile)) return false;
	TestTrue(TEXT("Enemy physical launch still matches its non-horizontal preview"),
		FVector(Projectile->GetLaunchState().Velocity).GetSafeNormal().Equals(Expected, .001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileMuzzleSnapshotTest, "Multiplayer.Combat.Targeting.ProjectileMuzzleAimSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 身体、镜头和本轮出手方向故意不同；前移起点只使用同一次瞄准快照，不依赖手部骨骼或异步视角。
bool FCoopCombatProjectileMuzzleSnapshotTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(false);
	auto* Caster = Fixture.Player(FVector(1300, 1400, 4000), true);
	Caster->SetActorRotation(FRotator(0, -110, 0));
	Caster->GetController()->SetControlRotation(FRotator(25, 170, 0));
	const FVector Chest = Caster->GetActorLocation() + FVector(0, 0, 40);
	const float ForwardDistance = Caster->GetCapsuleComponent()->GetScaledCapsuleRadius() + CombatTargeting::ProjectileRadius + 6.f;
	for (const float Yaw : { 0.f, 90.f, -73.f, 180.f })
	{
		const FRotator Aim(-35, Yaw, 0);
		const FVector Expected = Chest + FRotator(0, Yaw, 0).Vector() * ForwardDistance;
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
		TestTrue(*FString::Printf(TEXT("Yaw %.0f: explicit aim places muzzle ahead of capsule, independent of body and camera"), Yaw),
			Caster->GetCombatMuzzleLocation(&Aim).Equals(Expected, .01f));
		TestTrue(*FString::Printf(TEXT("Yaw %.0f: trajectory preview uses that exact physical muzzle"), Yaw),
			Preview.Origin.Equals(Expected, .01f));
	}
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr,
		TEXT("/Game/Combat/Presentation/ARPG/AM_ARPG_Attack.AM_ARPG_Attack"));
	const auto* MontageProperty = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("AttackMontage"));
	if (!TestNotNull(TEXT("Saved attack montage gives the real GAS attack an observable windup"), Montage)
		|| !TestNotNull(TEXT("Attack montage configuration exists"), MontageProperty)) return false;
	MontageProperty->SetObjectPropertyValue_InContainer(Caster, Montage);
	auto* Config = DuplicateObject<UmultiplayerCombatConfig>(&Caster->GetCombatConfig(), Caster);
	Config->Attack.Windup = .3f; Config->Attack.Recovery = .2f;
	const auto* ConfigProperty = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("CombatConfig"));
	if (!TestNotNull(TEXT("Attack configuration exists"), ConfigProperty)) return false;
	ConfigProperty->SetObjectPropertyValue_InContainer(Caster, Config);
	const FRotator SubmittedAim(15, 73, 0);
	Caster->GetController()->SetControlRotation(SubmittedAim);
	const auto Submitted = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &SubmittedAim);
	Caster->Attack();
	if (!TestTrue(TEXT("Actual GAS activation owns the attack-facing state"),
		Caster->GetAbilitySystemComponent()->HasMatchingGameplayTag(CombatTags::State_Attacking))) return false;
	Caster->GetController()->SetControlRotation(FRotator(-35, -120, 0));
	TestTrue(TEXT("Turning camera during windup does not move the locked muzzle"),
		Caster->GetCombatMuzzleLocation().Equals(Submitted.Origin, .01f));
	TestTrue(TEXT("Implicit active preview keeps the same locked muzzle"),
		Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack).Origin.Equals(Submitted.Origin, .01f));
	Fixture.Advance(.32f);
	AmultiplayerCombatProjectile* Projectile = nullptr;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) Projectile = *It;
	if (!TestNotNull(TEXT("Real delayed ability creates its authoritative projectile"), Projectile)) return false;
	TestTrue(TEXT("Authoritative launch snapshot starts at the submitted preview muzzle"),
		FVector(Projectile->GetLaunchState().Origin).Equals(Submitted.Origin, .1f));
	TestTrue(TEXT("Authoritative velocity uses the same submitted preview trajectory"),
		FVector(Projectile->GetLaunchState().Velocity).GetSafeNormal().Equals((Submitted.End - Submitted.Origin).GetSafeNormal(), .001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileMuzzleObstructionTest, "Multiplayer.Combat.Targeting.ProjectileMuzzleNearObstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 前移只改善空旷处的出手位置；身体前紧贴的墙和敌人仍必须由原始近距离扫掠处理。
bool FCoopCombatProjectileMuzzleObstructionTest::RunTest(const FString& Parameters)
{
	for (const bool bWall : { true, false })
	{
		FCombatTargetingWorld Fixture(false);
		auto* Caster = Fixture.Player(FVector(1000, 500, 4000));
		const FRotator Aim(0, 45, 0);
		const FVector Chest = Caster->GetActorLocation() + FVector(0, 0, 40);
		const FVector Offset = Aim.Vector() * 30.f;
		if (bWall) Fixture.Box(Chest + Offset, FVector(5, 5, 100));
		else Fixture.Enemy(Caster->GetActorLocation() + Offset);
		TestTrue(bWall ? TEXT("Near wall cannot be skipped by a forward muzzle") : TEXT("Overlapping enemy cannot be skipped by a forward muzzle"),
			Caster->GetCombatMuzzleLocation(&Aim).Equals(Chest, .01f));
		TestTrue(TEXT("Obstructed muzzle fallback remains identical in targeting preview"),
			Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim).Origin.Equals(Chest, .01f));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatProjectileWallPreviewTest, "Multiplayer.Combat.Targeting.ProjectileWallOcclusion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 准星能看见目标也不能让枪口跨过近墙；预览与真实弹体都由同一墙体截停。
bool FCoopCombatProjectileWallPreviewTest::RunTest(const FString& Parameters)
{
	for (const float Pitch : { -50.f, 0.f, 35.f })
	{
		FCombatTargetingWorld Fixture(false);
		auto* Caster = Fixture.Player(FVector(0, 0, 4000), true);
		auto* Target = Fixture.Enemy(FVector(400, 0, 4040));
		Fixture.Box(FVector(100, 0, 4040), FVector(10, 100, 150));
		const FRotator Aim(Pitch, 0, 0);
		const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Attack, &Aim);
		TestTrue(TEXT("Displayed horizontal trajectory stops at near wall regardless of camera pitch"), Preview.End.X < 115.f);
		TestTrue(TEXT("Occluded enemy is not highlighted as a hit"), Preview.Target.Get() != Target);
		TestTrue(TEXT("The wall-truncated preview still has no downward or upward component"), FMath::IsNearlyEqual(Preview.Origin.Z, Preview.End.Z, .01f));
		AmultiplayerCombatProjectile::FireFromSkill(*Caster, 25, 1600, &Aim);
		Fixture.Advance(.6f);
		TestEqual(TEXT("Authoritative projectile cannot damage through the wall"), Target->GetHealth(), 100.f);
	}
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

	// 第二次扣费通知使旧目标满血，生效阶段必须重新选择，而不是消费提交前的目标快照。
	auto* ASC = Caster->GetAbilitySystemComponent();
	ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(CombatTags::Cooldown_Heal));
	int32 CostNotifications = 0;
	const auto Listener = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute())
		.AddLambda([Selected, &CostNotifications](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue >= Change.OldValue) return;
			++CostNotifications;
			Selected->GetAbilitySystemComponent()->SetNumericAttributeBase(
				UmultiplayerCombatAttributes::GetHealthAttribute(), Selected->GetMaxHealth());
		});
	Caster->Heal();
	ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetEnergyAttribute()).Remove(Listener);
	TestEqual(TEXT("The second heal commits exactly one cost notification"), CostNotifications, 1);
	TestEqual(TEXT("Post-commit selection heals the next eligible ally"), Near->GetHealth(), 100.f);
	TestEqual(TEXT("The cost notification filled the previous target"), Selected->GetHealth(), 100.f);
	TestEqual(TEXT("Both heals consume only their own configured cost"), Caster->GetEnergy(),
		100.f - 2.f * Caster->GetSkillRule(EMultiplayerCombatSkill::Heal).Cost);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatReceivedEffectsFacingTest, "Multiplayer.Combat.Targeting.ReceivedEffectsPreserveFacing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 受击、范围控制和队友治疗只改变目标战斗状态，不把施法者或目标的镜头朝向写入目标身体。
bool FCoopCombatReceivedEffectsFacingTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	auto* Enemy = Fixture.Enemy(FVector(300, 0, 100));
	auto* Ally = Fixture.Player(FVector(300, -300, 100));
	if (!TestNotNull(TEXT("Effect source exists"), Caster) || !TestNotNull(TEXT("Damage receiver exists"), Enemy)
		|| !TestNotNull(TEXT("Healing receiver exists"), Ally)) return false;
	Caster->SetActorRotation(FRotator(0, -110, 0));
	Caster->GetController()->SetControlRotation(FRotator(-20, 73, 0));
	Enemy->SetActorRotation(FRotator(0, 145, 0));
	Ally->SetActorRotation(FRotator(0, -133, 0));
	Ally->GetController()->SetControlRotation(FRotator(15, 27, 0));
	const FRotator EnemyFacing = Enemy->GetActorRotation();
	const FRotator AllyFacing = Ally->GetActorRotation();
	TestTrue(TEXT("Server unified damage entry accepts the hostile"), Caster->ApplyCombatDamage(Enemy, 10.f));
	TestEqual(TEXT("The real damage GE changes health once"), Enemy->GetHealth(), 90.f);
	TestTrue(TEXT("Ordinary damage does not redirect the hostile toward the caster camera"),
		EnemyFacing.Equals(Enemy->GetActorRotation(), .001f));
	const FVector AreaCenter = Enemy->GetActorLocation() - FVector::UpVector * Enemy->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	UmultiplayerAreaAbility::ResolveTargets(*Caster, AreaCenter);
	TestEqual(TEXT("Real area settlement applies its configured damage"), Enemy->GetHealth(), 90.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Amount);
	TestTrue(TEXT("The area victim receives actual stun"), Enemy->IsStunned());
	TestTrue(TEXT("Area damage and control leave the hostile facing unchanged"), EnemyFacing.Equals(Enemy->GetActorRotation(), .001f));
	TargetingDamage(Ally, 20.f);
	Caster->Heal();
	TestEqual(TEXT("Actual healing ability restores the selected injured ally"), Ally->GetHealth(), 100.f);
	TestTrue(TEXT("Healing does not align its recipient with either free camera"), AllyFacing.Equals(Ally->GetActorRotation(), .001f));
	Caster->GetController()->SetControlRotation(FRotator(10, -45, 0));
	Ally->GetController()->SetControlRotation(FRotator(-20, 110, 0));
	Fixture.Advance(.05f);
	TestTrue(TEXT("Later camera movement still does not redirect the controlled hostile"), EnemyFacing.Equals(Enemy->GetActorRotation(), .001f));
	TestTrue(TEXT("Later camera movement still does not redirect the healed ally"), AllyFacing.Equals(Ally->GetActorRotation(), .001f));
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

// Q不把自由镜头方向写回身体；预览和短窗口弹反始终使用角色正面。
bool FCoopCombatDefensePreviewFacingTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	Caster->SetActorRotation(FRotator(0, -110, 0));
	Caster->GetController()->SetControlRotation(FRotator(-20, 73, 0));
	const FRotator BodyBefore = Caster->GetActorRotation();
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Defend);
	const FVector ShownDirection = (Preview.End - Preview.Origin).GetSafeNormal2D();
	TestTrue(TEXT("Defense preview follows the existing body heading, not the free camera"),
		ShownDirection.Equals(BodyBefore.Vector(), .001f));
	Caster->GetController()->SetControlRotation(FRotator(-35, 145, 0));
	const auto BeforeConfirmation = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Defend);
	TestTrue(TEXT("Turning the camera during Q preview leaves its direction unchanged"),
		(BeforeConfirmation.End - BeforeConfirmation.Origin).GetSafeNormal2D().Equals(ShownDirection, .001f));
	Caster->Defend();
	TestTrue(TEXT("Defense actually becomes active"), Caster->IsDefending());
	TestTrue(TEXT("Accepted Q preserves the existing body rotation"), BodyBefore.Equals(Caster->GetActorRotation(), .001f));
	TestTrue(TEXT("Authoritative frontal defense equals the displayed body direction"),
		Caster->GetActorForwardVector().Equals(ShownDirection, .001f));
	Caster->GetController()->SetControlRotation(FRotator(20, -40, 0));
	Fixture.Advance(.05f);
	const auto AfterLook = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Defend);
	TestTrue(TEXT("Turning the camera during the parry window does not rotate the body"),
		BodyBefore.Equals(Caster->GetActorRotation(), .001f));
	TestTrue(TEXT("Turning the camera after Q does not redirect its frontal preview"),
		(AfterLook.End - AfterLook.Origin).GetSafeNormal2D().Equals(ShownDirection, .001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTargetingConfirmationTest, "Multiplayer.Combat.Targeting.ConfirmCancelAndLockedCenter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 预览不扣费，取消不放技能；确认后起手转镜头不再把已显示的本轮落点偷偷改走。
bool FCoopCombatTargetingConfirmationTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	Caster->SetActorRotation(FRotator(0, -110, 0));
	const FRotator BodyBefore = Caster->GetActorRotation();
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
	TestTrue(TEXT("Accepted R locks the ground target without changing body rotation"),
		BodyBefore.Equals(Caster->GetActorRotation(), .001f));
	TestEqual(TEXT("Confirmation commits energy once"), Caster->GetEnergy(), 100.f - Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Cost);
	Caster->GetController()->SetControlRotation(FRotator(-30, 120, 0));
	const auto During = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	TestTrue(TEXT("Casting circle remains at confirmed center after mouse turns"), During.End.Equals(Before.End, .01f));
	TestTrue(TEXT("Turning the camera after R does not redirect the body"),
		BodyBefore.Equals(Caster->GetActorRotation(), .001f));
	TestEqual(TEXT("Windup has not resolved early damage"), Target->GetHealth(), 100.f);
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	Fixture.Advance(Rule.Windup + Rule.Recovery + .2f);
	TestEqual(TEXT("Actual release hits the target at shown center, once"), Target->GetHealth(), 100.f - Rule.Amount);
	TestFalse(TEXT("Completed cast leaves no casting state"), Caster->IsCasting());
	TestTrue(TEXT("Completed R does not leave a camera-aligned body rotation"),
		BodyBefore.Equals(Caster->GetActorRotation(), .001f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTargetConfirmReentrantCancelTest,
	"Multiplayer.Combat.Targeting.ConfirmNotificationCanCancelAcceptedCast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 预览关闭通知中的取消必须针对已接纳的本次施法，不能先取消空状态、返回后再偷偷启动旧请求。
bool FCoopCombatTargetConfirmReentrantCancelTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(true, true);
	auto* Caster = Fixture.AdmitInputPlayer();
	if (!TestNotNull(TEXT("Saved production player is admitted for real input"), Caster)) return false;
	auto* PC = Cast<APlayerController>(Caster->GetController());
	if (!TestTrue(TEXT("Production keyboard bindings and EnhancedPlayerInput exist"),
		PC && Cast<UEnhancedPlayerInput>(PC->PlayerInput) && Caster->InputComponent)) return false;
	Fixture.Advance(.3f);
	PC->SetControlRotation(FRotator(-20, 0, 0));
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	if (!TestTrue(TEXT("Real camera query finds the area ground target"), Preview.bValid)) return false;
	auto* Target = Fixture.Enemy(Preview.End + FVector(0, 0, 100));
	if (!TestNotNull(TEXT("A real enemy occupies the pending damage area"), Target)) return false;
	auto* ASC = Caster->GetAbilitySystemComponent();
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	const float EnergyBefore = Caster->GetEnergy(), HealthBefore = Target->GetHealth();
	int32 CancelRequests = 0, AreaResults = 0;
	bool bSawActiveCast = false, bSawCastingState = false;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->StateCallback = [&]
	{
		if (CancelRequests || Caster->IsTargetingSkill()) return;
		++CancelRequests; // 清理本身还会同步通知；只认领一次，不能递归取消。
		const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		bSawActiveCast = Spec && Spec->IsActive();
		bSawCastingState = Caster->IsCasting();
		Caster->CancelCast();
	};
	auto* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	const auto CueListener = CueManager->OnGameplayCueRouted().AddLambda(
		[&](AActor* Recipient, FGameplayTag Tag, EGameplayCueEvent::Type Event,
			const FGameplayCueParameters&, EGameplayCueExecutionOptions)
		{ if (Recipient == Caster && Tag == CombatTags::Cue_Area && Event == EGameplayCueEvent::Executed) ++AreaResults; });
	Caster->OnCombatStateChanged.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
	ON_SCOPE_EXIT
	{
		Caster->OnCombatStateChanged.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
		Probe->StateCallback = nullptr;
		CueManager->OnGameplayCueRouted().Remove(CueListener);
	};
	PC->InputKey(FInputKeyParams(EKeys::R, IE_Pressed, 1.));
	Fixture.Advance(.02f);
	TestTrue(TEXT("Actual R press starts preview without spending resources"), Caster->IsTargetingSkill()
		&& Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Area && Caster->GetEnergy() == EnergyBefore);
	PC->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.));
	Fixture.Advance(.02f);
	TestEqual(TEXT("Preview-close observer synchronously requests one cancellation"), CancelRequests, 1);
	TestTrue(TEXT("Notification never exposes the old gap before GAS accepted R"), bSawActiveCast && bSawCastingState);
	TestEqual(TEXT("Cancel targets an already committed cast, not an unstarted request"), Caster->GetEnergy(), EnergyBefore - Rule.Cost);
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	TestTrue(TEXT("Reentrant cancellation ends the real ability and removes its casting effect"),
		Spec && !Spec->IsActive() && !Caster->IsCasting() && !Caster->IsTargetingSkill());
	Fixture.Advance(Rule.Windup + Rule.Recovery + .3f);
	TestEqual(TEXT("No old release task damages the target after the notification canceled R"), Target->GetHealth(), HealthBefore);
	TestEqual(TEXT("No late area result is published by the canceled request"), AreaResults, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatTargetConfirmReentrantPreviewTest,
	"Multiplayer.Combat.Targeting.ConfirmNotificationPreservesNextPreview",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 完成本次接纳后，监听者可以开始下一技能预览；旧 R 松键不能把新 E 预览当作自己的确认。
bool FCoopCombatTargetConfirmReentrantPreviewTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture(true, true);
	auto* Caster = Fixture.AdmitInputPlayer();
	if (!TestNotNull(TEXT("Saved production player is admitted"), Caster)) return false;
	auto* PC = Cast<APlayerController>(Caster->GetController());
	if (!TestTrue(TEXT("The real keyboard input stack is installed"),
		PC && Cast<UEnhancedPlayerInput>(PC->PlayerInput) && Caster->InputComponent)) return false;
	Fixture.Advance(.3f);
	PC->SetControlRotation(FRotator(-20, 0, 0));
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	if (!TestTrue(TEXT("The saved player can resolve an area target"), Preview.bValid)) return false;
	auto* Target = Fixture.Enemy(Preview.End + FVector(0, 0, 100));
	if (!TestNotNull(TEXT("The area has a real damage recipient"), Target)) return false;
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	const float EnergyBefore = Caster->GetEnergy(), HealthBefore = Target->GetHealth();
	bool bOpenedNextPreview = false, bPreviousRequestAlreadyAccepted = false;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->StateCallback = [&]
	{
		if (bOpenedNextPreview || Caster->IsTargetingSkill()) return;
		bOpenedNextPreview = true;
		const auto* Spec = Caster->GetAbilitySystemComponent()->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
		bPreviousRequestAlreadyAccepted = Spec && Spec->IsActive() && Caster->IsCasting();
		Caster->BeginSkillTargeting(EMultiplayerCombatSkill::Heal);
	};
	Caster->OnCombatStateChanged.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
	ON_SCOPE_EXIT
	{
		Caster->OnCombatStateChanged.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleState);
		Probe->StateCallback = nullptr;
	};
	PC->InputKey(FInputKeyParams(EKeys::R, IE_Pressed, 1.));
	Fixture.Advance(.02f);
	PC->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.));
	Fixture.Advance(.02f);
	TestTrue(TEXT("The next preview is opened only after the previous request was accepted"),
		bOpenedNextPreview && bPreviousRequestAlreadyAccepted);
	TestTrue(TEXT("New heal preview remains independent of the accepted area cast"),
		Caster->IsCasting() && Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Heal);
	PC->InputKey(FInputKeyParams(EKeys::R, IE_Released, 0.));
	Fixture.Advance(.02f);
	TestTrue(TEXT("A stale R release does not confirm or erase the next heal preview"),
		Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Heal);
	TestEqual(TEXT("Only the original area request commits resources"), Caster->GetEnergy(), EnergyBefore - Rule.Cost);
	TestEqual(TEXT("Old key-up never creates a heal cooldown"), Caster->GetHealCooldownRemaining(), 0.f);
	Fixture.Advance(Rule.Windup + Rule.Recovery + .3f);
	TestEqual(TEXT("The original area cast still resolves exactly one configured hit"), Target->GetHealth(), HealthBefore - Rule.Amount);
	TestFalse(TEXT("Original cast ends while the next preview stays editable"), Caster->IsCasting());
	TestTrue(TEXT("Ending the original cast does not erase the next preview"),
		Caster->IsTargetingSkill() && Caster->GetTargetingSkill() == EMultiplayerCombatSkill::Heal);
	Caster->CancelSkillTargeting();
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaLethalWindupTest, "Multiplayer.Combat.Targeting.AreaLethalWindupNoRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 只以真实致命伤害中断R起手，不先眩晕、取消或撤销接纳；超过旧释放期限也不得结算伤害或表现。
bool FCoopCombatAreaLethalWindupTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	const auto Preview = Caster->GetSkillTargetPreview(EMultiplayerCombatSkill::Area);
	if (!TestTrue(TEXT("Lethal windup fixture has a valid real ground target"), Preview.bValid)) return false;
	auto* Target = Fixture.Enemy(Preview.End + FVector(0, 0, 100));
	auto* Killer = Fixture.Enemy(FVector(-1500, 0, 100));
	if (!TestNotNull(TEXT("Pending R victim exists"), Target) || !TestNotNull(TEXT("Hostile lethal-damage source exists"), Killer)) return false;
	auto* ASC = Caster->GetAbilitySystemComponent();
	auto* TargetASC = Target->GetAbilitySystemComponent();
	int32 DamageTransitions = 0, AreaResults = 0;
	const auto HealthListener = TargetASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute())
		.AddLambda([&DamageTransitions](const FOnAttributeChangeData& Change) { if (Change.NewValue < Change.OldValue) ++DamageTransitions; });
	auto* CueManager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	const auto CueListener = CueManager->OnGameplayCueRouted().AddLambda(
		[&](AActor* Recipient, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters&, EGameplayCueExecutionOptions)
		{ if (Recipient == Caster && Tag == CombatTags::Cue_Area && Event == EGameplayCueEvent::Executed) ++AreaResults; });
	Caster->CastArea();
	TestTrue(TEXT("R genuinely enters its active windup before death"), Caster->IsCasting());
	TestEqual(TEXT("The initial R activation commits its cost once"), Caster->GetEnergy(), 100.f - Rule.Cost);
	Fixture.Advance(Rule.Windup * .25f);
	TestTrue(TEXT("R remains in windup immediately before lethal damage"), Caster->IsCasting());
	TestEqual(TEXT("The pending target has not taken early damage"), Target->GetHealth(), 100.f);
	TestTrue(TEXT("Only the production unified damage entry causes the interruption"), Killer->ApplyCombatDamage(Caster, 1000.f));
	TestEqual(TEXT("The actual lethal damage GE reduces caster health to zero"), Caster->GetHealth(), 0.f);
	TestFalse(TEXT("Death, not a preliminary stun, ends the caster"), Caster->IsAlive());
	TestFalse(TEXT("Lethal damage immediately removes the casting state"), Caster->IsCasting());
	TestFalse(TEXT("Lethal interruption does not depend on stun"), Caster->IsStunned());
	const auto* AreaSpec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	TestTrue(TEXT("Death ends the actual active R ability"), AreaSpec && !AreaSpec->IsActive());
	const float EnergyAfterDeath = Caster->GetEnergy();
	Fixture.Advance(Rule.Windup + Rule.Recovery + .5f);
	TestEqual(TEXT("The expired old R task never damages its target after death"), Target->GetHealth(), 100.f);
	TestEqual(TEXT("No delayed target damage notification survives lethal interruption"), DamageTransitions, 0);
	TestEqual(TEXT("No resolved-area Cue is emitted by the cancelled old release"), AreaResults, 0);
	Caster->CastArea(); Caster->Attack(); Caster->Heal(); Caster->Defend();
	Fixture.Advance(Rule.Windup + Rule.Recovery + .2f);
	TestEqual(TEXT("Dead retry requests do not spend another skill cost"), Caster->GetEnergy(), EnergyAfterDeath);
	TestEqual(TEXT("Dead retries and late callbacks still cannot damage the pending target"), Target->GetHealth(), 100.f);
	TestEqual(TEXT("Dead retries cannot publish an area result"), AreaResults, 0);
	int32 Projectiles = 0;
	for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) ++Projectiles;
	TestEqual(TEXT("Dead retry requests cannot generate a projectile"), Projectiles, 0);
	TestFalse(TEXT("No late callback restores casting or movement after death"), Caster->IsCasting()
		|| Caster->GetCharacterMovement()->MovementMode != MOVE_None);
	CueManager->OnGameplayCueRouted().Remove(CueListener);
	TargetASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(HealthListener);
	return !HasAnyErrors();
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
		AmultiplayerCombatProjectile::FireFromSkill(*Caster, 25, Config->ProjectileSpeed);
		Fixture.Advance(1.f);
		TestEqual(TEXT("Only a target inside configured range takes real swept projectile damage"), Target->GetHealth(), bInside ? 75.f : 100.f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaReleaseInputTest, "Multiplayer.Combat.Targeting.AreaReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 按住再松开 R 经真实输入栈确认一次，验证资源、伤害和范围 Cue。
bool FCoopCombatAreaReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::Release);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaConfirmReleaseInputTest, "Multiplayer.Combat.Targeting.AreaConfirmThenReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 已用确认键释放后再松开 R，旧 release 输入不能重复施放。
bool FCoopCombatAreaConfirmReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::ConfirmThenRelease);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaCancelReleaseInputTest, "Multiplayer.Combat.Targeting.AreaCancelThenReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 右键取消后再松开 R，旧 release 不得重新确认或结算。
bool FCoopCombatAreaCancelReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::CancelThenRelease);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaSlotReleaseInputTest, "Multiplayer.Combat.Targeting.AreaSwappedSlotReleaseInput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
// 交换装配槽位后仍按实际槽位确认范围技能，不把键位写死为技能分类。
bool FCoopCombatAreaSlotReleaseInputTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::SwappedSlot);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaReleaseParticleTest, "Multiplayer.Combat.Targeting.AreaReleaseInputParticle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)
// 在非空渲染后端检查真实输入释放后的范围粒子，不只断言 Cue 请求已发出。
bool FCoopCombatAreaReleaseParticleTest::RunTest(const FString& Parameters)
{
	return RunAreaInputCase(*this, EAreaInputCase::Release, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaCandidateBoundaryTest, "Multiplayer.Combat.Targeting.AreaCandidateBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 宽阶段不提前裁决命中；预览和结算使用同一完整过滤，但结算在应用前才查询遮挡。
bool FCoopCombatAreaCandidateBoundaryTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	const FVector Center(600, 0, 0);
	constexpr float Radius = 200.f;
	auto* Visible = Fixture.Enemy(Center + FVector(0, 130, 100));
	auto* Occluded = Fixture.Enemy(Center + FVector(0, -130, 100));
	auto* Corner = Fixture.Enemy(Center + FVector(180, 180, 100));
	auto* Ally = Fixture.Player(Center + FVector(-100, 0, 100));
	if (!TestNotNull(TEXT("Caster exists"), Caster) || !TestNotNull(TEXT("Visible target exists"), Visible)
		|| !TestNotNull(TEXT("Occluded target exists"), Occluded) || !TestNotNull(TEXT("Corner target exists"), Corner)
		|| !TestNotNull(TEXT("Ally exists"), Ally)) return false;
	Fixture.Box(Center + FVector(0, -65, 100), FVector(45, 8, 100));
	const auto Candidates = CombatTargeting::CollectAreaCandidates(*Caster, Center, Radius);
	TestTrue(TEXT("Broad phase retains the obstructed candidate"), Candidates.Contains(Occluded));
	TestTrue(TEXT("Broad phase retains box corners outside the final cylinder"), Candidates.Contains(Corner));
	TestTrue(TEXT("Broad phase does not decide allegiance"), Candidates.Contains(Ally));
	const auto Targets = CombatTargeting::CollectAreaTargets(*Caster, Center, Radius);
	TestEqual(TEXT("Full preview retains only the visible hostile inside the cylinder"), Targets.Num(), 1);
	TestTrue(TEXT("Full preview selects the visible target"), Targets.Contains(Visible));
	UmultiplayerAreaAbility::ResolveTargets(*Caster, Center);
	TestTrue(TEXT("The real settlement damages the visible target"), Visible->GetHealth() < Visible->GetMaxHealth());
	TestEqual(TEXT("Settlement retains its wall check"), Occluded->GetHealth(), Occluded->GetMaxHealth());
	TestEqual(TEXT("Settlement never harms an ally"), Ally->GetHealth(), Ally->GetMaxHealth());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaRuleSnapshotTest, "Multiplayer.Combat.Targeting.AreaRuleSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 属性通知同步提升技能等级时，本次范围结算仍使用同一份规则；下一次结算才读取新等级。
bool FCoopCombatAreaRuleSnapshotTest::RunTest(const FString& Parameters)
{
	FCombatTargetingWorld Fixture;
	auto* Caster = Fixture.Player();
	const FVector Center(600, 0, 0);
	auto* First = Fixture.Enemy(Center + FVector(0, -100, 100));
	auto* Second = Fixture.Enemy(Center + FVector(0, 100, 100));
	if (!TestNotNull(TEXT("Caster exists"), Caster) || !TestNotNull(TEXT("First target exists"), First)
		|| !TestNotNull(TEXT("Second target exists"), Second)) return false;
	const auto Rule = Caster->GetSkillRule(EMultiplayerCombatSkill::Area);
	bool bLevelChanged = false;
	TArray<TPair<UAbilitySystemComponent*, FDelegateHandle>> Subscriptions;
	ON_SCOPE_EXIT
	{
		for (const auto& Subscription : Subscriptions)
			Subscription.Key->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute())
				.Remove(Subscription.Value);
	};
	for (auto* Target : {First, Second})
	{
		auto* TargetASC = Target->GetAbilitySystemComponent();
		const FDelegateHandle Handle = TargetASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute())
			.AddLambda([Caster, &bLevelChanged](const FOnAttributeChangeData& Change)
			{
				if (bLevelChanged || Change.NewValue >= Change.OldValue) return;
				bLevelChanged = true;
				auto* ASC = Caster->GetAbilitySystemComponent();
				auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
				if (Spec) { Spec->Level += 5; ASC->MarkAbilitySpecDirty(*Spec); }
			});
		Subscriptions.Emplace(TargetASC, Handle);
	}
	UmultiplayerAreaAbility::ResolveTargets(*Caster, Center);
	TestTrue(TEXT("A real damage callback changed the ability level during settlement"), bLevelChanged);
	TestTrue(TEXT("Later skill queries see the upgraded rule"), Caster->GetSkillRule(EMultiplayerCombatSkill::Area).Amount > Rule.Amount);
	TestEqual(TEXT("First target uses the release snapshot"), First->GetHealth(), First->GetMaxHealth() - Rule.Amount);
	TestEqual(TEXT("Second target uses the same release snapshot"), Second->GetHealth(), Second->GetMaxHealth() - Rule.Amount);
	return true;
}

#endif
