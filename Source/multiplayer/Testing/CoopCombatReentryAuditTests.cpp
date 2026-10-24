// Copyright Epic Games, Inc. All Rights Reserved.

// 回归真实动画开始、转向通知和激活标签回调中的同步取消／替换。
// 使用保存角色与实际 ASC，重点保证旧调用栈、旧预测拒绝不接管或终止新一轮动作。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatAbilitySystem.h"
#include "Misc/ScopeExit.h"
#include "Testing/CoopCombatRelationshipTestProbe.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatProjectile.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameplayPrediction.h"
#include "UObject/StructOnScope.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace
{
	// 用保存的玩家、真实动画实例和 ASC；通知只发起取消，不替代技能执行。
	struct FCombatReentryAuditWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle SpawnObserver;
		int32 ProjectileCount = 0;

		// 建立可施法地面并记录所有真实弹体出生，取消后立即销毁也不能掩盖错误发弹。
		FCombatReentryAuditWorld()
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
			SpawnObserver = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (Actor->IsA<AmultiplayerCombatProjectile>()) ++ProjectileCount;
			}));
		}

		// 先移除生成观察者，再结束世界与还原帧号，防止回调跨用例存活。
		~FCombatReentryAuditWorld()
		{
			World->RemoveOnActorSpawnedHandler(SpawnObserver);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 加载保存玩家、真实控制器及持续动画更新，让重入发生在正式蒙太奇通知中。
		AmultiplayerCombatCharacter* Player()
		{
			auto* Class = LoadClass<AmultiplayerCombatCharacter>(nullptr,
				TEXT("/Game/Combat/Presentation/BP_CombatPlayer.BP_CombatPlayer_C"));
			if (!Class) return nullptr;
			FActorSpawnParameters Spawn;
			Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			auto* Character = World->SpawnActor<AmultiplayerCombatCharacter>(Class, FVector(0, 0, 100), FRotator::ZeroRotator, Spawn);
			if (!Character) return nullptr;
			auto* Controller = World->SpawnActor<APlayerController>();
			Controller->Possess(Character);
			Controller->SetControlRotation(FRotator(-35, 0, 0));
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			Character->GetMesh()->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			return Character;
		}

		// 继续推进真实任务，检查取消后不会迟到发弹、伤害或重新启动旧动作。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Elapsed += Step;
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAttackMontageStartCancelAudit,
	"Multiplayer.Combat.ReentryAudit.AttackMontageStartCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Montage_Play 在 ASC 写入动画归属前广播 Started；取消后不应留下已结束能力的动作。
bool FCoopCombatAttackMontageStartCancelAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	auto* ASC = Player->GetAbilitySystemComponent();
	const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	if (!TestTrue(TEXT("Real animation, attack montage and granted spec exist"), Animation && Montage && Spec)) return false;
	const auto Handle = Spec->Handle;
	int32 Starts = 0;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->MontageCallback = [&](UAnimMontage* Started)
	{
		if (Started == Montage && Starts++ == 0) ASC->CancelAbilityHandle(Handle);
	};
	Animation->OnMontageStarted.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
	ON_SCOPE_EXIT
	{
		Animation->OnMontageStarted.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
		Probe->MontageCallback = nullptr;
	};
	Player->Attack();
	TestEqual(TEXT("Actual Montage_Play emitted its synchronous start notification"), Starts, 1);
	TestFalse(TEXT("Cancellation ends the actual granted attack"), ASC->FindAbilitySpecFromHandle(Handle)->IsActive());
	TestFalse(TEXT("Returned PlayMontage cannot leave the cancelled attack animation playing"), Animation->Montage_IsPlaying(Montage));
	TestFalse(TEXT("Cancelled attack releases facing ownership"), ASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
	Fixture.Advance(Player->GetCombatConfig().Attack.Windup + .2f);
	TestEqual(TEXT("Cancelled windup emits no projectile"), Fixture.ProjectileCount, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAreaMontageStartCancelAudit,
	"Multiplayer.Combat.ReentryAudit.AreaMontageStartCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// R 动画开始通知内取消时，返回旧播放栈不能残留动作、施法锁或后续范围结算。
bool FCoopCombatAreaMontageStartCancelAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Area);
	auto* ASC = Player->GetAbilitySystemComponent();
	const auto* Spec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	if (!TestTrue(TEXT("Real animation, area montage and granted spec exist"), Animation && Montage && Spec)) return false;
	const auto Handle = Spec->Handle;
	int32 Starts = 0;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->MontageCallback = [&](UAnimMontage* Started)
	{
		if (Started == Montage && Starts++ == 0) Player->CancelCast();
	};
	Animation->OnMontageStarted.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
	ON_SCOPE_EXIT
	{
		Animation->OnMontageStarted.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
		Probe->MontageCallback = nullptr;
	};
	Player->CastArea();
	TestEqual(TEXT("Area enters the actual saved montage before synchronous cancellation"), Starts, 1);
	TestFalse(TEXT("Cancellation ends the actual area spec"), ASC->FindAbilitySpecFromHandle(Handle)->IsActive());
	TestFalse(TEXT("Cancellation releases its infinite casting effect"), Player->IsCasting());
	TestFalse(TEXT("Returned PlayMontage cannot leave the cancelled area animation playing"), Animation->Montage_IsPlaying(Montage));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAttackRotationCancelAudit,
	"Multiplayer.Combat.ReentryAudit.AttackRotationCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 角色转向也会通知组件；转向时被取消后，旧调用栈不能再播放一个新动作。
bool FCoopCombatAttackRotationCancelAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	if (!TestTrue(TEXT("Actual animation and attack spec are ready"), Animation && Montage && Spec)) return false;
	const auto Handle = Spec->Handle;
	Player->GetController()->SetControlRotation(FRotator(-35, 90, 0));
	int32 RotationCancellations = 0, Starts = 0;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->MontageCallback = [&](UAnimMontage* Started) { if (Started == Montage) ++Starts; };
	Animation->OnMontageStarted.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
	const auto RotationListener = Player->GetRootComponent()->TransformUpdated.AddLambda(
		[&](USceneComponent*, EUpdateTransformFlags, ETeleportType)
		{
			if (RotationCancellations == 0 && ASC->FindAbilitySpecFromHandle(Handle)->IsActive())
			{
				++RotationCancellations;
				ASC->CancelAbilityHandle(Handle);
			}
		});
	ON_SCOPE_EXIT
	{
		Player->GetRootComponent()->TransformUpdated.Remove(RotationListener);
		Animation->OnMontageStarted.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
		Probe->MontageCallback = nullptr;
	};
	Player->Attack();
	TestEqual(TEXT("Actual body rotation synchronously cancelled the active attack"), RotationCancellations, 1);
	TestFalse(TEXT("Rotation cancellation ends the original attack"), ASC->FindAbilitySpecFromHandle(Handle)->IsActive());
	TestEqual(TEXT("Cancelled rotation cannot start an attack montage afterward"), Starts, 0);
	Fixture.Advance(Player->GetCombatConfig().Attack.Windup + .2f);
	TestEqual(TEXT("Rotation cancellation emits no projectile"), Fixture.ProjectileCount, 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatPreActivateDuplicateRequestAudit,
	"Multiplayer.Combat.ReentryAudit.PreActivateDuplicateRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 原生 PreActivate 先授予 owned tag，最后才增加 Spec.ActiveCount；该通知内的请求也必须拒绝复用。
bool FCoopCombatPreActivateDuplicateRequestAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	if (!TestTrue(TEXT("Real attack has its primary per-actor instance"), Spec && Spec->GetPrimaryInstance())) return false;
	const auto Handle = Spec->Handle;
	int32 NestedRequests = 0, ActualActivations = 0;
	bool bNestedAccepted = false, bObservedPreActivateWindow = false;
	const auto ActivationListener = ASC->AbilityActivatedCallbacks.AddLambda([&](UGameplayAbility* Ability)
	{
		if (Ability == ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance()) ++ActualActivations;
	});
	const auto TagListener = ASC->RegisterGameplayTagEvent(CombatTags::State_Attacking, EGameplayTagEventType::NewOrRemoved)
		.AddLambda([&](FGameplayTag, int32 Count)
		{
			if (Count <= 0 || NestedRequests != 0) return;
			++NestedRequests;
			const auto* Current = ASC->FindAbilitySpecFromHandle(Handle);
			bObservedPreActivateWindow = Current && !Current->IsActive() && Current->GetPrimaryInstance()->IsActive();
			// 进入生产事件分发与 InternalTryActivateAbility，不直接调用 ActivateAbility。
			bNestedAccepted = Player->ActivateSkill(EMultiplayerCombatSkill::Attack);
		});
	ON_SCOPE_EXIT
	{
		ASC->AbilityActivatedCallbacks.Remove(ActivationListener);
		ASC->RegisterGameplayTagEvent(CombatTags::State_Attacking, EGameplayTagEventType::NewOrRemoved).Remove(TagListener);
	};
	TestTrue(TEXT("Original real attack request is accepted"), Player->ActivateSkill(EMultiplayerCombatSkill::Attack));
	TestEqual(TEXT("The owned tag made exactly one nested request"), NestedRequests, 1);
	TestTrue(TEXT("The callback occurs before ActiveCount, not after ordinary activation"), bObservedPreActivateWindow);
	TestFalse(TEXT("The real internal activation path rejects same-instance reentry"), bNestedAccepted);
	TestEqual(TEXT("Only one actual GAS preactivation notification occurs"), ActualActivations, 1);
	TestTrue(TEXT("The original per-actor instance remains active through its windup"),
		ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance()->IsActive());
	Fixture.Advance(Player->GetCombatConfig().Attack.Windup + Player->GetCombatConfig().Attack.Recovery + .2f);
	TestFalse(TEXT("Normal recovery cannot leave a phantom active count"), ASC->FindAbilitySpecFromHandle(Handle)->IsActive());
	TestFalse(TEXT("Normal recovery releases the owned attacking state"), ASC->HasMatchingGameplayTag(CombatTags::State_Attacking));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMontageStartReplacementAudit,
	"Multiplayer.Combat.ReentryAudit.MontageStartReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 动画开始通知内，R 按现有关系替换普攻；外层旧 PlayMontage 返回后不能覆盖新动画归属。
bool FCoopCombatMontageStartReplacementAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Attack = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	auto* Area = Player->GetActionMontage(EMultiplayerCombatAnimation::Area);
	const auto* AttackSpec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	const auto* AreaSpec = ASC->FindAbilitySpecFromClass(UmultiplayerAreaAbility::StaticClass());
	if (!TestTrue(TEXT("Real two abilities and their distinct montages exist"),
		Animation && Attack && Area && Attack != Area && AttackSpec && AreaSpec)) return false;
	const auto AttackHandle = AttackSpec->Handle, AreaHandle = AreaSpec->Handle;
	int32 ReplacementRequests = 0, AreaStarts = 0;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->MontageCallback = [&](UAnimMontage* Started)
	{
		if (Started == Area) ++AreaStarts;
		if (Started == Attack && ReplacementRequests == 0)
		{
			++ReplacementRequests;
			Player->CastArea();
		}
	};
	Animation->OnMontageStarted.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
	ON_SCOPE_EXIT
	{
		Animation->OnMontageStarted.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
		Probe->MontageCallback = nullptr;
	};
	Player->Attack();
	TestEqual(TEXT("Attack's real montage notification requests one replacement"), ReplacementRequests, 1);
	TestEqual(TEXT("Replacement actually plays its saved area montage"), AreaStarts, 1);
	TestFalse(TEXT("Relationship cancellation ends the replaced attack"), ASC->FindAbilitySpecFromHandle(AttackHandle)->IsActive());
	const auto* CurrentArea = ASC->FindAbilitySpecFromHandle(AreaHandle);
	TestTrue(TEXT("Replacement area owns a real active casting lifecycle"), CurrentArea->IsActive() && Player->IsCasting());
	TestTrue(TEXT("The animation instance is actually playing the replacement area"), Animation->Montage_IsPlaying(Area));
	TestTrue(TEXT("ASC animation owner agrees with the actual replacement"), ASC->GetAnimatingAbility() == CurrentArea->GetPrimaryInstance());
	TestTrue(TEXT("ASC current montage agrees with the actual replacement"), ASC->GetCurrentMontage() == Area);
	Player->CancelCast();
	TestFalse(TEXT("The replacement can stop its own animation after nested playback"), Animation->Montage_IsPlaying(Area));
	TestFalse(TEXT("The replacement can release its own casting lock"), Player->IsCasting());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMontagePredictionKeyOwnershipAudit,
	"Multiplayer.Combat.ReentryAudit.MontagePredictionKeyOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 窄测 ASC 的真实预测拒绝绑定，不替代双客户端激活验收；同资产的新播放不能被旧 key 停止。
bool FCoopCombatMontagePredictionKeyOwnershipAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	if (!TestTrue(TEXT("Actual animation and saved montage exist"), Animation && Montage)) return false;
	Player->SetRole(ROLE_AutonomousProxy);
	// 角色改 Role 不会刷新已注册 ASC 的缓存；新接收器必须在客户端 Role 下注册。
	auto* ReceiverASC = NewObject<UmultiplayerCombatAbilitySystem>(Player);
	ReceiverASC->SetIsReplicated(true);
	ReceiverASC->RegisterComponent();
	ReceiverASC->InitAbilityActorInfo(Player, Player);
	FPredictionKey OldKey, NewKey;
	bool bOldRejected = false, bNewRejected = false;
	// 使用真实接收 RPC 的反射入口，不调用 GameplayAbilities 未导出的全局委托实现。
	UFunction* RejectFunction = ReceiverASC->FindFunction(TEXT("ClientActivateAbilityFailed"));
	const auto* HandleProperty = RejectFunction ? FindFProperty<FStructProperty>(RejectFunction, TEXT("AbilityToActivate")) : nullptr;
	const auto* KeyProperty = RejectFunction ? FindFProperty<FInt16Property>(RejectFunction, TEXT("PredictionKey")) : nullptr;
	const bool bValidRejectSignature = RejectFunction && RejectFunction->NumParms == 2
		&& HandleProperty && HandleProperty->Struct == FGameplayAbilitySpecHandle::StaticStruct() && KeyProperty;
	const auto RejectKey = [&](const FPredictionKey& Key)
	{
		if (!Key.IsValidKey() || !bValidRejectSignature) return;
		// FStructOnScope 按 UFunction 布局分配，不假定 C++ 结构体对齐或参数偏移。
		FStructOnScope Frame(RejectFunction);
		const FGameplayAbilitySpecHandle NoAbility;
		HandleProperty->CopyCompleteValue(HandleProperty->ContainerPtrToValuePtr<void>(Frame.GetStructMemory()), &NoAbility);
		KeyProperty->SetPropertyValue_InContainer(Frame.GetStructMemory(), Key.Current);
		ReceiverASC->ProcessEvent(RejectFunction, Frame.GetStructMemory());
	};
	ON_SCOPE_EXIT
	{
		ReceiverASC->CurrentMontageStop(0.f);
		ReceiverASC->DestroyComponent();
		Player->SetRole(ROLE_Authority);
	};
	if (!TestFalse(TEXT("Test ASC has a genuinely non-authoritative role cache"), ReceiverASC->IsOwnerActorAuthoritative())) return false;
	if (!TestTrue(TEXT("Real rejection RPC has the verified handle and int16 parameter signature"), bValidRejectSignature)) return false;
	int32 FirstInstance = INDEX_NONE, SecondInstance = INDEX_NONE;
	{
		FScopedPredictionWindow Window(ReceiverASC, true);
		OldKey = ReceiverASC->GetPredictionKeyForNewAction();
		if (!TestTrue(TEXT("First playback receives a real local client prediction key"), OldKey.IsLocalClientKey())) return false;
		OldKey.NewRejectedDelegate().BindWeakLambda(ReceiverASC, [&] { bOldRejected = true; });
		TestTrue(TEXT("First real ASC playback succeeds"), ReceiverASC->PlayMontage(nullptr, FGameplayAbilityActivationInfo(), Montage, 1.f) > 0.f);
		if (const auto* Instance = Animation->GetActiveInstanceForMontage(Montage)) FirstInstance = Instance->GetInstanceID();
	}
	{
		FScopedPredictionWindow Window(ReceiverASC, true);
		NewKey = ReceiverASC->GetPredictionKeyForNewAction();
		TestTrue(TEXT("Second playback uses an independent later client key"), NewKey.IsLocalClientKey() && NewKey.Current != OldKey.Current && NewKey.Base == 0);
		NewKey.NewRejectedDelegate().BindWeakLambda(ReceiverASC, [&] { bNewRejected = true; });
		TestTrue(TEXT("Same saved asset starts a second actual playback"), ReceiverASC->PlayMontage(nullptr, FGameplayAbilityActivationInfo(), Montage, 1.f) > 0.f);
		if (const auto* Instance = Animation->GetActiveInstanceForMontage(Montage)) SecondInstance = Instance->GetInstanceID();
	}
	TestTrue(TEXT("The second play is a new montage instance, not the original instance"),
		FirstInstance != INDEX_NONE && SecondInstance != INDEX_NONE && FirstInstance != SecondInstance);
	RejectKey(OldKey);
	TestTrue(TEXT("The native client rejection actually broadcasts the old key"), bOldRejected);
	TestTrue(TEXT("Rejecting the old real key cannot stop the new same-asset animation"), Animation->Montage_IsPlaying(Montage));
	const auto* Surviving = Animation->GetActiveInstanceForMontage(Montage);
	TestTrue(TEXT("The same new montage instance survives the old rejection"), Surviving && Surviving->GetInstanceID() == SecondInstance);
	TestTrue(TEXT("ASC continues to own the current saved montage"), ReceiverASC->GetCurrentMontage() == Montage);
	RejectKey(NewKey);
	TestTrue(TEXT("The native client rejection actually broadcasts the new key"), bNewRejected);
	TestFalse(TEXT("Rejecting the current real key does stop its own animation"), Animation->Montage_IsPlaying(Montage));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatFailedNestedMontageAudit,
	"Multiplayer.Combat.ReentryAudit.FailedNestedMontage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Started 中的失败播放没有创建动作，不能令外层成功播放丢失归属和后续取消能力。
bool FCoopCombatFailedNestedMontageAudit::RunTest(const FString& Parameters)
{
	FCombatReentryAuditWorld Fixture;
	auto* Player = Fixture.Player();
	if (!TestNotNull(TEXT("Saved animated player exists"), Player)) return false;
	auto* ASC = Player->GetAbilitySystemComponent();
	auto* Animation = Player->GetMesh()->GetAnimInstance();
	auto* Montage = Player->GetActionMontage(EMultiplayerCombatAnimation::Attack);
	const auto* Spec = ASC->FindAbilitySpecFromClass(Player->GetAttackAbilityClass());
	if (!TestTrue(TEXT("Actual attack animation and granted instance exist"), Animation && Montage && Spec && Spec->GetPrimaryInstance())) return false;
	const auto Handle = Spec->Handle;
	TStrongObjectPtr<UAnimMontage> EmptyMontage(NewObject<UAnimMontage>());
	TestEqual(TEXT("Failure fixture has no playable animation length"), EmptyMontage->GetPlayLength(), 0.f);
	int32 NestedRequests = 0;
	float NestedDuration = 1.f;
	TStrongObjectPtr<UCoopCombatRelationshipTestProbe> Probe(NewObject<UCoopCombatRelationshipTestProbe>());
	Probe->MontageCallback = [&](UAnimMontage* Started)
	{
		if (Started == Montage && NestedRequests == 0)
		{
			++NestedRequests;
			auto* Ability = ASC->FindAbilitySpecFromHandle(Handle)->GetPrimaryInstance();
			NestedDuration = ASC->PlayMontage(Ability, Ability->GetCurrentActivationInfo(), EmptyMontage.Get(), 1.f);
		}
	};
	Animation->OnMontageStarted.AddDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
	ON_SCOPE_EXIT
	{
		Animation->OnMontageStarted.RemoveDynamic(Probe.Get(), &UCoopCombatRelationshipTestProbe::HandleMontage);
		Probe->MontageCallback = nullptr;
	};
	Player->Attack();
	TestEqual(TEXT("Actual montage notification makes exactly one nested attempt"), NestedRequests, 1);
	TestTrue(TEXT("The real animation system rejects the empty nested montage"), NestedDuration <= 0.f);
	const auto* Current = ASC->FindAbilitySpecFromHandle(Handle);
	TestTrue(TEXT("The successful outer attack remains active"), Current->IsActive() && Current->GetPrimaryInstance()->IsActive());
	TestTrue(TEXT("Outer real animation continues playing"), Animation->Montage_IsPlaying(Montage));
	TestTrue(TEXT("Failed nested play cannot steal ASC's ability owner"), ASC->GetAnimatingAbility() == Current->GetPrimaryInstance());
	TestTrue(TEXT("Failed nested play cannot steal ASC's montage record"), ASC->GetCurrentMontage() == Montage);
	ASC->CancelAbilityHandle(Handle);
	TestFalse(TEXT("Outer ability can still stop its own correctly owned animation"), Animation->Montage_IsPlaying(Montage));
	return !HasAnyErrors();
}

#endif
