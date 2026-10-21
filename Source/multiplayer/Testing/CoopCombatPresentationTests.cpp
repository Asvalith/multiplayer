// Copyright Epic Games, Inc. All Rights Reserved.

// 回归保存的角色、动画、HUD 和 Cue 的实际配置及可观察表现状态。
// 不修改共享素材来让断言通过；碰撞／粒子状态检查仍不代替最终实机观感验收。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "Blueprint/UserWidget.h"
#include "Camera/CameraComponent.h"
#include "Camera/CameraTypes.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Combat/multiplayerCombatPresentation.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/Border.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/DecalComponent.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/WidgetComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameplayCueManager.h"
#include "Materials/MaterialInterface.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeExit.h"
#include "NiagaraComponent.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Player/multiplayerCharacter.h"
#include "Slate/SObjectWidget.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#if WITH_EDITOR
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_Slot.h"
#include "EdGraph/EdGraph.h"
#endif

namespace
{
	// 提供真实碰撞和保存蓝图实例，供相机、动作及特效断言使用，不创建网络连接或修改资产。
	struct FCombatPresentationWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;

		// 创建带碰撞地面的独立表现世界，保留保存资产的正式构造与运行状态。
		FCombatPresentationWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			Box(FVector(0, 0, -10), FVector(5000, 5000, 10));
		}

		// 对称结束表现对象和世界，清除其计时器并恢复全局帧号。
		~FCombatPresentationWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 推进两个真实帧，让初始化、动画和组件更新完成后再读取断言值。
		void Advance()
		{
			for (int32 Frame = 0; Frame < 2; ++Frame)
			{
				++GFrameCounter;
				World->Tick(LEVELTICK_All, .02f);
			}
		}

		// 加载保存的实际角色蓝图并执行其构造脚本；禁用 AI 和移动，避免碰撞定位被追击或落地干扰。
		AmultiplayerCombatCharacter* Character(UClass* Class, const FVector& Location)
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			auto* Result = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(Class, Transform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Result) return nullptr;
			Result->AutoPossessPlayer = EAutoReceiveInput::Disabled;
			Result->AutoPossessAI = EAutoPossessAI::Disabled;
			Result->FinishSpawning(Transform);
			Result->GetCharacterMovement()->DisableMovement();
			Result->GetCharacterMovement()->SetComponentTickEnabled(false);
			return Result;
		}

		// 创建真实场景阻挡，供相机避让与表现定位查询使用。
		AActor* Box(const FVector& Location, const FVector& Extent)
		{
			AActor* Actor = World->SpawnActor<AActor>();
			UBoxComponent* Shape = NewObject<UBoxComponent>(Actor);
			Actor->SetRootComponent(Shape);
			Shape->SetBoxExtent(Extent);
			Shape->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Shape->SetCollisionObjectType(ECC_WorldStatic);
			Shape->SetCollisionResponseToAllChannels(ECR_Block);
			Shape->RegisterComponent();
			Actor->SetActorLocation(Location);
			return Actor;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGuardImpactPresentationTest, "Multiplayer.Combat.Guard.BlockedHitPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 真实正式蓝图、ASC Cue 路由和受击蒙太奇；无损正面格挡不能触发伤害粒子或受击动作。
bool FCoopCombatGuardImpactPresentationTest::RunTest(const FString& Parameters)
{
	UGameplayCueManager* Manager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	if (!TestNotNull(TEXT("Actual gameplay cue manager exists"), Manager)) return false;
#if WITH_EDITOR
	Manager->LoadNotifyForEditorPreview(CombatTags::Cue_Impact);
#endif
	for (const TCHAR* Path : { TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"),
		TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant_C") })
	{
		UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr, Path);
		if (!TestNotNull(TEXT("Saved formal player blueprint exists"), PlayerClass)) return false;
		for (int32 Case = 0; Case < 3; ++Case)
		{
			FCombatPresentationWorld Fixture;
			auto* Player = Fixture.Character(PlayerClass, FVector(0, 0, 100));
			const FVector SourceLocation = Case == 0 ? FVector(500, 0, 100)
				: Case == 1 ? FVector(0, 500, 100) : FVector(-500, 0, 100);
			auto* Source = Fixture.Character(AmultiplayerCombatEnemy::StaticClass(), SourceLocation);
			if (!TestNotNull(TEXT("Formal guard player spawned"), Player) || !TestNotNull(TEXT("Real hostile source spawned"), Source)) return false;
			Player->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			Player->GetMesh()->bEnableUpdateRateOptimizations = false;
			auto* Animation = Cast<UmultiplayerCombatAnimInstance>(Player->GetMesh()->GetAnimInstance());
			const auto* HitField = FindFProperty<FObjectPropertyBase>(Player->GetClass(), TEXT("HitMontage"));
			auto* Hit = HitField ? Cast<UAnimMontage>(HitField->GetObjectPropertyValue_InContainer(Player)) : nullptr;
			if (!TestNotNull(TEXT("Formal animation blueprint is initialized"), Animation)
				|| !TestNotNull(TEXT("Formal player retains a real hit montage as the failure control"), Hit)) return false;
			const auto& Feedback = Player->GetCombatConfig().ImpactFeedback;
			UParticleSystem* Cascade = Feedback.CascadeEffect.IsNull() ? nullptr : Feedback.CascadeEffect.LoadSynchronous();
			UNiagaraSystem* Niagara = Cascade || Feedback.Effect.IsNull() ? nullptr : Feedback.Effect.LoadSynchronous();
			Feedback.Sound.LoadSynchronous();
			if (!TestTrue(TEXT("The real configured impact particle loads"), Cascade || Niagara)) return false;
			const auto ImpactComponents = [&]()
			{
				int32 Count = 0;
				if (Cascade)
					for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
						if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->Template == Cascade) ++Count;
				if (Niagara)
					for (TObjectIterator<UNiagaraComponent> It; It; ++It)
						if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->GetAsset() == Niagara) ++Count;
				return Count;
			};
			Player->Defend();
			Fixture.Advance();
			if (!TestTrue(TEXT("Real Q reaches its persistent guard pose without a hit montage"), Player->IsDefending()
				&& Animation->CombatIsDefending && !Animation->IsAnyMontagePlaying())) return false;
			const float PreviousHealth = Player->GetHealth(), PreviousShield = Player->GetShield();
			const float Damage = FMath::Max(50.f, PreviousShield + 10.f);
			const int32 PreviousParticles = ImpactComponents(), PreviousMontages = Animation->MontageInstances.Num();
			int32 ImpactRoutes = 0;
			const FDelegateHandle Route = Manager->OnGameplayCueRouted().AddLambda(
				[&](AActor* Target, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters&, EGameplayCueExecutionOptions)
				{
					if (Target == Player && Tag == CombatTags::Cue_Impact && Event == EGameplayCueEvent::Executed) ++ImpactRoutes;
				});
			auto* ASC = Player->GetAbilitySystemComponent();
			FDelegateHandle TurnOnDamage;
			if (Case == 2)
				TurnOnDamage = ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).AddLambda(
					[Player, Source](const FOnAttributeChangeData& Change)
					{
						// 已经吃到背击后同步转身，不能让提交后的新朝向追溯取消本次受击反馈。
						if (Change.NewValue < Change.OldValue)
							Player->SetActorRotation((Source->GetActorLocation() - Player->GetActorLocation()).Rotation());
					});
			ON_SCOPE_EXIT
			{
				Manager->OnGameplayCueRouted().Remove(Route);
				if (TurnOnDamage.IsValid()) ASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetHealthAttribute()).Remove(TurnOnDamage);
			};
			const FString Label = FString::Printf(TEXT("%s %s"), *PlayerClass->GetName(),
				Case == 0 ? TEXT("front") : Case == 1 ? TEXT("side") : TEXT("rear-turn-callback"));
			TestTrue(*(Label + TEXT(" accepts the real authoritative damage request")), Source->ApplyCombatDamage(Player, Damage));
			const bool bBlocked = Case == 0;
			TestEqual(*(Label + TEXT(" retains the exact health settlement")), Player->GetHealth(),
				bBlocked ? PreviousHealth : PreviousHealth - FMath::Max(0.f, Damage - PreviousShield));
			TestEqual(*(Label + TEXT(" retains the exact shield settlement")), Player->GetShield(),
				bBlocked ? PreviousShield : FMath::Max(0.f, PreviousShield - Damage));
			TestEqual(*(Label + TEXT(" publishes zero blocked or one unblocked damage cue")), ImpactRoutes, bBlocked ? 0 : 1);
			TestEqual(*(Label + TEXT(" spawns no blocked-hit particle and one actual unblocked impact")),
				ImpactComponents(), PreviousParticles + (bBlocked ? 0 : 1));
			TestEqual(*(Label + TEXT(" only an unblocked hit starts the connected hit montage")), Animation->Montage_IsPlaying(Hit), !bBlocked);
			if (bBlocked)
			{
				TestEqual(TEXT("Blocked hit allocates no montage instance"), Animation->MontageInstances.Num(), PreviousMontages);
				Fixture.Advance();
				TestTrue(TEXT("The actual guard pose remains after the blocked hit"), Animation->CombatIsDefending && !Animation->Montage_IsPlaying(Hit));
			}
			else if (Case == 2)
				TestTrue(TEXT("Rear-hit health notification really turned the body to the guarded front before cue publication"),
					FVector::DotProduct(Player->GetActorForwardVector(), (Source->GetActorLocation() - Player->GetActorLocation()).GetSafeNormal()) > .99f);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCameraObstructionTest, "Multiplayer.Combat.CameraObstruction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 近身角色不能将镜头推入玩家身体；墙体仍须收缩弹簧臂，不能靠关闭全部相机碰撞修复。
bool FCoopCombatCameraObstructionTest::RunTest(const FString& Parameters)
{
	UClass* PlayerClass = LoadClass<AmultiplayerCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	UClass* EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	if (!TestNotNull(TEXT("Saved combat player blueprint exists"), PlayerClass)
		|| !TestNotNull(TEXT("Saved combat enemy blueprint exists"), EnemyClass)) return false;

	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(PlayerClass, FVector(0, 0, 200));
	if (!TestNotNull(TEXT("Actual combat player spawned"), Player)) return false;
	USpringArmComponent* Arm = Player->FindComponentByClass<USpringArmComponent>();
	TInlineComponentArray<UCameraComponent*> Cameras;
	Player->GetComponents(Cameras);
	TestEqual(TEXT("Actual player has exactly one camera"), Cameras.Num(), 1);
	if (!TestNotNull(TEXT("Actual player inherited its spring arm"), Arm) || Cameras.Num() != 1) return false;
	UCameraComponent* Camera = Cameras[0];
	TestTrue(TEXT("Actual camera is active after construction"), Camera->IsActive());
	TestTrue(TEXT("Actual camera is attached to the spring arm"), Camera->GetAttachParent() == Arm);
	TestEqual(TEXT("Actual camera uses the spring-arm endpoint"), Camera->GetAttachSocketName(), USpringArmComponent::SocketName);
	TestTrue(TEXT("Actual player retains the 400cm arm"), FMath::IsNearlyEqual(Arm->TargetArmLength, 400.f));
	TestTrue(TEXT("World obstruction testing remains enabled"), Arm->bDoCollisionTest);
	TestTrue(TEXT("Fixture has no player controller"), Player->GetController() == nullptr);

	struct FCameraObservation
	{
		float Distance;
		bool bFixed;
		FHitResult Hit;
	};
	// 使用和弹簧臂相同的探针记录阻挡组件，再比对 CalcCamera 真正返回的视点，而非只检查 CDO。
	const auto Observe = [&](const FString& Phase)
	{
		Fixture.Advance();
		FMinimalViewInfo View;
		Player->CalcCamera(.02f, View);
		const FVector Origin = Arm->GetComponentLocation() + Arm->TargetOffset;
		FCameraObservation Result;
		Result.Distance = FVector::Distance(Origin, View.Location);
		Result.bFixed = Arm->IsCollisionFixApplied();
		FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatCameraObstruction), false, Player);
		Fixture.World->SweepSingleByChannel(Result.Hit, Origin, Arm->GetUnfixedCameraPosition(),
			FQuat::Identity, Arm->ProbeChannel, FCollisionShape::MakeSphere(Arm->ProbeSize), Query);
		AddInfo(FString::Printf(TEXT("CombatCamera phase=%s distance=%.2f fixed=%d arm=%.2f active=%d "
			"view=%s component=%s hitActor=%s hitComponent=%s startPenetrating=%d"),
			*Phase, Result.Distance, Result.bFixed, Arm->TargetArmLength, Camera->IsActive(),
			*View.Location.ToCompactString(), *Camera->GetComponentLocation().ToCompactString(),
			*GetNameSafe(Result.Hit.GetActor()), *GetNameSafe(Result.Hit.GetComponent()), Result.Hit.bStartPenetrating));
		TestTrue(*FString::Printf(TEXT("%s CalcCamera uses the active component"), *Phase),
			View.Location.Equals(Camera->GetComponentLocation(), .01f));
		return Result;
	};

	const FCameraObservation Empty = Observe(TEXT("Empty"));
	TestTrue(TEXT("Unobstructed actual player uses full arm length"), FMath::IsNearlyEqual(Empty.Distance, 400.f, .5f));
	TestFalse(TEXT("Player does not collide with its own camera probe"), Empty.bFixed);

	struct FDefaultResponse
	{
		FString Role;
		ECollisionResponse Capsule;
		ECollisionResponse Mesh;
	};
	TArray<FDefaultResponse> Defaults;
	Defaults.Add({ TEXT("OwningPlayer"), Player->GetCapsuleComponent()->GetCollisionResponseToChannel(ECC_Camera),
		Player->GetMesh()->GetCollisionResponseToChannel(ECC_Camera) });
	for (UClass* BlockerClass : { PlayerClass, EnemyClass })
	{
		const FString Role = BlockerClass == PlayerClass ? TEXT("OtherPlayer") : TEXT("Enemy");
		auto* Blocker = Fixture.Character(BlockerClass, FVector(-80, 0, 200));
		if (!TestNotNull(*FString::Printf(TEXT("%s actual blueprint spawned"), *Role), Blocker)) return false;
		UCapsuleComponent* Capsule = Blocker->GetCapsuleComponent();
		USkeletalMeshComponent* Mesh = Blocker->GetMesh();
		Defaults.Add({ Role, Capsule->GetCollisionResponseToChannel(ECC_Camera), Mesh->GetCollisionResponseToChannel(ECC_Camera) });
		AddInfo(FString::Printf(TEXT("CombatCamera defaults role=%s capsule=%d mesh=%d radius=%.2f"),
			*Role, static_cast<int32>(Defaults.Last().Capsule), static_cast<int32>(Defaults.Last().Mesh), Capsule->GetScaledCapsuleRadius()));
		TestTrue(*FString::Printf(TEXT("%s has no controller or AI"), *Role), Blocker->GetController() == nullptr);
		Observe(Role + TEXT("Default"));

		// 显式保留故障对照：只修改测试实例，即使生产默认值已修复，也必须能复现旧的近身挤镜头。
		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
		Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Block);
		const FCameraObservation Blocked = Observe(Role + TEXT("Block"));
		TestTrue(*FString::Printf(TEXT("%s block response contracts the arm"), *Role), Blocked.bFixed);
		TestTrue(*FString::Printf(TEXT("%s block reproduces a view inside the owning capsule"), *Role),
			Blocked.Distance < Player->GetCapsuleComponent()->GetScaledCapsuleRadius());
		TestTrue(*FString::Printf(TEXT("%s is the camera obstruction"), *Role), Blocked.Hit.GetActor() == Blocker);

		Capsule->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		Mesh->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
		const FCameraObservation Ignored = Observe(Role + TEXT("Ignore"));
		TestTrue(*FString::Printf(TEXT("%s ignore response restores the full arm"), *Role),
			FMath::IsNearlyEqual(Ignored.Distance, 400.f, .5f));
		TestFalse(*FString::Printf(TEXT("%s no longer triggers collision correction"), *Role), Ignored.bFixed);
		Blocker->Destroy();
	}

	AActor* Wall = Fixture.Box(FVector(-200, 0, 200), FVector(10, 300, 300));
	const FCameraObservation Walled = Observe(TEXT("Wall"));
	TestTrue(TEXT("World wall still contracts the camera arm"), Walled.bFixed);
	TestTrue(TEXT("World wall, not the player, caused the obstruction"), Walled.Hit.GetActor() == Wall);
	TestTrue(TEXT("Camera remains before the wall rather than passing through it"), Walled.Distance > 100.f && Walled.Distance < 200.f);

	// 最后检查真实资产实例的原始响应；故障对照的手工 Ignore 不能掩盖生产角色仍旧配置错误。
	for (const FDefaultResponse& Response : Defaults)
	{
		TestTrue(*FString::Printf(TEXT("%s actual capsule defaults to Camera Ignore"), *Response.Role), Response.Capsule == ECR_Ignore);
		TestTrue(*FString::Printf(TEXT("%s actual mesh defaults to Camera Ignore"), *Response.Role), Response.Mesh == ECR_Ignore);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHUDEventLifecycleTest, "Multiplayer.Combat.HUDEventLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 加载真实 WBP，推进真实 GAS/Timer 并读取显示结果；不截图，不把无 RHI 的逻辑测试说成画面验收。
bool FCoopCombatHUDEventLifecycleTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Slate is initialized for UMG construction without a viewport"), FSlateApplication::IsInitialized())) return false;
	UClass* WidgetClass = LoadClass<UmultiplayerCombatWidget>(nullptr,
		TEXT("/Game/Combat/Presentation/UI/WBP_ARPGCombatVisuals.WBP_ARPGCombatVisuals_C"));
	if (!TestNotNull(TEXT("Actual combat HUD blueprint exists"), WidgetClass)) return false;

	FCombatPresentationWorld Fixture;
	const auto Advance = [&](float Seconds)
	{
		for (float Elapsed = 0; Elapsed < Seconds;)
		{
			const float Step = FMath::Min(.02f, Seconds - Elapsed);
			++GFrameCounter;
			Fixture.World->Tick(LEVELTICK_All, Step);
			Elapsed += Step;
		}
	};
	auto* Player = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(0, 0, 100));
	auto* Widget = CreateWidget<UmultiplayerCombatWidget>(Fixture.World, WidgetClass);
	if (!TestNotNull(TEXT("HUD fixture character spawned"), Player)
		|| !TestNotNull(TEXT("Actual HUD is created with the fixture world"), Widget)) return false;
	// TakeWidget 按引擎正常路径调用 NativeConstruct，无需窗口、AddToViewport 或 GPU 读回。
	TSharedPtr<SObjectWidget> Slate = StaticCastSharedRef<SObjectWidget>(Widget->TakeWidget());
	if (!TestTrue(TEXT("Slate wrapper owns the constructed actual HUD"), Slate->GetWidgetObject() == Widget)) return false;
	UFunction* BindFunction = Widget->FindFunction(TEXT("BindPawn"));
	if (!TestNotNull(TEXT("Pawn binding remains a reflected lifecycle entry"), BindFunction)) return false;
	const auto Bind = [&](APawn* Previous, APawn* Current)
	{
		struct FBindPawnParameters { APawn* PreviousPawn; APawn* NewPawn; } Arguments{Previous, Current};
		Widget->ProcessEvent(BindFunction, &Arguments);
	};
	// UE5.5 没有 ObjectHasAnyTimers；公开 ListTimers 同时列出活动/暂停/待执行项，按完整对象路径一次检查。
	const auto HasWidgetTimer = [&]
	{
		// 日志由独立线程分发；隔离上次输出并等本次完成，不能把旧 Timer 行当成当前泄漏。
		GLog->FlushThreadedLogs();
		FStringOutputDevice Output;
		GLog->AddOutputDevice(&Output);
		Fixture.World->GetTimerManager().ListTimers();
		GLog->FlushThreadedLogs();
		GLog->RemoveOutputDevice(&Output);
		return Output.Contains(Widget->GetPathName(), ESearchCase::CaseSensitive);
	};
	const auto Damage = [](AmultiplayerCombatCharacter* Target, float Amount)
	{
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f,
			ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	};
	UTextBlock* HealthText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("HealthText")));
	UTextBlock* EnergyText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("EnergyText")));
	UTextBlock* StateText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("StateText")));
	UTextBlock* AreaCooldownText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("AreaCooldownText")));
	UTextBlock* FeedbackText = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("FeedbackText")));
	UProgressBar* HealthBar = Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("HealthBar")));
	UProgressBar* CastBar = Cast<UProgressBar>(Widget->GetWidgetFromName(TEXT("CastBar")));
	UBorder* CastingPanel = Cast<UBorder>(Widget->GetWidgetFromName(TEXT("CastingPanel")));
	UBorder* FeedbackPanel = Cast<UBorder>(Widget->GetWidgetFromName(TEXT("FeedbackPanel")));
	if (!TestNotNull(TEXT("HUD health text"), HealthText) || !TestNotNull(TEXT("HUD energy text"), EnergyText)
		|| !TestNotNull(TEXT("HUD state text"), StateText) || !TestNotNull(TEXT("HUD area cooldown"), AreaCooldownText)
		|| !TestNotNull(TEXT("HUD feedback"), FeedbackText) || !TestNotNull(TEXT("HUD health bar"), HealthBar)
		|| !TestNotNull(TEXT("HUD cast bar"), CastBar) || !TestNotNull(TEXT("HUD casting panel"), CastingPanel)
		|| !TestNotNull(TEXT("HUD feedback panel"), FeedbackPanel)) return false;

	// 检查保存的左下布局和设计尺寸；这里不把无视口的控件属性当成像素级画面验收。
	UVerticalBox* Vitals = Cast<UVerticalBox>(Widget->GetWidgetFromName(TEXT("ARPGVitalsStack")));
	UBorder* Skills = Cast<UBorder>(Widget->GetWidgetFromName(TEXT("ARPGSkillsPanel")));
	UBorder* Stats = Cast<UBorder>(Widget->GetWidgetFromName(TEXT("ARPGStatsPanel")));
	if (!TestNotNull(TEXT("Vitals retain their own vertical stack"), Vitals)
		|| !TestNotNull(TEXT("Actual ARPG skills panel exists"), Skills)
		|| !TestNotNull(TEXT("Actual ARPG stats panel exists"), Stats)) return false;
	UCanvasPanelSlot* VitalsSlot = Cast<UCanvasPanelSlot>(Vitals->Slot);
	if (!TestNotNull(TEXT("The shared vitals stack has one canvas placement"), VitalsSlot)) return false;
	TestTrue(TEXT("Vitals remain anchored at bottom left"),
		VitalsSlot->GetAnchors().Minimum.Equals(FVector2D(0, 1))
		&& VitalsSlot->GetAnchors().Maximum.Equals(FVector2D(0, 1))
		&& VitalsSlot->GetAlignment().Equals(FVector2D(0, 1)));
	TestEqual(TEXT("The vitals stack contains only stats"), Vitals->GetChildrenCount(), 1);
	TestEqual(TEXT("Stats retain their independent left-side parent"), Vitals->GetChildIndex(Stats->GetParent()), 0);
	const auto* SkillCanvas = Cast<UCanvasPanelSlot>(Skills->Slot);
	if (!TestNotNull(TEXT("Skills have an independent canvas placement"), SkillCanvas)) return false;
	TestTrue(TEXT("Five skills are centered at the bottom instead of squeezed into the health bar"),
		SkillCanvas->GetAnchors().Minimum.Equals(FVector2D(.5f, 1))
		&& SkillCanvas->GetAlignment().Equals(FVector2D(.5f, 1))
		&& SkillCanvas->GetSize().Equals(FVector2D(800, 160)));
	TestTrue(TEXT("Skills and stats panels never draw a backing block"),
		Skills->Background.DrawAs == ESlateBrushDrawType::NoDrawType
		&& Stats->Background.DrawAs == ESlateBrushDrawType::NoDrawType);
	const TCHAR* IconNames[] = { TEXT("AttackIcon"), TEXT("DefenseIcon"), TEXT("HealIcon"), TEXT("AreaIcon"), TEXT("DashIcon") };
	const TCHAR* SlotNames[] = { TEXT("AttackSlot"), TEXT("DefenseSlot"), TEXT("HealSlot"), TEXT("AreaSlot"), TEXT("DashSlot") };
	const TCHAR* CounterNames[] = { TEXT("AttackCooldownText"), TEXT("DefenseCooldownText"), TEXT("HealCooldownText"), TEXT("AreaCooldownText"), TEXT("DashCooldownText") };
	UImage* Icons[5] = {};
	UBorder* SkillSlots[5] = {};
	UTextBlock* Counters[5] = {};
	FLinearColor CounterColors[5];
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Icons); ++Index)
	{
		Icons[Index] = Cast<UImage>(Widget->GetWidgetFromName(FName(IconNames[Index])));
		SkillSlots[Index] = Cast<UBorder>(Widget->GetWidgetFromName(FName(SlotNames[Index])));
		Counters[Index] = Cast<UTextBlock>(Widget->GetWidgetFromName(FName(CounterNames[Index])));
		if (!TestNotNull(IconNames[Index], Icons[Index]) || !TestNotNull(SlotNames[Index], SkillSlots[Index])
			|| !TestNotNull(CounterNames[Index], Counters[Index])) return false;
		UOverlay* IconLayer = Cast<UOverlay>(Icons[Index]->GetParent());
		USizeBox* IconSize = IconLayer ? Cast<USizeBox>(IconLayer->GetParent()) : nullptr;
		if (!TestNotNull(TEXT("Each skill icon has its own size box"), IconSize)) return false;
		TestTrue(TEXT("44-unit skill icons at design scale 2 are 88 by 88"),
			IconSize->IsWidthOverride() && IconSize->IsHeightOverride()
			&& FMath::IsNearlyEqual(IconSize->GetWidthOverride(), 88.f)
			&& FMath::IsNearlyEqual(IconSize->GetHeightOverride(), 88.f));
		CounterColors[Index] = Counters[Index]->GetColorAndOpacity().GetSpecifiedColor();
		TestTrue(TEXT("Cooldown text starts bright and opaque"),
			CounterColors[Index].A > .99f && CounterColors[Index].GetLuminance() > .8f);
	}
	const auto CheckSkillPresentation = [&](const TCHAR* Phase)
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Icons); ++Index)
		{
			TestTrue(*FString::Printf(TEXT("%s %s keeps NoDraw and a transparent background"), Phase, SlotNames[Index]),
				SkillSlots[Index]->Background.DrawAs == ESlateBrushDrawType::NoDrawType
				&& FMath::IsNearlyZero(SkillSlots[Index]->GetBrushColor().A));
			TestTrue(*FString::Printf(TEXT("%s %s remains readable independently of icon dimming"), Phase, CounterNames[Index]),
				SkillSlots[Index]->GetIsEnabled() && SkillSlots[Index]->GetContentColorAndOpacity().Equals(FLinearColor::White)
				&& Counters[Index]->GetColorAndOpacity().GetSpecifiedColor().Equals(CounterColors[Index])
				&& FMath::IsNearlyEqual(Counters[Index]->GetRenderOpacity(), 1.f));
		}
	};
	const FLinearColor Unavailable(.4f, .4f, .4f, 1.f);

	Bind(nullptr, Player);
	TestEqual(TEXT("Binding reads the health snapshot without waiting for a future event"), HealthText->GetText().ToString(), FString(TEXT("100 / 100")));
	TestEqual(TEXT("Binding reads the energy snapshot"), EnergyText->GetText().ToString(), FString(TEXT("100 / 100")));
	TestTrue(TEXT("Idle casting panel is collapsed"), CastingPanel->GetVisibility() == ESlateVisibility::Collapsed);
	TestTrue(TEXT("Idle state text is collapsed"), StateText->GetVisibility() == ESlateVisibility::Collapsed);
	TestFalse(TEXT("Idle HUD has no bound timer"), HasWidgetTimer());
	for (int32 Index = 0; Index < 4; ++Index) TestTrue(TEXT("Learned skills with ready resources start at full icon brightness"), Icons[Index]->GetColorAndOpacity().Equals(FLinearColor::White));
	TestTrue(TEXT("The visible fifth slot clearly dims the unlearned dash"), Icons[4]->GetColorAndOpacity().Equals(Unavailable));
	CheckSkillPresentation(TEXT("Idle"));

	Damage(Player, 40);
	TestEqual(TEXT("Real damage updates health text synchronously through its event"), HealthText->GetText().ToString(), FString(TEXT("60 / 100")));
	TestTrue(TEXT("Real damage updates the health bar"), FMath::IsNearlyEqual(HealthBar->GetPercent(), .6f));
	TestTrue(TEXT("Injury retains the resource-ready healing icon"), Icons[2]->GetColorAndOpacity().Equals(FLinearColor::White));
	TestFalse(TEXT("Attribute updates do not start HUD polling"), HasWidgetTimer());
	Player->Defend();
	TestTrue(TEXT("Defense appears immediately without advancing a display timer"), StateText->GetText().ToString().Contains(TEXT("防御")));
	TestEqual(TEXT("Predicted-style resource events refresh the energy display"), EnergyText->GetText().ToString(), FString(TEXT("80 / 100")));
	TestTrue(TEXT("A real cooldown starts the short display timer"), HasWidgetTimer());
	TestTrue(TEXT("Defense cooldown dims only its unavailable icon"), Icons[1]->GetColorAndOpacity().Equals(Unavailable)
		&& Icons[0]->GetColorAndOpacity().Equals(FLinearColor::White));
	Player->CastArea();
	TestTrue(TEXT("A real area ability entered casting"), Player->IsCasting());
	TestTrue(TEXT("Casting event shows the previously hidden panel immediately"), CastingPanel->GetVisibility() != ESlateVisibility::Collapsed);
	TestFalse(TEXT("Active cooldown displays its number"), AreaCooldownText->GetText().IsEmpty());
	Advance(.2f);
	TestTrue(TEXT("Only active casting advances its display progress"), CastBar->GetPercent() > 0);
	for (const UImage* Icon : Icons) TestTrue(TEXT("Casting dims the unavailable skill icons"), Icon->GetColorAndOpacity().Equals(Unavailable));
	CheckSkillPresentation(TEXT("Casting"));
	Player->CancelCast();
	TestFalse(TEXT("Cancellation clears the real casting state"), Player->IsCasting());
	TestTrue(TEXT("Cancellation hides the panel immediately"), CastingPanel->GetVisibility() == ESlateVisibility::Collapsed);
	TestTrue(TEXT("Cancellation retains the committed cooldown display"), HasWidgetTimer());
	TestTrue(TEXT("Cancellation restores ready icons but not the committed area cooldown"),
		Icons[0]->GetColorAndOpacity().Equals(FLinearColor::White) && Icons[3]->GetColorAndOpacity().Equals(Unavailable));
	TestFalse(TEXT("Committed area cooldown retains readable digits after cancellation"), AreaCooldownText->GetText().IsEmpty());
	CheckSkillPresentation(TEXT("Cooldown"));
	Advance(9.f);
	TestTrue(TEXT("Expired area cooldown clears its number"), AreaCooldownText->GetText().IsEmpty());
	TestTrue(TEXT("Expired defense clears its state text"), StateText->GetText().IsEmpty());
	TestFalse(TEXT("All cooldowns expired so the display timer stops"), HasWidgetTimer());
	for (int32 Index = 0; Index < 4; ++Index) TestTrue(TEXT("Expired cooldowns restore learned icon brightness"), Icons[Index]->GetColorAndOpacity().Equals(FLinearColor::White));
	Player->GetProgressionComponent()->RequestLearnSkill(EMultiplayerCombatSkill::Dash);
	TestTrue(TEXT("Learning dash refreshes the fifth icon through the progression event"), Icons[4]->GetColorAndOpacity().Equals(FLinearColor::White));
	CheckSkillPresentation(TEXT("Expired"));

	auto* Fresh = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(0, 1200, 100));
	if (!TestNotNull(TEXT("Replacement pawn spawned"), Fresh)) return false;
	Bind(Player, Fresh);
	TestEqual(TEXT("Pawn replacement immediately displays its own snapshot"), HealthText->GetText().ToString(), FString(TEXT("100 / 100")));
	Damage(Player, 10);
	Player->Defend();
	Player->OnCombatMessage.Broadcast(FText::FromString(TEXT("旧角色提示")));
	TestEqual(TEXT("Old pawn attribute events no longer change the HUD"), HealthText->GetText().ToString(), FString(TEXT("100 / 100")));
	TestTrue(TEXT("Old pawn state events no longer change the HUD"), StateText->GetText().IsEmpty());
	TestTrue(TEXT("Old pawn feedback is unsubscribed"), FeedbackText->GetText().IsEmpty());
	TestFalse(TEXT("Old pawn cooldowns do not restart HUD timers"), HasWidgetTimer());
	Fresh->ApplyStun(.25f);
	TestTrue(TEXT("Real stun immediately updates the state area"), StateText->GetText().ToString().Contains(TEXT("眩晕")));
	TestFalse(TEXT("Static stun state needs no periodic display timer"), HasWidgetTimer());
	Advance(.4f);
	TestTrue(TEXT("Stun removal event clears the state area"), StateText->GetText().IsEmpty());
	Fresh->SetCombatSessionReady(false);
	TestTrue(TEXT("Admission changes update the state area"), StateText->GetText().ToString().Contains(TEXT("连接")));
	Fresh->SetCombatSessionReady(true);
	TestTrue(TEXT("Admission completion hides the empty state area"), StateText->GetVisibility() == ESlateVisibility::Collapsed);
	Damage(Fresh, 1000);
	TestTrue(TEXT("Real death immediately displays its persistent state"), StateText->GetText().ToString().Contains(TEXT("死亡")));
	TestFalse(TEXT("Death does not leave a HUD refresh timer"), HasWidgetTimer());

	auto* Third = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(0, 2400, 100));
	if (!TestNotNull(TEXT("Feedback and destruction fixture spawned"), Third)) return false;
	Bind(Fresh, Third);
	Third->OnCombatMessage.Broadcast(FText::FromString(TEXT("测试提示")));
	TestEqual(TEXT("Feedback events show the current message"), FeedbackText->GetText().ToString(), FString(TEXT("测试提示")));
	TestTrue(TEXT("Feedback panel is visible only while needed"), FeedbackPanel->GetVisibility() != ESlateVisibility::Collapsed);
	TestTrue(TEXT("Feedback owns a single expiration timer"), HasWidgetTimer());
	Advance(2.2f);
	TestTrue(TEXT("Feedback expires without a perpetual polling loop"), FeedbackText->GetText().IsEmpty());
	TestTrue(TEXT("Expired feedback panel is collapsed"), FeedbackPanel->GetVisibility() == ESlateVisibility::Collapsed);
	TestFalse(TEXT("Expired one-shot feedback leaves no HUD timer"), HasWidgetTimer());
	auto* PreviewController = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Local targeting controller spawned"), PreviewController)) return false;
	PreviewController->Possess(Third);
	if (!TestTrue(TEXT("Target preview uses a genuinely locally controlled pawn"), Third->IsLocallyControlled())) return false;
	Third->OnCombatMessage.Broadcast(FText::FromString(TEXT("上一技能失败")));
	Third->BeginSkillTargeting(EMultiplayerCombatSkill::Heal);
	TestTrue(TEXT("Beginning another preview clears the previous skill failure immediately"), FeedbackText->GetText().IsEmpty());
	TestTrue(TEXT("New preview collapses stale feedback instead of contradicting current eligibility"), FeedbackPanel->GetVisibility() == ESlateVisibility::Collapsed);
	TestFalse(TEXT("Clearing stale feedback also releases its expiration timer"), HasWidgetTimer());
	Third->CancelSkillTargeting();
	Third->CastArea();
	TestTrue(TEXT("Destroy fixture entered casting"), Third->IsCasting());
	Third->Destroy();
	TestTrue(TEXT("Pawn destruction hides its casting panel"), CastingPanel->GetVisibility() == ESlateVisibility::Collapsed);
	TestFalse(TEXT("Pawn destruction clears all widget-owned timers"), HasWidgetTimer());

	auto* Fourth = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(0, 3600, 100));
	if (!TestNotNull(TEXT("Native destruction fixture spawned"), Fourth)) return false;
	Bind(nullptr, Fourth);
	Fourth->CastArea();
	Fourth->OnCombatMessage.Broadcast(FText::FromString(TEXT("退出前提示")));
	TestTrue(TEXT("Native destruction begins with active display work"), HasWidgetTimer());
	// 公开 Slate 生命周期实际调用 NativeDestruct；不直接绕过它手动清理生产 Timer。
	Slate->ResetWidget();
	TestNull(TEXT("Slate reset releases the actual HUD object after NativeDestruct"), Slate->GetWidgetObject());
	TestFalse(TEXT("NativeDestruct clears both widget timers"), HasWidgetTimer());
	const FString DetachedHealth = HealthText->GetText().ToString();
	const FString DetachedState = StateText->GetText().ToString();
	Damage(Fourth, 10);
	Fourth->CancelCast();
	Fourth->OnCombatMessage.Broadcast(FText::FromString(TEXT("退出后不应显示")));
	TestEqual(TEXT("NativeDestruct unbinds attribute events"), HealthText->GetText().ToString(), DetachedHealth);
	TestEqual(TEXT("NativeDestruct unbinds state events"), StateText->GetText().ToString(), DetachedState);
	TestTrue(TEXT("NativeDestruct unbinds feedback events"), FeedbackText->GetText().IsEmpty());
	TestFalse(TEXT("Late events cannot restart a detached HUD timer"), HasWidgetTimer());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatEscapeHUDHealFailureTest, "Multiplayer.Combat.EscapeHUDHealFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 使用正式撤离角色和 HUD 副本走治疗确认；拒绝原因必须可见，不能提交资源与冷却。
bool FCoopCombatEscapeHUDHealFailureTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Slate is initialized for the saved Escape HUD"), FSlateApplication::IsInitialized())) return false;
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Escape/BP_EscapePlayerVariant.BP_EscapePlayerVariant_C"));
	UClass* WidgetClass = LoadClass<UUserWidget>(nullptr,
		TEXT("/Game/Combat/Escape/WBP_EscapeHUD.WBP_EscapeHUD_C"));
	if (!TestNotNull(TEXT("Formal Escape player loads"), PlayerClass)
		|| !TestNotNull(TEXT("Formal Escape HUD loads"), WidgetClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(PlayerClass, FVector(0, 0, 100));
	auto* Controller = Fixture.World->SpawnActor<APlayerController>();
	if (!TestNotNull(TEXT("Formal Escape player spawned"), Player)
		|| !TestNotNull(TEXT("Local failure-feedback controller exists"), Controller)) return false;
	Controller->Possess(Player);
	if (!TestTrue(TEXT("Failure comes from a genuinely locally controlled player"), Player->IsLocallyControlled())) return false;
	TestEqual(TEXT("Formal player retains its separate Escape rules"), Player->GetCombatConfig().GetPathName(),
		FString(TEXT("/Game/Combat/Escape/DA_EscapeCombat.DA_EscapeCombat")));
	auto* Outer = CreateWidget<UUserWidget>(Fixture.World, WidgetClass);
	if (!TestNotNull(TEXT("Saved Escape HUD is constructed"), Outer)) return false;
	TSharedPtr<SObjectWidget> Slate = StaticCastSharedRef<SObjectWidget>(Outer->TakeWidget());
	if (!TestTrue(TEXT("Normal Slate construction owns the saved Escape HUD"), Slate->GetWidgetObject() == Outer)) return false;
	auto* Widget = Cast<UmultiplayerCombatWidget>(Outer->GetWidgetFromName(TEXT("ARPGCombatVisuals")));
	if (!TestNotNull(TEXT("Actual Escape HUD owns its combat visuals"), Widget)) return false;
	TestEqual(TEXT("Failure feedback uses the Escape visual copy"), Widget->GetClass()->GetPathName(),
		FString(TEXT("/Game/Combat/Escape/WBP_EscapeCombatVisuals.WBP_EscapeCombatVisuals_C")));
	UFunction* BindFunction = Widget->FindFunction(TEXT("BindPawn"));
	if (!TestNotNull(TEXT("Formal HUD retains its normal pawn binding"), BindFunction)) return false;
	struct FBindPawnParameters { APawn* PreviousPawn; APawn* NewPawn; } Bind{ nullptr, Player };
	Widget->ProcessEvent(BindFunction, &Bind);
	auto* Icon = Cast<UImage>(Widget->GetWidgetFromName(TEXT("HealIcon")));
	auto* Feedback = Cast<UTextBlock>(Widget->GetWidgetFromName(TEXT("FeedbackText")));
	auto* Panel = Cast<UBorder>(Widget->GetWidgetFromName(TEXT("FeedbackPanel")));
	if (!TestNotNull(TEXT("Formal healing icon exists"), Icon)
		|| !TestNotNull(TEXT("Formal failure message exists"), Feedback)
		|| !TestNotNull(TEXT("Formal failure panel exists"), Panel)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), Player->GetMaxHealth());
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
	const auto HasHUDTimer = [&]()
	{
		GLog->FlushThreadedLogs();
		FStringOutputDevice Output;
		GLog->AddOutputDevice(&Output);
		Fixture.World->GetTimerManager().ListTimers();
		GLog->FlushThreadedLogs();
		GLog->RemoveOutputDevice(&Output);
		return Output.Contains(Widget->GetPathName(), ESearchCase::CaseSensitive);
	};
	TestTrue(TEXT("Healing icon reflects ready resources without promising a target"), Icon->GetColorAndOpacity().Equals(FLinearColor::White));
	TestFalse(TEXT("Idle formal HUD has no refresh or feedback timer"), HasHUDTimer());
	const FVector OutsideRange(Player->GetSkillRule(EMultiplayerCombatSkill::Heal).Range + 200.f, 0, 100);
	auto* Teammate = Fixture.Character(PlayerClass, OutsideRange);
	if (!TestNotNull(TEXT("Independent teammate exists for idle target changes"), Teammate)) return false;
	Teammate->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), Teammate->GetMaxHealth() - 30.f);
	TestFalse(TEXT("An injured teammate outside range does not qualify healing"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	Teammate->SetActorLocation(FVector(180, 0, 100));
	TestTrue(TEXT("A teammate entering range immediately qualifies the production target query"),
		Player->GetHealTarget() == Teammate && Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	TestTrue(TEXT("Without an owner event or cooldown timer healing cannot remain falsely dimmed"), Icon->GetColorAndOpacity().Equals(FLinearColor::White));
	TestFalse(TEXT("Teammate movement does not introduce HUD polling"), HasHUDTimer());
	Teammate->SetActorLocation(OutsideRange);
	TestFalse(TEXT("A teammate leaving range immediately loses healing qualification"), Player->CheckSkillAvailable(EMultiplayerCombatSkill::Heal));
	TestTrue(TEXT("Resource-only icon brightness does not cache obsolete target eligibility"), Icon->GetColorAndOpacity().Equals(FLinearColor::White));
	TestEqual(TEXT("Idle target changes never create a healing cooldown"), Player->GetHealCooldownRemaining(), 0.f);
	TestEqual(TEXT("Idle target changes never charge energy"), Player->GetEnergy(), 100.f);
	TestFalse(TEXT("Target changes leave the event-driven HUD idle"), HasHUDTimer());
	Player->BeginSlotTargeting(2);
	Player->ConfirmSkillTargeting();
	TestTrue(TEXT("Rejected full-health E explains both full health and the missing injured teammate"),
		Feedback->GetText().ToString().Contains(TEXT("生命已满")) && Feedback->GetText().ToString().Contains(TEXT("受伤队友")));
	TestTrue(TEXT("Rejected E displays the actual feedback panel"), Panel->GetVisibility() == ESlateVisibility::HitTestInvisible);
	for (const UWidget* Parent = Panel; Parent; Parent = Parent->GetParent())
		TestTrue(TEXT("Formal feedback is not under a hidden or collapsed parent"),
			Parent->GetVisibility() != ESlateVisibility::Hidden && Parent->GetVisibility() != ESlateVisibility::Collapsed);
	TestEqual(TEXT("No-target E does not charge energy"), Player->GetEnergy(), 100.f);
	TestEqual(TEXT("No-target E does not start a cooldown"), Player->GetHealCooldownRemaining(), 0.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), Player->GetMaxHealth() - 10.f);
	TestTrue(TEXT("Injury keeps the resource-ready healing icon bright"), Icon->GetColorAndOpacity().Equals(FLinearColor::White));
	TestFalse(TEXT("A missing-target reason on an injured client does not claim full health"),
		Player->GetSkillFailureText(EMultiplayerCombatSkill::Heal, CombatTags::Failure_NoHealTarget).ToString().Contains(TEXT("生命已满")));
	const float InsufficientEnergy = Player->GetCombatConfig().Heal.Cost - 1.f;
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), InsufficientEnergy);
	TestTrue(TEXT("Insufficient energy still dims healing through its owner attribute event"), Icon->GetColorAndOpacity().Equals(FLinearColor(.4f, .4f, .4f, 1.f)));
	Player->BeginSlotTargeting(2);
	Player->ConfirmSkillTargeting();
	TestTrue(TEXT("A different E refusal shows its resource reason"), Feedback->GetText().ToString().Contains(TEXT("能量不足")));
	TestEqual(TEXT("Rejected resource E leaves the balance unchanged"), Player->GetEnergy(), InsufficientEnergy);
	TestEqual(TEXT("Rejected resource E leaves health unchanged"), Player->GetHealth(), Player->GetMaxHealth() - 10.f);
	TestEqual(TEXT("Rejected resource E leaves cooldown at zero"), Player->GetHealCooldownRemaining(), 0.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAnimationIntegrationTest, "Multiplayer.Combat.AnimationIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 检查保存的实际资产与运行时播放，而非只检查生成脚本；这不替代真人观看动作衔接。
bool FCoopCombatAnimationIntegrationTest::RunTest(const FString& Parameters)
{
	const FString Folder = TEXT("/Game/Combat/Presentation/Animations/");
	for (const TCHAR* Name : {TEXT("AM_Attack"), TEXT("AM_Heal"), TEXT("AM_Death"), TEXT("AM_AreaCast"), TEXT("AM_Hit")})
	{
		UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, *(Folder + Name + TEXT(".") + Name));
		if (!TestNotNull(TEXT("Saved action montage exists"), Montage)) return false;
		if (!TestEqual(TEXT("Each montage has exactly one slot track"), Montage->SlotAnimTracks.Num(), 1)) return false;
		const bool bFullBody = Montage->GetName() == TEXT("AM_Death") || Montage->GetName() == TEXT("AM_Hit");
		TestEqual(TEXT("Only death and hit replace the full-body pose"), Montage->SlotAnimTracks[0].SlotName,
			FName(bFullBody ? TEXT("DefaultSlot") : TEXT("UpperBody")));
		float NaturalLength = 0;
		for (const FAnimSegment& Segment : Montage->SlotAnimTracks[0].AnimTrack.AnimSegments)
		{
			UAnimSequenceBase* Sequence = Segment.GetAnimReference();
			if (!TestNotNull(TEXT("Segment references a real animation"), Sequence)) return false;
			TestTrue(TEXT("Montage segments retain natural playback speed"), FMath::IsNearlyEqual(Segment.AnimPlayRate, 1.f));
			TestTrue(TEXT("Segment is not compressed by cropping its end"), FMath::IsNearlyEqual(Segment.AnimEndTime, Sequence->GetPlayLength(), .001f));
			NaturalLength += Sequence->GetPlayLength();
		}
		TestTrue(TEXT("Montage preserves complete source duration"), FMath::IsNearlyEqual(NaturalLength, Montage->GetPlayLength(), .001f));
	}
#if WITH_EDITOR
	for (const TCHAR* Name : {TEXT("ABP_CombatLocomotion"), TEXT("ABP_CombatQuinn")})
	{
		UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *(Folder + Name + TEXT(".") + Name));
		if (!TestNotNull(TEXT("Actual combat locomotion blueprint exists"), Blueprint)) return false;
		TestTrue(TEXT("Locomotion blueprint has a valid generated class"), Blueprint->GeneratedClass && Blueprint->Status != BS_Error);
		TArray<UEdGraph*> Graphs;
		Blueprint->GetAllGraphs(Graphs);
		int32 UpperSlots = 0, FullSlots = 0, UpperLayers = 0;
		for (const UEdGraph* Graph : Graphs)
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (auto* Slot = Cast<UAnimGraphNode_Slot>(Node))
				{
					UpperSlots += Slot->Node.SlotName == TEXT("UpperBody");
					FullSlots += Slot->Node.SlotName == TEXT("DefaultSlot");
				}
				if (auto* Layer = Cast<UAnimGraphNode_LayeredBoneBlend>(Node))
					for (const FInputBlendPose& Pose : Layer->Node.LayerSetup)
						for (const FBranchFilter& Branch : Pose.BranchFilters)
							UpperLayers += Branch.BoneName == TEXT("spine_01");
			}
		const bool bInherited = Blueprint->GetName() == TEXT("ABP_CombatQuinn");
		const int32 ExpectedNodes = bInherited ? 0 : 1;
		TestEqual(TEXT("Child inherits its action slot without adding a duplicate"), UpperSlots, ExpectedNodes);
		TestEqual(TEXT("Full-body slot exists only once in the animation hierarchy"), FullSlots, ExpectedNodes);
		TestEqual(TEXT("Spine layering exists only once in the animation hierarchy"), UpperLayers, ExpectedNodes);
		if (bInherited) TestEqual(TEXT("Quinn inherits the validated combat locomotion blueprint"),
			Blueprint->ParentClass->GetName(), FString(TEXT("ABP_CombatLocomotion_C")));
	}
#endif
	FCombatPresentationWorld Fixture;
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Actual saved combat player exists"), PlayerClass)) return false;
	auto* Player = Fixture.Character(PlayerClass, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Actual animated pawn spawned"), Player)) return false;
	UAnimInstance* Animation = Player->GetMesh()->GetAnimInstance();
	if (!TestNotNull(TEXT("Actual pawn instantiates its animation blueprint"), Animation)) return false;
	Player->Attack();
	UAnimMontage* Attack = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	TestTrue(TEXT("Attack actually starts its montage through GAS"), Animation->Montage_IsPlaying(Attack));
	TestTrue(TEXT("Short attack cooldown does not speed up the entire animation"), FMath::IsNearlyEqual(Animation->Montage_GetPlayRate(Attack), 1.f));
	Player->CastArea();
	UAnimMontage* Area = Player->GetActionMontage(EMultiplayerCombatAnimation::Area);
	if (!TestNotNull(TEXT("Actual pawn uses the area montage"), Area)) return false;
	const int32 Release = Area->GetSectionIndex(TEXT("Release"));
	if (!TestTrue(TEXT("Area montage contains a release boundary"), Release != INDEX_NONE)) return false;
	const float Windup = Area->CompositeSections[Release].GetTime();
	const auto& Rule = Player->GetCombatConfig().Area;
	TestTrue(TEXT("Saved config windup follows the complete source animation"), FMath::IsNearlyEqual(Rule.Windup, Windup, .001f));
	TestTrue(TEXT("Saved config recovery retains release and recovery clips"), FMath::IsNearlyEqual(Rule.Recovery, Area->GetPlayLength() - Windup, .001f));
	TestTrue(TEXT("Area starts at natural speed with the saved config"), FMath::IsNearlyEqual(Animation->Montage_GetPlayRate(Area), 1.f, .001f));
	Player->CancelCast();
	TestFalse(TEXT("Cancel releases the gameplay casting lock"), Player->IsCasting());
	TestFalse(TEXT("Cancel stops the currently playing cast montage"), Animation->Montage_IsPlaying(Area));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCascadeLifecycleTest, "Multiplayer.Combat.CascadeLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

// 用迁入的真实 Cascade 检查附着、二选一与收尾；需要渲染环境，但不把组件检查当作画面验收。
bool FCoopCombatCascadeLifecycleTest::RunTest(const FString& Parameters)
{
	UParticleSystem* Cascade = LoadObject<UParticleSystem>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/ReplenishmentCast/P_AscendTakeoffFalle.P_AscendTakeoffFalle"));
	if (!TestNotNull(TEXT("Imported healing Cascade exists"), Cascade)) return false;
	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Cascade fixture player spawned"), Player)) return false;
	auto* Config = NewObject<UmultiplayerCombatConfig>(Player);
	Config->DefenseFeedback.CascadeEffect = Cascade;
	Config->DefenseFeedback.Effect = NewObject<UNiagaraSystem>(Config);
	Config->ProjectileCascadeEffect = Cascade;
	Config->ProjectileEffect = Config->DefenseFeedback.Effect;
	auto* ConfigProperty = FindFProperty<FObjectPropertyBase>(Player->GetClass(), TEXT("CombatConfig"));
	if (!TestNotNull(TEXT("Character config remains selectable"), ConfigProperty)) return false;
	ConfigProperty->SetObjectPropertyValue_InContainer(Player, Config);

	auto* Cue = Fixture.World->SpawnActor<AmultiplayerCombatCue>();
	if (!TestNotNull(TEXT("Cascade cue spawned"), Cue)) return false;
	Cue->GameplayCueTag = CombatTags::Cue_Defense;
	FGameplayCueParameters CueParameters;
	Cue->OnActive_Implementation(Player, CueParameters);
	const auto Loop = [&]() -> UParticleSystemComponent*
	{
		const auto* Property = FindFProperty<FObjectPropertyBase>(Cue->GetClass(), TEXT("LoopCascadeEffect"));
		return Property ? Cast<UParticleSystemComponent>(Property->GetObjectPropertyValue_InContainer(Cue)) : nullptr;
	};
	UParticleSystemComponent* First = Loop();
	if (!TestNotNull(TEXT("Active cue creates its Cascade component"), First)) return false;
	TestTrue(TEXT("Cue Cascade uses the selected imported template"), First->Template == Cascade);
	TestTrue(TEXT("Persistent cue follows the character root"), First->GetAttachParent() == Player->GetRootComponent());
	const auto* NiagaraProperty = FindFProperty<FObjectPropertyBase>(Cue->GetClass(), TEXT("LoopEffect"));
	TestTrue(TEXT("Selecting Cascade suppresses the configured Niagara"), NiagaraProperty
		&& NiagaraProperty->GetObjectPropertyValue_InContainer(Cue) == nullptr);
	Cue->WhileActive_Implementation(Player, CueParameters);
	TestTrue(TEXT("WhileActive does not duplicate the persistent component"), Loop() == First);
	const FVector Before = First->GetComponentLocation();
	Player->SetActorLocation(Player->GetActorLocation() + FVector(200, 0, 0));
	TestTrue(TEXT("Attached Cascade actually moves with its target"), First->GetComponentLocation().Equals(Before + FVector(200, 0, 0), .1f));
	Cue->OnRemove_Implementation(Player, CueParameters);
	TestNull(TEXT("Effect removal releases the stored component"), Loop());
	TestFalse(TEXT("Effect removal unregisters the old particle"), First->IsRegistered());
	Cue->WhileActive_Implementation(Player, CueParameters);
	UParticleSystemComponent* Restored = Loop();
	if (!TestNotNull(TEXT("Late relevance can restore a fresh persistent component"), Restored)) return false;
	Cue->Recycle();
	TestNull(TEXT("Cue recycling clears its persistent presentation"), Loop());
	TestFalse(TEXT("Cue recycling unregisters the restored particle"), Restored->IsRegistered());
	Cue->WhileActive_Implementation(Player, CueParameters);
	UParticleSystemComponent* EndedWithCue = Loop();
	if (!TestNotNull(TEXT("World-exit fixture has an active effect"), EndedWithCue)) return false;
	// 普通结束走 Cue 回收；这里模拟关卡移除的 EndPlay，不用 Destroy 违反 GAS 的回收约定。
	Cue->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
	TestFalse(TEXT("Cue EndPlay cleans up an effect even without OnRemove"), EndedWithCue->IsRegistered());

	const FTransform Transform(FRotator::ZeroRotator, FVector(0, 500, 150));
	auto* Projectile = Fixture.World->SpawnActorDeferred<AmultiplayerCombatProjectile>(AmultiplayerCombatProjectile::StaticClass(),
		Transform, Player, Player, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Cascade projectile spawned"), Projectile)) return false;
	Projectile->Initialize(Player, 25.f, 1600.f);
	Projectile->FinishSpawning(Transform);
	UParticleSystemComponent* Trail = Projectile->FindComponentByClass<UParticleSystemComponent>();
	if (!TestNotNull(TEXT("Projectile creates its Cascade trail"), Trail)) return false;
	TestTrue(TEXT("Projectile trail follows the collision root"), Trail->GetAttachParent() == Projectile->GetRootComponent());
	TestNull(TEXT("Projectile does not also create its configured Niagara"), Projectile->FindComponentByClass<UNiagaraComponent>());
	TInlineComponentArray<UParticleSystemComponent*> Trails;
	Projectile->GetComponents(Trails);
	TestEqual(TEXT("Projectile owns exactly one trail"), Trails.Num(), 1);
	const FVector TrailBefore = Trail->GetComponentLocation();
	Projectile->SetActorLocation(Projectile->GetActorLocation() + FVector(200, 0, 0));
	TestTrue(TEXT("Projectile trail actually follows flight movement"), Trail->GetComponentLocation().Equals(TrailBefore + FVector(200, 0, 0), .1f));
	Projectile->Destroy();
	TestFalse(TEXT("Projectile EndPlay destroys its trail"), Trail->IsRegistered());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatGroundMarkerTest, "Multiplayer.Combat.GroundMarker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 用真实材质与 Cue 检查地面标记的世界坐标、投射法线和回收，不把组件属性检查说成画面验收。
bool FCoopCombatGroundMarkerTest::RunTest(const FString& Parameters)
{
	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr,
		TEXT("/Game/Combat/Presentation/Materials/M_CombatAreaMarker.M_CombatAreaMarker"));
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Saved area marker material exists"), Material)
		|| !TestNotNull(TEXT("Actual marker player blueprint exists"), PlayerClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(PlayerClass, FVector(1000, 2000, 100));
	auto* Cue = Fixture.World->SpawnActor<AmultiplayerCombatCue>();
	if (!TestNotNull(TEXT("Non-origin marker caster spawned"), Player)
		|| !TestNotNull(TEXT("Actual marker cue spawned"), Cue)) return false;
	const auto& Config = Player->GetCombatConfig();
	TestTrue(TEXT("The actual player config selects the saved marker material"), Config.AreaMarkerMaterial.Get() == Material);
	Cue->GameplayCueTag = CombatTags::Cue_Casting;
	const auto ParametersAt = [&](const FVector& Point, const FVector& Normal)
	{
		FGameplayCueParameters Result;
		Result.EffectContext = Player->GetAbilitySystemComponent()->MakeEffectContext();
		FHitResult Hit;
		Hit.bBlockingHit = true;
		Hit.Location = Hit.ImpactPoint = Point;
		Hit.Normal = Hit.ImpactNormal = Normal;
		Result.EffectContext.AddHitResult(Hit);
		Result.Location = Player->GetActorLocation();
		Result.RawMagnitude = Config.AreaRadius;
		return Result;
	};
	const auto Markers = [&]
	{
		TArray<UDecalComponent*> Result;
		for (TObjectIterator<UDecalComponent> It; It; ++It)
			if (It->GetWorld() == Fixture.World && It->IsRegistered()) Result.Add(*It);
		return Result;
	};
	const FVector PredictedPoint(1100, 2400, 2);
	// 零法线必须回退到 Up；故意让 Parameters.Location 与 Hit 不同，防止误用施法者位置。
	const FGameplayCueParameters Predicted = ParametersAt(PredictedPoint, FVector::ZeroVector);
	Cue->OnActive_Implementation(Player, Predicted);
	TArray<UDecalComponent*> Active = Markers();
	if (!TestEqual(TEXT("Casting creates exactly one real ground decal"), Active.Num(), 1)) return false;
	UDecalComponent* First = Active[0];
	TestTrue(TEXT("The decal uses the saved marker material"), First->GetDecalMaterial() == Material);
	TestTrue(TEXT("The ground marker takes its center from the effect-context hit"),
		First->GetComponentLocation().Equals(PredictedPoint, .1f));
	TestTrue(TEXT("Missing hit normal projects down toward the ground"), First->GetForwardVector().Equals(-FVector::UpVector, .001f));
	TestTrue(TEXT("Marker projection extents follow the configured area radius"),
		First->DecalSize.Equals(FVector(32, Config.AreaRadius, Config.AreaRadius), .001f));
	TestNull(TEXT("A ground marker is not attached to the moving caster"), First->GetAttachParent());
	Player->SetActorLocation(Player->GetActorLocation() + FVector(300, 150, 0));
	Fixture.Advance();
	TestTrue(TEXT("Moving the caster cannot drag the fixed ground center"),
		First->GetComponentLocation().Equals(PredictedPoint, .1f));

	const FVector AuthorityPoint(1200, 2450, 30);
	const FVector AuthorityNormal = FVector(.2f, -.3f, 1.f).GetSafeNormal();
	const FGameplayCueParameters Authority = ParametersAt(AuthorityPoint, AuthorityNormal);
	Cue->WhileActive_Implementation(Player, Authority);
	Active = Markers();
	TestEqual(TEXT("An authoritative context update does not duplicate the decal"), Active.Num(), 1);
	TestTrue(TEXT("The existing predicted marker is updated rather than replaced"), Active.Num() == 1 && Active[0] == First);
	TestTrue(TEXT("The marker adopts the authoritative ground center"), First->GetComponentLocation().Equals(AuthorityPoint, .1f));
	TestTrue(TEXT("The decal projection follows the authoritative slope normal"),
		First->GetForwardVector().Equals(-AuthorityNormal, .001f));
	Cue->WhileActive_Implementation(Player, Authority);
	TestEqual(TEXT("Repeated WhileActive notifications remain idempotent"), Markers().Num(), 1);
	Cue->OnRemove_Implementation(Player, Authority);
	TestFalse(TEXT("Removing casting unregisters the previous marker"), First->IsRegistered());
	TestEqual(TEXT("Removing casting leaves no registered marker"), Markers().Num(), 0);
	Fixture.Advance();
	TestEqual(TEXT("No marker returns merely from advancing after removal"), Markers().Num(), 0);

	// 新的 WhileActive 是合法的晚相关恢复；不能把它误判成已取消异步回调的复活。
	Cue->WhileActive_Implementation(Player, Authority);
	Active = Markers();
	if (!TestEqual(TEXT("Late relevance can restore one marker"), Active.Num(), 1)) return false;
	UDecalComponent* Restored = Active[0];
	Cue->Recycle();
	TestFalse(TEXT("Cue recycling unregisters its marker"), Restored->IsRegistered());
	TestEqual(TEXT("Cue recycling leaves no marker component"), Markers().Num(), 0);
	Cue->WhileActive_Implementation(Player, Authority);
	Active = Markers();
	if (!TestEqual(TEXT("World-exit fixture has one active marker"), Active.Num(), 1)) return false;
	UDecalComponent* EndedWithCue = Active[0];
	Cue->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
	TestFalse(TEXT("World removal unregisters a marker without requiring OnRemove"), EndedWithCue->IsRegistered());
	TestEqual(TEXT("World removal leaves no marker component"), Markers().Num(), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHealRecipientFeetAndFollowTest, "Multiplayer.Combat.HealRecipientFeetAndFollow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

// 治疗素材已经自带向上偏移，组件原点必须放在受治疗者脚底；实际 Niagara 在移动和缩放后也不能重复抬高。
bool FCoopCombatHealRecipientFeetAndFollowTest::RunTest(const FString& Parameters)
{
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Actual healing player blueprint exists"), PlayerClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Caster = Fixture.Character(PlayerClass, FVector(1000, 2000, 300));
	auto* Recipient = Fixture.Character(PlayerClass, FVector(1300, 2000, 300));
	if (!TestNotNull(TEXT("Healing caster uses the actual player"), Caster)
		|| !TestNotNull(TEXT("Healing origin probe uses a different actual recipient"), Recipient)) return false;
	const auto& Config = Caster->GetCombatConfig();
	UNiagaraSystem* Shield = Config.HealFeedback.Effect.LoadSynchronous();
	if (!TestNotNull(TEXT("Actual E Niagara shield is loaded"), Shield)) return false;
	Config.HealFeedback.Sound.LoadSynchronous();
	TestEqual(TEXT("Healing uses the calibrated project shield without unrelated ground waves"), Shield->GetPathName(),
		FString(TEXT("/Game/Combat/Presentation/Effects/NS_CombatDefenseShield.NS_CombatDefenseShield")));
	TestTrue(TEXT("Actual E does not replace Niagara with a Cascade fallback"), Config.HealFeedback.CascadeEffect.IsNull());
	TestTrue(TEXT("Actual Q has no Niagara particle"), Config.DefenseFeedback.Effect.IsNull());
	TestTrue(TEXT("Actual Q has no Cascade particle"), Config.DefenseFeedback.CascadeEffect.IsNull());
	UGameplayCueManager* Manager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
	if (!TestNotNull(TEXT("Real gameplay cue manager exists"), Manager)) return false;
#if WITH_EDITOR
	Manager->LoadNotifyForEditorPreview(CombatTags::Cue_Heal);
#endif
	const auto Shields = [&]()
	{
		TArray<UNiagaraComponent*> Result;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
			if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->GetAsset() == Shield) Result.Add(*It);
		return Result;
	};
	for (const float Scale : { .75f, 1.5f })
	{
		Recipient->SetActorLocation(FVector(1300, 2000, 300));
		Recipient->SetActorScale3D(FVector(Scale));
		Recipient->GetAbilitySystemComponent()->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 40.f);
		auto* ASC = Caster->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 100.f);
		ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(CombatTags::Cooldown_Heal));
		if (!TestTrue(TEXT("Authority selects the injured recipient"), Caster->GetHealTarget() == Recipient)) return false;
		int32 HealRoutes = 0;
		const FDelegateHandle Route = Manager->OnGameplayCueRouted().AddLambda(
			[&](AActor* Target, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters&, EGameplayCueExecutionOptions)
		{
			if (Target == Recipient && Tag == CombatTags::Cue_Heal && Event == EGameplayCueEvent::Executed) ++HealRoutes;
		});
		Caster->Heal();
		Manager->OnGameplayCueRouted().Remove(Route);
		TestEqual(TEXT("Real healing dispatches exactly one recipient result cue"), HealRoutes, 1);
		TestEqual(TEXT("Real healing settles on the recipient"), Recipient->GetHealth(), FMath::Min(40.f + Config.Heal.Amount, Recipient->GetMaxHealth()));
		const auto Active = Shields();
		if (!TestEqual(TEXT("Real heal creates exactly one actual shield"), Active.Num(), 1)) return false;
		UNiagaraComponent* First = Active[0];
		const FVector Feet = Recipient->GetActorLocation() - Recipient->GetActorUpVector()
			* Recipient->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TestTrue(TEXT("E uses the actual configured Niagara system"), First->GetAsset() == Shield);
		TestTrue(TEXT("E origin is at scaled recipient feet without repeating the authored upward offset"), First->GetComponentLocation().Equals(Feet, .1f));
		TestTrue(TEXT("E remains attached to the recipient root"), First->GetAttachParent() == Recipient->GetRootComponent());
		TestTrue(TEXT("Healing attachment preserves absolute authored scale"), First->IsUsingAbsoluteScale());
		TestTrue(TEXT("Recipient scale does not multiply the authored shield"), First->GetComponentScale().Equals(FVector::OneVector, .001f));
		TestTrue(TEXT("Healing uses the short shield duration"), FMath::IsNearlyEqual(First->GetOverrideParameters().GetParameterValue<float>(
			FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), TEXT("User.Duration"))), .8f));
		const FVector Delta(260, -130, 50);
		Recipient->SetActorLocation(Recipient->GetActorLocation() + Delta);
		TestTrue(TEXT("Moving the recipient immediately moves the shield feet origin"), First->GetComponentLocation().Equals(Feet + Delta, .1f));
		TestTrue(TEXT("Following the recipient retains world scale one"), First->GetComponentScale().Equals(FVector::OneVector, .001f));
		TWeakObjectPtr<UNiagaraComponent> Expiring = First;
		for (int32 Step = 0; Step < 55; ++Step) Fixture.Advance();
		TestTrue(TEXT("Transient heal expiry unregisters its Niagara component"), !Expiring.IsValid() || !Expiring->IsRegistered());
		TestEqual(TEXT("Heal expiry leaves no shield particle behind"), Shields().Num(), 0);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatEnemyHealthPresentationTest, "Multiplayer.Combat.EnemyHealthPresentation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 复用敌人骨骼、动画和条形布局，但属性来自自己的 GAS；真实伤害后不推进一帧也应立即更新。
bool FCoopCombatEnemyHealthPresentationTest::RunTest(const FString& Parameters)
{
	if (!TestTrue(TEXT("Enemy health construction has an initialized Slate application"), FSlateApplication::IsInitialized())) return false;
	UClass* EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	if (!TestNotNull(TEXT("Actual enemy blueprint exists"), EnemyClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Enemy = Cast<AmultiplayerCombatEnemy>(Fixture.Character(EnemyClass, FVector(0, 0, 100)));
	if (!TestNotNull(TEXT("Actual enemy is spawned without a source-pack AI controller"), Enemy)) return false;
	USkeleton* SourceSkeleton = LoadObject<USkeleton>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Meshes/SK_CombatMannequin.SK_CombatMannequin"));
	USkeletalMesh* SourceMesh = LoadObject<USkeletalMesh>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Meshes/SKM_CombatMannequin.SKM_CombatMannequin"));
	UPhysicsAsset* SourcePhysics = LoadObject<UPhysicsAsset>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Rigs/PA_Mannequin.PA_Mannequin"));
	UClass* SourcePostProcess = LoadClass<UAnimInstance>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/Characters/Heroes/Mannequins/Rigs/ABP_Manny_PostProcess.ABP_Manny_PostProcess_C"));
	if (!TestNotNull(TEXT("Imported enemy skeleton exists"), SourceSkeleton)
		|| !TestNotNull(TEXT("Imported complete enemy mesh exists"), SourceMesh)
		|| !TestNotNull(TEXT("Imported enemy physics asset exists"), SourcePhysics)
		|| !TestNotNull(TEXT("Imported complete enemy post-process exists"), SourcePostProcess)
		|| !TestNotNull(TEXT("Enemy has its imported skeletal mesh"), Enemy->GetMesh()->GetSkeletalMeshAsset())) return false;
	TestTrue(TEXT("Enemy uses the complete source mesh rather than its Simple version"), Enemy->GetMesh()->GetSkeletalMeshAsset() == SourceMesh);
	TestTrue(TEXT("Enemy directly reuses the source skeleton"), Enemy->GetMesh()->GetSkeletalMeshAsset()->GetSkeleton() == SourceSkeleton);
	TestTrue(TEXT("Enemy directly reuses the source physics asset"), Enemy->GetMesh()->GetPhysicsAsset() == SourcePhysics);
	TestTrue(TEXT("Complete source mesh keeps its source post-process class"), SourceMesh->GetPostProcessAnimBlueprint().Get() == SourcePostProcess);
	TestTrue(TEXT("Enemy does not override or disable the source post-process"), !Enemy->GetMesh()->GetDisablePostProcessBlueprint()
		&& Enemy->GetMesh()->GetPostProcessAnimBPClassToBeUsed().Get() == SourcePostProcess);
	UAnimInstance* Animation = Enemy->GetMesh()->GetAnimInstance();
	if (!TestNotNull(TEXT("Enemy instantiates its adapted animation blueprint"), Animation)) return false;
	TestTrue(TEXT("Enemy animation reads CMC through our native parent"), Animation->IsA<UmultiplayerCombatAnimInstance>());
	UStaticMeshComponent* Weapon = nullptr;
	TArray<UStaticMeshComponent*> StaticMeshes;
	Enemy->GetComponents(StaticMeshes);
	for (UStaticMeshComponent* Component : StaticMeshes)
		if (Component->GetFName() == TEXT("EnemyWeapon")) Weapon = Component;
	if (!TestNotNull(TEXT("Minion has its separate cosmetic weapon component"), Weapon)) return false;
	TestTrue(TEXT("Cosmetic weapon follows the imported right hand"),
		Weapon->GetAttachParent() == Enemy->GetMesh() && Weapon->GetAttachSocketName() == TEXT("FengMaoWeapon_Active")
		&& Enemy->GetMesh()->GetSocketBoneName(Weapon->GetAttachSocketName()) == TEXT("hand_r"));
	TestTrue(TEXT("Weapon mesh cannot independently settle hits or block characters"),
		Weapon->GetCollisionEnabled() == ECollisionEnabled::NoCollision && !Weapon->GetGenerateOverlapEvents());
	for (const TCHAR* PropertyName : { TEXT("AttackMontage"), TEXT("HitMontage"), TEXT("DeathMontage") })
	{
		const auto* Property = FindFProperty<FObjectPropertyBase>(Enemy->GetClass(), PropertyName);
		UAnimMontage* Montage = Property ? Cast<UAnimMontage>(Property->GetObjectPropertyValue_InContainer(Enemy)) : nullptr;
		if (!TestNotNull(TEXT("Enemy action has a compatible montage"), Montage)) return false;
		TestTrue(TEXT("Enemy action montage uses its source skeleton"), Montage->GetSkeleton() == SourceSkeleton);
		TestEqual(TEXT("Minion montage has no source-pack business notifies"), Montage->Notifies.Num(), 0);
		for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
			for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
			{
				auto* Sequence = Cast<UAnimSequence>(Segment.GetAnimReference());
				if (!TestNotNull(TEXT("Minion montage uses its adapted source sequence"), Sequence)) return false;
				TestTrue(TEXT("Minion action comes from the melee asset copies"),
					Sequence->GetName().StartsWith(TEXT("AS_Enemy_Melee_")));
				TestEqual(TEXT("Minion action sequence has no source-pack gameplay callbacks"), Sequence->Notifies.Num(), 0);
				TestFalse(TEXT("Minion animation does not override CMC movement with root motion"), Sequence->bEnableRootMotion);
			}
	}
	TestNull(TEXT("Ordinary minion has no player's healing montage"), Enemy->GetActionMontage(EMultiplayerCombatAnimation::Heal));
	TestNull(TEXT("Ordinary minion has no player's area-cast montage"), Enemy->GetActionMontage(EMultiplayerCombatAnimation::Area));
	UBlendSpace* MeleeLocomotion = LoadObject<UBlendSpace>(nullptr,
		TEXT("/Game/Combat/Presentation/ARPG/EnemySource/BS_EnemyMeleeLocomotion.BS_EnemyMeleeLocomotion"));
	if (!TestNotNull(TEXT("Minion uses its dedicated one-handed locomotion blend space"), MeleeLocomotion)) return false;
	for (const FBlendSample& Sample : MeleeLocomotion->GetBlendSamples())
		TestTrue(TEXT("One-handed locomotion only uses adapted melee samples, not the old magic movement"),
			Sample.Animation && Sample.Animation->GetName().StartsWith(TEXT("AS_Enemy_Melee_")));
	UWidgetComponent* Component = Enemy->FindComponentByClass<UWidgetComponent>();
	if (!TestNotNull(TEXT("Enemy owns a real health widget component"), Component)) return false;
	TestTrue(TEXT("Health widget is world-space and follows the network-smoothed body"), Component->GetWidgetSpace() == EWidgetSpace::World
		&& Component->GetAttachParent() == Enemy->GetMesh() && Component->GetManuallyRedraw());
	auto* Outer = Cast<UmultiplayerEnemyHealthWidget>(Component->GetUserWidgetObject());
	if (!TestNotNull(TEXT("Enemy BeginPlay created the actual native-backed health layout"), Outer)) return false;
	TSharedPtr<SObjectWidget> Slate = StaticCastSharedRef<SObjectWidget>(Outer->TakeWidget());
	auto* Inner = Cast<UmultiplayerEnemyHealthWidget>(Outer->GetWidgetFromName(TEXT("Health")));
	if (!TestNotNull(TEXT("Source outer layout contains our adapted inner health widget"), Inner)) return false;
	auto* Bar = Cast<UProgressBar>(Inner->GetWidgetFromName(TEXT("HealthBar")));
	auto* BackBar = Cast<UProgressBar>(Inner->GetWidgetFromName(TEXT("LerpAttributeBar")));
	auto* Text = Cast<UTextBlock>(Inner->GetWidgetFromName(TEXT("HealthText")));
	if (!TestNotNull(TEXT("Enemy has its real source-style foreground bar"), Bar)
		|| !TestNotNull(TEXT("Enemy has its real source-style background bar"), BackBar)
		|| !TestNotNull(TEXT("Enemy has numeric health text"), Text)) return false;
	TestEqual(TEXT("Construction reads the initial GAS snapshot"), Text->GetText().ToString(), FString::Printf(TEXT("%.0f / %.0f"), Enemy->GetHealth(), Enemy->GetMaxHealth()));
	TestTrue(TEXT("Only the inner widget subscribes to health events"), Enemy->OnCombatStatsChanged.GetAllObjects().Contains(Inner)
		&& !Enemy->OnCombatStatsChanged.GetAllObjects().Contains(Outer));
	const auto Damage = [](AmultiplayerCombatCharacter* Target, float Amount)
	{
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Amount);
		ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	};
	Damage(Enemy, 25.f);
	TestTrue(TEXT("GAS damage updates the foreground immediately without a display tick"), FMath::IsNearlyEqual(Bar->GetPercent(), Enemy->GetHealth() / Enemy->GetMaxHealth()));
	TestTrue(TEXT("The source-style background is updated without a source timer"), FMath::IsNearlyEqual(BackBar->GetPercent(), Bar->GetPercent()));
	TestEqual(TEXT("GAS damage immediately updates numeric health"), Text->GetText().ToString(), FString::Printf(TEXT("%.0f / %.0f"), Enemy->GetHealth(), Enemy->GetMaxHealth()));

	auto* Replacement = Fixture.Character(EnemyClass, FVector(0, 1000, 100));
	if (!TestNotNull(TEXT("Health rebind target spawned"), Replacement)) return false;
	Outer->BindCharacter(Replacement);
	TestFalse(TEXT("Rebinding removes the old enemy's subscription"), Enemy->OnCombatStatsChanged.GetAllObjects().Contains(Inner));
	const FString FreshText = Text->GetText().ToString();
	Damage(Enemy, 10.f);
	TestEqual(TEXT("The previous enemy can no longer change the rebound bar"), Text->GetText().ToString(), FreshText);
	Damage(Replacement, 1000.f);
	TestFalse(TEXT("Real GAS damage commits replacement death"), Replacement->IsAlive());
	TestTrue(TEXT("Death immediately hides the actual inner health layout"), Inner->GetVisibility() == ESlateVisibility::Collapsed);
	TestTrue(TEXT("Death immediately empties the health bar"), FMath::IsNearlyZero(Bar->GetPercent()));
	GLog->FlushThreadedLogs();
	FStringOutputDevice Timers;
	GLog->AddOutputDevice(&Timers);
	Fixture.World->GetTimerManager().ListTimers();
	GLog->FlushThreadedLogs();
	GLog->RemoveOutputDevice(&Timers);
	TestFalse(TEXT("Neither enemy health layout owns a polling timer"), Timers.Contains(Outer->GetPathName(), ESearchCase::CaseSensitive)
		|| Timers.Contains(Inner->GetPathName(), ESearchCase::CaseSensitive));
	Slate->ResetWidget();
	TestFalse(TEXT("NativeDestruct unbinds the inner observer"), Replacement->OnCombatStatsChanged.GetAllObjects().Contains(Inner));
	Outer->BindCharacter(Enemy);
	Enemy->Destroy();
	TestFalse(TEXT("Enemy EndPlay removes its health subscription"), Enemy->OnCombatStatsChanged.GetAllObjects().Contains(Inner));
	TestNull(TEXT("Enemy EndPlay releases the component's user widget"), Component->GetUserWidgetObject());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMinionWeaponAttachmentTest, "Multiplayer.Combat.MinionWeaponAttachment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 在实际小兵的待机和挥击骨骼姿态中检查挂接；只验证随手运动，不把变换测试当作握持与穿插的画面验收。
bool FCoopCombatMinionWeaponAttachmentTest::RunTest(const FString& Parameters)
{
	UClass* EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	UStaticMesh* SourceWeapon = LoadObject<UStaticMesh>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/MeleeCombatSystem/DemoAssets/Meshes/Weapons/Paragon/FengMao_Weapon.FengMao_Weapon"));
	if (!TestNotNull(TEXT("Saved minion blueprint exists for weapon attachment"), EnemyClass)
		|| !TestNotNull(TEXT("Imported polearm mesh exists"), SourceWeapon)) return false;
	FCombatPresentationWorld Fixture;
	auto* Enemy = Fixture.Character(EnemyClass, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Actual minion spawned without a source-pack controller"), Enemy)) return false;
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	UStaticMeshComponent* Weapon = nullptr;
	TInlineComponentArray<UStaticMeshComponent*> StaticMeshes;
	Enemy->GetComponents(StaticMeshes);
	for (UStaticMeshComponent* Component : StaticMeshes)
		if (Component->GetFName() == TEXT("EnemyWeapon")) Weapon = Component;
	if (!TestNotNull(TEXT("Actual minion owns its cosmetic weapon component"), Weapon)) return false;
	TestTrue(TEXT("Minion uses the imported polearm rather than an empty weapon slot"), Weapon->GetStaticMesh() == SourceWeapon);
	const FName GripSocket(TEXT("FengMaoWeapon_Active"));
	TestTrue(TEXT("Polearm uses the source pack's active right-hand grip socket"),
		Weapon->GetAttachParent() == Mesh && Weapon->GetAttachSocketName() == GripSocket
		&& Mesh->DoesSocketExist(GripSocket) && Mesh->GetSocketBoneName(GripSocket) == TEXT("hand_r"));
	TestTrue(TEXT("Polearm cannot block or independently settle melee hits"),
		Weapon->GetCollisionEnabled() == ECollisionEnabled::NoCollision && !Weapon->GetGenerateOverlapEvents());
	if (!TestTrue(TEXT("Actual skeletal mesh contains the attachment bone"), Mesh->GetBoneIndex(TEXT("hand_r")) != INDEX_NONE)) return false;
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	auto* Animation = Cast<UmultiplayerCombatAnimInstance>(Mesh->GetAnimInstance());
	UAnimMontage* Attack = Enemy->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	if (!TestNotNull(TEXT("Actual minion runs its native-backed animation blueprint"), Animation)
		|| !TestNotNull(TEXT("Actual minion has its adapted melee attack montage"), Attack)) return false;
	const FTransform Grip = Weapon->GetRelativeTransform();
	const auto AdvanceAndCheck = [&](const TCHAR* Phase, int32 Frames)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			++GFrameCounter;
			Fixture.World->Tick(LEVELTICK_All, .02f);
			const FTransform Expected = Grip * Mesh->GetSocketTransform(GripSocket, RTS_World);
			if (!TestTrue(*FString::Printf(TEXT("%s frame %d follows the evaluated hand transform"), Phase, Frame),
				Weapon->GetComponentTransform().Equals(Expected, .05f))
				|| !TestTrue(*FString::Printf(TEXT("%s frame %d retains its saved grip offset"), Phase, Frame),
					Weapon->GetRelativeTransform().Equals(Grip, .001f))) return false;
		}
		return true;
	};
	if (!AdvanceAndCheck(TEXT("Idle"), 30)) return false;
	TestFalse(TEXT("Stationary attachment probe uses the real idle state"), Animation->CombatShouldMove);
	const FVector IdleHand = Mesh->GetSocketLocation(TEXT("hand_r"));
	const FVector IdleWeapon = Weapon->GetComponentLocation();
	// 这里直接播放已连接的表现蒙太奇；技能资格与伤害时机由 MinionMeleeHit 独立验证。
	if (!TestTrue(TEXT("Connected melee montage starts on the actual animation instance"), Animation->Montage_Play(Attack) > 0.f)) return false;
	float HandTravel = 0.f;
	float WeaponTravel = 0.f;
	const int32 AttackFrames = FMath::Max(1, FMath::CeilToInt(Attack->GetPlayLength() / .02f));
	for (int32 Frame = 0; Frame < AttackFrames; ++Frame)
	{
		if (!AdvanceAndCheck(TEXT("Attack"), 1)) return false;
		HandTravel = FMath::Max(HandTravel, FVector::Distance(IdleHand, Mesh->GetSocketLocation(TEXT("hand_r"))));
		WeaponTravel = FMath::Max(WeaponTravel, FVector::Distance(IdleWeapon, Weapon->GetComponentLocation()));
	}
	TestTrue(TEXT("Real attack changes the right hand rather than only playing an empty montage"), HandTravel > 1.f);
	TestTrue(TEXT("Polearm moves through the swing rather than remaining at its idle world position"), WeaponTravel > 1.f);
	const FVector BeforeMove = Weapon->GetComponentLocation();
	Enemy->SetActorLocationAndRotation(FVector(350, 120, 100), FRotator(0, 85, 0));
	if (!AdvanceAndCheck(TEXT("Relocated"), 2)) return false;
	TestTrue(TEXT("Polearm also follows actor translation and rotation"),
		FVector::Distance(BeforeMove, Weapon->GetComponentLocation()) > 100.f);
	AddInfo(FString::Printf(TEXT("Minion polearm attachment: handTravel=%.2fcm weaponTravel=%.2fcm mesh=%s"),
		HandTravel, WeaponTravel, *GetPathNameSafe(Weapon->GetStaticMesh())));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatEnemyLocomotionTest, "Multiplayer.Combat.EnemyLocomotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 实际敌人通过 CMC 行走，保留完整网格的后处理；组件空间足骨摆幅不能由胶囊平移代替。
bool FCoopCombatEnemyLocomotionTest::RunTest(const FString& Parameters)
{
	UClass* EnemyClass = LoadClass<AmultiplayerCombatEnemy>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatEnemy.BP_CombatEnemy_C"));
	if (!TestNotNull(TEXT("Actual enemy blueprint exists for locomotion"), EnemyClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Enemy = Fixture.Character(EnemyClass, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Actual enemy is spawned above the real floor"), Enemy)) return false;
	USkeletalMeshComponent* Mesh = Enemy->GetMesh();
	UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	Movement->bRunPhysicsWithNoController = true;
	Movement->SetComponentTickEnabled(true);
	Movement->SetMovementMode(MOVE_Walking);
	const auto Advance = [&](int32 Frames, bool bMove = false)
	{
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			if (bMove) Enemy->AddMovementInput(FVector::ForwardVector);
			++GFrameCounter;
			Fixture.World->Tick(LEVELTICK_All, .02f);
		}
	};
	Advance(15);
	auto* Animation = Cast<UmultiplayerCombatAnimInstance>(Mesh->GetAnimInstance());
	UAnimInstance* PostProcess = Mesh->GetPostProcessInstance();
	if (!TestNotNull(TEXT("Enemy runs its adapted native-backed animation"), Animation)
		|| !TestNotNull(TEXT("Complete enemy mesh runs its original post-process"), PostProcess)) return false;
	TestTrue(TEXT("Locomotion preserves the complete mesh post-process class"), !Mesh->GetDisablePostProcessBlueprint()
		&& PostProcess->GetClass() == Mesh->GetSkeletalMeshAsset()->GetPostProcessAnimBlueprint().Get());
	TestTrue(TEXT("Enemy settles onto the actual fixture floor"), Movement->IsMovingOnGround());
	const FName Feet[] = { TEXT("foot_l"), TEXT("foot_r") };
	for (const FName Foot : Feet)
		if (!TestTrue(*FString::Printf(TEXT("Enemy complete mesh contains %s"), *Foot.ToString()), Mesh->GetBoneIndex(Foot) != INDEX_NONE)) return false;
	const FVector Start = Enemy->GetActorLocation();
	float ForwardMin[2] = { MAX_flt, MAX_flt }, ForwardMax[2] = { -MAX_flt, -MAX_flt };
	for (int32 Sample = 0; Sample < 45; ++Sample)
	{
		Advance(2, true);
		const FVector ComponentForward = Mesh->GetComponentQuat().UnrotateVector(Enemy->GetActorForwardVector());
		for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
		{
			const FVector Position = Mesh->GetSocketTransform(Feet[Foot], RTS_Component).GetLocation();
			const float Forward = FVector::DotProduct(Position, ComponentForward);
			ForwardMin[Foot] = FMath::Min(ForwardMin[Foot], Forward);
			ForwardMax[Foot] = FMath::Max(ForwardMax[Foot], Forward);
		}
	}
	const float Travel = FVector::Dist2D(Start, Enemy->GetActorLocation());
	TestTrue(TEXT("Real input moves the enemy more than 150cm through CMC"), Travel > 150.f);
	TestTrue(TEXT("Enemy locomotion reads its real current movement"), Animation->CombatShouldMove && Animation->CombatMoveSpeed > 100.f);
	for (int32 Foot = 0; Foot < UE_ARRAY_COUNT(Feet); ++Foot)
		TestTrue(*FString::Printf(TEXT("Enemy %s swings more than 10cm in component space with source post-process active"), *Feet[Foot].ToString()),
			ForwardMax[Foot] - ForwardMin[Foot] > 10.f);
	AddInfo(FString::Printf(TEXT("Enemy locomotion travel=%.2fcm footForwardRange left=%.2fcm right=%.2fcm postProcess=%s"),
		Travel, ForwardMax[0] - ForwardMin[0], ForwardMax[1] - ForwardMin[1], *PostProcess->GetClass()->GetPathName()));

	Movement->StopMovementImmediately();
	Movement->DisableMovement();
	Advance(15);
	// 近战需要真实 AI 黑板目标，实际 GAS 攻击与蒙太奇推进由 MinionMeleeHit 验证。
	Enemy->PlayImpactReaction();
	const auto* HitField = FindFProperty<FObjectPropertyBase>(Enemy->GetClass(), TEXT("HitMontage"));
	UAnimMontage* Hit = HitField ? Cast<UAnimMontage>(HitField->GetObjectPropertyValue_InContainer(Enemy)) : nullptr;
	TestTrue(TEXT("Enemy impact entry really plays the connected hit montage"), Hit && Animation->Montage_IsPlaying(Hit));
	Advance(4);
	TestTrue(TEXT("Enemy hit montage advances through real world ticks"), Hit && Animation->Montage_GetPosition(Hit) > .03f);
	const auto* DeathField = FindFProperty<FObjectPropertyBase>(Enemy->GetClass(), TEXT("DeathMontage"));
	UAnimMontage* Death = DeathField ? Cast<UAnimMontage>(DeathField->GetObjectPropertyValue_InContainer(Enemy)) : nullptr;
	UAbilitySystemComponent* ASC = Enemy->GetAbilitySystemComponent();
	FGameplayEffectSpecHandle Damage = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	Damage.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, Enemy->GetMaxHealth() + 1.f);
	ASC->ApplyGameplayEffectSpecToSelf(*Damage.Data.Get());
	TestFalse(TEXT("Real GAS damage commits enemy death"), Enemy->IsAlive());
	TestTrue(TEXT("GAS death replaces the hit with the connected death montage"), Death && Animation->Montage_IsPlaying(Death));
	Advance(4);
	TestTrue(TEXT("Enemy death montage advances through real world ticks"), Death && Animation->Montage_GetPosition(Death) > .03f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAttackAnimationTimingTest, "Multiplayer.Combat.AttackAnimationTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 实际角色蓝图按配置等待出手；取消只撤销尚未出手的动作，不能追回已经生成的真实投射物。
bool FCoopCombatAttackAnimationTimingTest::RunTest(const FString& Parameters)
{
	UClass* PlayerClass = LoadClass<AmultiplayerCombatCharacter>(nullptr,
		TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
	if (!TestNotNull(TEXT("Actual animated player blueprint exists"), PlayerClass)) return false;
	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(PlayerClass, FVector(0, 0, 100));
	if (!TestNotNull(TEXT("Attack timing player spawned"), Player)
		|| !TestNotNull(TEXT("Actual player has an attack montage"), Player->GetActionMontage(EMultiplayerCombatAnimation::Attack))) return false;
	const FmultiplayerCombatSkillRule Rule = Player->GetCombatConfig().Attack;
	if (!TestTrue(TEXT("Actual attack has a positive windup"), Rule.Windup > 0)) return false;
	const auto Advance = [&](float Seconds)
	{
		for (float Elapsed = 0; Elapsed < Seconds;)
		{
			const float Step = FMath::Min(.01f, Seconds - Elapsed);
			++GFrameCounter;
			Fixture.World->Tick(LEVELTICK_All, Step);
			Elapsed += Step;
		}
	};
	const auto CountProjectiles = [&]
	{
		int32 Count = 0;
		for (TActorIterator<AmultiplayerCombatProjectile> It(Fixture.World); It; ++It) ++Count;
		return Count;
	};
	int32 TotalSpawned = 0;
	const FDelegateHandle Spawned = Fixture.World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda(
		[&](AActor* Actor) { if (Actor->IsA<AmultiplayerCombatProjectile>()) ++TotalSpawned; }));
	UAbilitySystemComponent* ASC = Player->GetAbilitySystemComponent();
	const FGameplayTagContainer AttackTag(CombatTags::Ability_Attack);
	const auto ResetAttackCooldown = [&]
	{
		ASC->RemoveActiveEffectsWithGrantedTags(FGameplayTagContainer(CombatTags::Cooldown_Attack));
	};
	Player->Attack();
	TestEqual(TEXT("Starting the montage does not immediately fire"), CountProjectiles(), 0);
	Advance(Rule.Windup * .5f);
	TestEqual(TEXT("No projectile exists before the configured release"), CountProjectiles(), 0);
	ASC->CancelAbilities(&AttackTag);
	Advance(Rule.Windup + .05f);
	TestEqual(TEXT("Cancelled windup cannot fire from its old delay"), TotalSpawned, 0);

	ResetAttackCooldown();
	Player->Attack();
	Advance(Rule.Windup + .03f);
	TestEqual(TEXT("A completed windup creates exactly one real projectile"), CountProjectiles(), 1);
	TestEqual(TEXT("Release creates only one projectile in total"), TotalSpawned, 1);
	Player->ApplyStun(.1f);
	Advance(Rule.Recovery + .13f);
	TestEqual(TEXT("Post-release stun does not create another projectile"), TotalSpawned, 1);
	TestEqual(TEXT("Post-release stun does not retract the existing projectile"), CountProjectiles(), 1);

	ResetAttackCooldown();
	Player->Attack();
	Advance(Rule.Windup * .5f);
	Player->ApplyStun(.2f);
	Advance(Rule.Windup + Rule.Recovery + .25f);
	TestEqual(TEXT("Stun during a later windup cannot produce a new projectile"), TotalSpawned, 1);
	TestFalse(TEXT("The final stun has expired normally"), Player->IsStunned());
	Fixture.World->RemoveOnActorSpawnedHandler(Spawned);
	AddInfo(FString::Printf(TEXT("Actual attack timing: windup=%.6f recovery=%.6f spawned=%d"), Rule.Windup, Rule.Recovery, TotalSpawned));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAttackCueSnapshotTest, "Multiplayer.Combat.AttackCueSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

// 真正生成两种粒子组件，检查网络事件快照，而不是用角色当前朝向替代本次发射方向。
bool FCoopCombatAttackCueSnapshotTest::RunTest(const FString& Parameters)
{
	UParticleSystem* Cascade = LoadObject<UParticleSystem>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/IggyVFX/Particles/Primary/P_Fireball_Projectile_Cast.P_Fireball_Projectile_Cast"));
	UNiagaraSystem* Niagara = LoadObject<UNiagaraSystem>(nullptr,
		TEXT("/Game/Pack_Effects_2/Water_Magic/VFX_Niagara/NS_Water_Magic_Splash2.NS_Water_Magic_Splash2"));
	if (!TestNotNull(TEXT("Real attack Cascade loads"), Cascade) || !TestNotNull(TEXT("Real alternate Niagara loads"), Niagara)) return false;
	FCombatPresentationWorld Fixture;
	auto* Player = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(1300, 1400, 100));
	if (!TestNotNull(TEXT("Attack cue caster spawned"), Player)) return false;
	auto* Config = NewObject<UmultiplayerCombatConfig>(Player);
	auto* Property = FindFProperty<FObjectPropertyBase>(Player->GetClass(), TEXT("CombatConfig"));
	if (!TestNotNull(TEXT("Attack cue config property exists"), Property)) return false;
	Property->SetObjectPropertyValue_InContainer(Player, Config);
	const FVector Origin = Player->GetCombatMuzzleLocation();
	const FVector Direction = FRotator(22.f, 73.f, 0.f).Vector();
	FGameplayCueParameters Feedback(Player->GetAbilitySystemComponent()->MakeEffectContext());
	Feedback.Location = Origin; Feedback.Normal = Direction;
	// 显式让新身体朝向、位置与发射时刻不同，模拟网络到达或异步加载期间角色已经转身。
	Player->SetActorRotation(FRotator(0, -120, 0));
	Player->SetActorLocation(Player->GetActorLocation() + FVector(300, 200, 0));
	Config->AttackFeedback.CascadeEffect = Cascade;
	auto* CascadeCue = Fixture.World->SpawnActor<AmultiplayerCombatCue>();
	CascadeCue->GameplayCueTag = CombatTags::Cue_Attack;
	CascadeCue->OnExecute_Implementation(Player, Feedback);
	UParticleSystemComponent* CastParticle = nullptr;
	for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
		if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->Template == Cascade) CastParticle = *It;
	if (!TestNotNull(TEXT("Actual Cascade muzzle component is created"), CastParticle)) return false;
	TestTrue(TEXT("Cascade stays at the stable server launch snapshot"), CastParticle->GetComponentLocation().Equals(Origin, .1f));
	TestTrue(TEXT("Cascade points along the launch direction including pitch"), CastParticle->GetForwardVector().Equals(Direction, .001f));
	Config->AttackFeedback.CascadeEffect.Reset(); Config->AttackFeedback.Effect = Niagara;
	auto* NiagaraCue = Fixture.World->SpawnActor<AmultiplayerCombatCue>();
	NiagaraCue->GameplayCueTag = CombatTags::Cue_Attack;
	NiagaraCue->OnExecute_Implementation(Player, Feedback);
	UNiagaraComponent* NiagaraParticle = nullptr;
	for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->GetAsset() == Niagara) NiagaraParticle = *It;
	if (!TestNotNull(TEXT("Actual Niagara muzzle component is created"), NiagaraParticle)) return false;
	TestTrue(TEXT("Niagara uses the same launch position as Cascade"), NiagaraParticle->GetComponentLocation().Equals(Origin, .1f));
	TestTrue(TEXT("Niagara uses the same launch direction as Cascade"), NiagaraParticle->GetForwardVector().Equals(Direction, .001f));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatHealResultCueTest, "Multiplayer.Combat.HealResultCueOwnerAndObserver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

// 真正的治疗能力提供上下文；随后调用引擎 RPC 接收实现重现拥有者预测过滤，不能代替双进程网络验收。
bool FCoopCombatHealResultCueTest::RunTest(const FString& Parameters)
{
	UClass* HealCueClass = LoadClass<AmultiplayerCombatCue>(nullptr,
		TEXT("/Game/Combat/Presentation/Cues/GCN_Heal.GCN_Heal_C"));
	if (!TestNotNull(TEXT("Formal healing cue class loads"), HealCueClass)) return false;
	TestTrue(TEXT("Different healers retain independent asynchronous result cue instances"),
		HealCueClass->GetDefaultObject<AmultiplayerCombatCue>()->bUniqueInstancePerInstigator);
	UParticleSystem* Cascade = LoadObject<UParticleSystem>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/ReplenishmentCast/P_AscendTakeoffFalle.P_AscendTakeoffFalle"));
	if (!TestNotNull(TEXT("Real healing particle loads"), Cascade)) return false;
	FCombatPresentationWorld Fixture;
	auto* Caster = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(1000, 1000, 100));
	auto* Recipient = Fixture.Character(AmultiplayerCombatCharacter::StaticClass(), FVector(1300, 1000, 100));
	if (!TestNotNull(TEXT("Healer exists"), Caster) || !TestNotNull(TEXT("Different-config recipient exists"), Recipient)) return false;
	auto* Config = NewObject<UmultiplayerCombatConfig>(Caster);
	Config->HealFeedback.CascadeEffect = Cascade;
	auto* Property = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("CombatConfig"));
	if (!TestNotNull(TEXT("Healer config is selectable"), Property)) return false;
	Property->SetObjectPropertyValue_InContainer(Caster, Config);
	UAbilitySystemComponent* ASC = Caster->GetAbilitySystemComponent();
	UGameplayCueManager* Manager = UAbilitySystemGlobals::Get().GetGameplayCueManager();
#if WITH_EDITOR
	Manager->LoadNotifyForEditorPreview(CombatTags::Cue_Heal);
#endif
	FGameplayEffectSpecHandle DamageSpec = ASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	DamageSpec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, 20.f);
	ASC->ApplyGameplayEffectSpecToSelf(*DamageSpec.Data.Get());
	FGameplayCueParameters Captured;
	int32 HealRoutes = 0;
	const FDelegateHandle Route = Manager->OnGameplayCueRouted().AddLambda(
		[&](AActor* Target, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters& Feedback, EGameplayCueExecutionOptions)
		{
			if (Target == Caster && Tag == CombatTags::Cue_Heal && Event == EGameplayCueEvent::Executed)
			{ Captured = Feedback; ++HealRoutes; }
		});
	Caster->Heal();
	TestEqual(TEXT("Actual self heal reaches full health once"), Caster->GetHealth(), 100.f);
	TestEqual(TEXT("Authority self heal publishes exactly one result cue"), HealRoutes, 1);
	TestTrue(TEXT("Production healing cue retains the caster context"), Captured.GetInstigator() == Caster);
	const auto Particles = [&]()
	{
		TArray<UParticleSystemComponent*> Result;
		for (TObjectIterator<UParticleSystemComponent> It; It; ++It)
			if (It->GetWorld() == Fixture.World && It->IsRegistered() && It->Template == Cascade) Result.Add(*It);
		return Result;
	};
	const int32 AuthorityCount = Particles().Num();
	TestEqual(TEXT("Authority dispatch creates one actual healing component"), AuthorityCount, 1);
	Caster->SetRole(ROLE_AutonomousProxy);
	// ASC 在注册时缓存网络角色；不能只改已经注册的权威角色来冒充客户端。
	auto* ReceiverASC = NewObject<UAbilitySystemComponent>(Caster);
	ReceiverASC->SetIsReplicated(true);
	ReceiverASC->RegisterComponent();
	ReceiverASC->InitAbilityActorInfo(Caster, Caster);
	TestFalse(TEXT("RPC receiver is genuinely non-authoritative in the engine's role cache"), ReceiverASC->IsOwnerActorAuthoritative());
	const FPredictionKey OldClientKey = FPredictionKey::CreateNewPredictionKey(ReceiverASC);
	TestTrue(TEXT("Regression case contains a genuine local client key"), OldClientKey.IsLocalClientKey());
	ReceiverASC->NetMulticast_InvokeGameplayCueExecuted_WithParams_Implementation(CombatTags::Cue_Heal, OldClientKey, Captured);
	TestEqual(TEXT("Engine reproduces the old owner-side prediction suppression"), Particles().Num(), AuthorityCount);
	ReceiverASC->NetMulticast_InvokeGameplayCueExecuted_WithParams_Implementation(CombatTags::Cue_Heal, FPredictionKey(), Captured);
	TestEqual(TEXT("Independent authority result key creates exactly one owner component"), Particles().Num(), AuthorityCount + 1);
	Caster->SetRole(ROLE_SimulatedProxy);
	ReceiverASC->NetMulticast_InvokeGameplayCueExecuted_WithParams_Implementation(CombatTags::Cue_Heal, FPredictionKey(), Captured);
	TestEqual(TEXT("Observer dispatch creates one additional component, not a duplicate pair"), Particles().Num(), AuthorityCount + 2);
	ReceiverASC->DestroyComponent();
	Caster->SetRole(ROLE_Authority);
	Manager->OnGameplayCueRouted().Remove(Route);
	// 收到治疗的一方没有技能表现配置，也必须使用施法者定义在自己的脚底生成一次特效。
	auto* RecipientCue = Fixture.World->SpawnActor<AmultiplayerCombatCue>();
	RecipientCue->GameplayCueTag = CombatTags::Cue_Heal;
	RecipientCue->OnExecute_Implementation(Recipient, Captured);
	const FVector RecipientFeet = Recipient->GetActorLocation() - FVector::UpVector * Recipient->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	int32 AtRecipient = 0;
	for (const auto* Particle : Particles()) if (Particle->GetComponentLocation().Equals(RecipientFeet, .1f)) ++AtRecipient;
	TestEqual(TEXT("A different recipient config cannot suppress caster-defined healing VFX"), AtRecipient, 1);
	TestEqual(TEXT("Presentation at another recipient does not apply fake healing"), Recipient->GetHealth(), 100.f);
	return !HasAnyErrors();
}

#endif
