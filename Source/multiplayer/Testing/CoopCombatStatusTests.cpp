// Copyright Epic Games, Inc. All Rights Reserved.

// 回归统一伤害结算、护盾溢出、持续伤害和多来源控制限制的叠加／解除。
// 真实 GE 和属性委托覆盖同步重入，靶敌关闭自主行为以隔离每次结算。

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
	// 属性与状态用真实 GAS/Timer 执行；靶敌不启用 AI，避免追击、攻击或画面资产污染断言。
	struct FCombatStatusWorld
	{
		UWorld* World;
		uint64 PreviousFrame = GFrameCounter;
		FDelegateHandle SpawnedHandle;
		int32 Projectiles = 0;

		// 建立真实施法地面和 GAS 世界，记录弹体出生用于检测中断后的迟到出手。
		FCombatStatusWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			// 范围技能现在要求真实地面；状态集成测试同样提供可查询的物理地板。
			auto* Ground = World->SpawnActor<AActor>();
			auto* Floor = NewObject<UBoxComponent>(Ground);
			Ground->SetRootComponent(Floor);
			Floor->SetBoxExtent(FVector(5000, 5000, 10));
			Floor->SetCollisionProfileName(TEXT("BlockAll"));
			Floor->RegisterComponent();
			Ground->SetActorLocation(FVector(0, 0, -10));
			SpawnedHandle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([this](AActor* Actor)
			{
				if (Actor->IsA<AmultiplayerCombatProjectile>()) ++Projectiles;
			}));
		}

		// 移除观察者再结束真实世界，清除旧效果和任务并恢复全局帧号。
		~FCombatStatusWorld()
		{
			World->RemoveOnActorSpawnedHandler(SpawnedHandle);
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = PreviousFrame;
		}

		// 小步推进真实效果周期、控制到期和能力任务，不手动触发业务完成回调。
		void Advance(float Seconds)
		{
			for (float Elapsed = 0.f; Elapsed < Seconds;)
			{
				const float Step = FMath::Min(.02f, Seconds - Elapsed);
				++GFrameCounter;
				World->Tick(LEVELTICK_All, Step);
				Elapsed += Step;
			}
		}

		// 创建指定阵营的静止真实角色，关闭 AI 以排除额外伤害和状态来源。
		AmultiplayerCombatCharacter* Character(bool bEnemy = false, const FVector& Location = FVector(0, 0, 100))
		{
			const FTransform Transform(FRotator::ZeroRotator, Location);
			UClass* Class = bEnemy ? AmultiplayerCombatEnemy::StaticClass() : AmultiplayerCombatCharacter::StaticClass();
			auto* Result = World->SpawnActorDeferred<AmultiplayerCombatCharacter>(Class, Transform,
				nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			Result->AutoPossessAI = EAutoPossessAI::Disabled;
			Result->FinishSpawning(Transform);
			Result->GetCharacterMovement()->DisableMovement();
			return Result;
		}

		// 只用于移动锁恢复用例，保留真实 Controller/Pawn 生命周期及原先的 Flying 模式。
		void PossessFlying(AmultiplayerCombatCharacter* Character)
		{
			auto* Controller = World->SpawnActor<APlayerController>();
			Controller->SetPlayerState(World->SpawnActor<APlayerState>());
			Controller->Possess(Character);
			Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		}
	};

	// 测试初值同样通过真实瞬时 GE 写入；只有专门检查 BaseChange 的用例直接使用 Base setter。
	void OverrideAttribute(AmultiplayerCombatCharacter* Character, const FGameplayAttribute& Attribute, float Value)
	{
		UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
		Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
		FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
		Modifier.Attribute = Attribute;
		Modifier.ModifierOp = EGameplayModOp::Override;
		Modifier.ModifierMagnitude = FScalableFloat(Value);
		UAbilitySystemComponent* ASC = Character->GetAbilitySystemComponent();
		ASC->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Effect, ASC->MakeEffectContext(), 1.f));
	}

	// 经真实瞬时 GE 设置足够上限及护盾，保留正式属性约束与通知链。
	void GiveShield(AmultiplayerCombatCharacter* Character, float Value)
	{
		OverrideAttribute(Character, UmultiplayerCombatAttributes::GetMaxShieldAttribute(), Value);
		OverrideAttribute(Character, UmultiplayerCombatAttributes::GetShieldAttribute(), Value);
	}

	// 按效果定义统计真实句柄，区分刷新同源效果和独立多来源效果。
	int32 CountEffects(AmultiplayerCombatCharacter* Character, TSubclassOf<UGameplayEffect> Class)
	{
		FGameplayEffectQuery Query;
		Query.EffectDefinition = Class;
		return Character->GetAbilitySystemComponent()->GetActiveEffects(Query).Num();
	}

	// 从指定来源施加正式禁锢 GE，验证各句柄独立结束且共享禁行计数仍正确。
	FActiveGameplayEffectHandle RootFrom(AmultiplayerCombatCharacter* Source, AmultiplayerCombatCharacter* Target, float Duration)
	{
		UAbilitySystemComponent* ASC = Source->GetAbilitySystemComponent();
		FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(UmultiplayerRootEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
		Spec.Data->SetDuration(Duration, true);
		return ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data.Get(), Target->GetAbilitySystemComponent());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatShieldSettlementTest, "Multiplayer.Combat.ShieldSettlement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 伤害统一经过 Execution 和属性集；先判断正面格挡，未挡住时消耗盾，只有溢出部分扣血。
bool FCoopCombatShieldSettlementTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* Source = Fixture.Character(true, FVector(-1000, 0, 100));
	auto* Target = Fixture.Character();
	GiveShield(Target, 50.f);
	TestEqual(TEXT("A real effect initializes shield capacity"), Target->GetMaxShield(), 50.f);
	TestEqual(TEXT("A real effect grants the current shield"), Target->GetShield(), 50.f);
	TestTrue(TEXT("Authoritative damage accepts the shielded hostile"), Source->ApplyCombatDamage(Target, 30.f));
	TestEqual(TEXT("Shield absorbs damage before health"), Target->GetShield(), 20.f);
	TestEqual(TEXT("Unbroken shield leaves health unchanged"), Target->GetHealth(), 100.f);
	Source->ApplyCombatDamage(Target, 35.f);
	TestEqual(TEXT("A breaking hit consumes only the remaining shield"), Target->GetShield(), 0.f);
	TestEqual(TEXT("Only shield overflow reaches health"), Target->GetHealth(), 85.f);
	Source->ApplyCombatDamage(Target, 10.f);
	TestEqual(TEXT("Later damage reaches health after the shield is broken"), Target->GetHealth(), 75.f);

	auto* Defending = Fixture.Character(false, FVector(0, 1000, 100));
	GiveShield(Defending, 50.f);
	Defending->Defend();
	TestTrue(TEXT("The real defensive ability has granted its effect"), Defending->IsDefending());
	Defending->SetActorRotation((Source->GetActorLocation() - Defending->GetActorLocation()).Rotation());
	Source->ApplyCombatDamage(Defending, 40.f);
	TestEqual(TEXT("A frontal guard fully blocks forty damage before consuming any shield"), Defending->GetShield(), 50.f);
	TestEqual(TEXT("The defended shield still protects all health"), Defending->GetHealth(), 100.f);
	Defending->SetActorRotation(Defending->GetActorRotation() + FRotator(0, 180, 0));
	Source->ApplyCombatDamage(Defending, 80.f);
	TestEqual(TEXT("Rear damage breaks the defended shield normally"), Defending->GetShield(), 0.f);
	TestEqual(TEXT("All eighty rear damage settles as fifty shield and thirty health"), Defending->GetHealth(), 70.f);

	// 盾变化会同步通知外部；嵌套伤害不能被外层保存的旧生命快照覆盖。
	auto* Reentrant = Fixture.Character(false, FVector(0, 2000, 100));
	GiveShield(Reentrant, 20.f);
	UAbilitySystemComponent* ReentrantASC = Reentrant->GetAbilitySystemComponent();
	const FDelegateHandle Nested = ReentrantASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetShieldAttribute())
		.AddLambda([&](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue == 0.f) Source->ApplyCombatDamage(Reentrant, 10.f);
		});
	Source->ApplyCombatDamage(Reentrant, 40.f);
	TestEqual(TEXT("Shield callback's ten damage and outer twenty overflow both settle once"), Reentrant->GetHealth(), 70.f);
	ReentrantASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetShieldAttribute()).Remove(Nested);

	auto* Leaving = Fixture.Character(false, FVector(0, 3000, 100));
	GiveShield(Leaving, 20.f);
	UAbilitySystemComponent* LeavingASC = Leaving->GetAbilitySystemComponent();
	const FDelegateHandle Admission = LeavingASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetShieldAttribute())
		.AddLambda([&](const FOnAttributeChangeData& Change)
		{
			if (Change.NewValue == 0.f) Leaving->SetCombatSessionReady(false);
		});
	Source->ApplyCombatDamage(Leaving, 40.f);
	TestFalse(TEXT("Shield notification can end the target's admission synchronously"), Leaving->IsCombatReady());
	TestEqual(TEXT("Outer overflow rechecks admission before applying more health damage"), Leaving->GetHealth(), 100.f);
	LeavingASC->GetGameplayAttributeValueChangeDelegate(UmultiplayerCombatAttributes::GetShieldAttribute()).Remove(Admission);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatAttributeBaseClampTest, "Multiplayer.Combat.AttributeBaseClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 同时验证 base 与 current，避免只钳住显示值而在属性聚合器中积累不可见的额外生命或盾。
bool FCoopCombatAttributeBaseClampTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* Target = Fixture.Character();
	UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
	GiveShield(Target, 50.f);
	const FGameplayAttribute Values[] = { UmultiplayerCombatAttributes::GetHealthAttribute(),
		UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetShieldAttribute() };
	const float Limits[] = { 100.f, 100.f, 50.f };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index)
	{
		ASC->SetNumericAttributeBase(Values[Index], 1000.f);
		TestEqual(TEXT("Direct base assignment is capped before entering the aggregator"), ASC->GetNumericAttributeBase(Values[Index]), Limits[Index]);
		TestEqual(TEXT("Current value matches the capped base"), ASC->GetNumericAttribute(Values[Index]), Limits[Index]);
	}
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), -10.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute(), -10.f);
	TestEqual(TEXT("Energy base cannot become negative"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute()), 0.f);
	TestEqual(TEXT("Shield base cannot become negative"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute()), 0.f);
	OverrideAttribute(Target, UmultiplayerCombatAttributes::GetShieldAttribute(), 40.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxShieldAttribute(), 20.f);
	TestEqual(TEXT("Direct maximum-shield base change clamps the existing shield"), Target->GetShield(), 20.f);
	TestEqual(TEXT("Direct maximum-shield base change also corrects shield base"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute()), 20.f);
	auto* Attributes = const_cast<UmultiplayerCombatAttributes*>(ASC->GetSet<UmultiplayerCombatAttributes>());
	if (!TestNotNull(TEXT("Character owns its actual combat attribute set"), Attributes)) return false;
	Attributes->SetMaxShield(10.f);
	TestEqual(TEXT("Generated maximum-shield setter follows the same current-value constraint"), Target->GetShield(), 10.f);
	TestEqual(TEXT("Generated maximum-shield setter follows the same base-value constraint"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute()), 10.f);
	OverrideAttribute(Target, UmultiplayerCombatAttributes::GetMaxShieldAttribute(), 50.f);
	OverrideAttribute(Target, UmultiplayerCombatAttributes::GetShieldAttribute(), 40.f);
	OverrideAttribute(Target, UmultiplayerCombatAttributes::GetMaxShieldAttribute(), 20.f);
	TestEqual(TEXT("Shrinking capacity clamps the existing shield"), Target->GetShield(), 20.f);
	TestEqual(TEXT("Shrinking capacity also corrects shield base"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetShieldAttribute()), 20.f);
	OverrideAttribute(Target, UmultiplayerCombatAttributes::GetMaxHealthAttribute(), 70.f);
	TestEqual(TEXT("Shrinking maximum health clamps the existing health"), Target->GetHealth(), 70.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxShieldAttribute(), -10.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxEnergyAttribute(), -10.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute(), -10.f);
	TestEqual(TEXT("Maximum shield base is nonnegative"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxShieldAttribute()), 0.f);
	TestEqual(TEXT("Maximum energy base is nonnegative"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxEnergyAttribute()), 0.f);
	TestEqual(TEXT("Maximum health base retains a valid positive capacity"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetMaxHealthAttribute()), 1.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), -10.f);
	TestEqual(TEXT("Health base cannot become negative"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute()), 0.f);
	TestFalse(TEXT("Zero health still uses the real death transition"), Target->IsAlive());
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatPeriodicHealingClampTest, "Multiplayer.Combat.PeriodicHealingClamp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 周期治疗是实际定时执行的 GE，不用循环直接写血；治疗溢出不能污染下一次结算的 base。
bool FCoopCombatPeriodicHealingClampTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* Source = Fixture.Character(true, FVector(-1000, 0, 100));
	auto* Target = Fixture.Character();
	Source->ApplyCombatDamage(Target, 40.f);
	UGameplayEffect* Healing = NewObject<UGameplayEffect>(GetTransientPackage());
	Healing->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	Healing->DurationMagnitude = FScalableFloat(2.5f);
	Healing->Period = FScalableFloat(1.f);
	Healing->bExecutePeriodicEffectOnApplication = false;
	FGameplayModifierInfo& Modifier = Healing->Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UmultiplayerCombatAttributes::GetHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FScalableFloat(30.f);
	UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
	const FActiveGameplayEffectHandle Handle = ASC->ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Healing, ASC->MakeEffectContext(), 1.f));
	TestTrue(TEXT("Periodic healing creates a real active effect"), Handle.IsValid());
	TestEqual(TEXT("Periodic healing is not executed immediately"), Target->GetHealth(), 60.f);
	Fixture.Advance(1.1f);
	TestEqual(TEXT("First actual healing period adds thirty"), Target->GetHealth(), 90.f);
	Fixture.Advance(1.f);
	TestEqual(TEXT("Second healing period clamps to maximum health"), Target->GetHealth(), 100.f);
	TestEqual(TEXT("Overhealing does not leave hidden base health above the maximum"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute()), 100.f);
	Fixture.Advance(1.f);
	TestNull(TEXT("Periodic healing expires normally"), ASC->GetActiveGameplayEffect(Handle));
	Source->ApplyCombatDamage(Target, 20.f);
	TestEqual(TEXT("Post-healing damage starts from the real capped health"), Target->GetHealth(), 80.f);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatBurningSourcesTest, "Multiplayer.Combat.BurningSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 同源只刷新持续时间，不推迟下一跳；不同 ASC 的灼烧各自计时、扣血与移除。
bool FCoopCombatBurningSourcesTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* First = Fixture.Character(false, FVector(-1000, 0, 100));
	auto* Second = Fixture.Character(false, FVector(-1000, 1000, 100));
	auto* Target = Fixture.Character(true);
	UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
	const FActiveGameplayEffectHandle Original = First->ApplyBurning(Target, 10.f, 2.5f);
	if (!TestTrue(TEXT("Authoritative burning produces a real active effect"), Original.IsValid())) return false;
	TestEqual(TEXT("Applying burning does not cause an immediate tick"), Target->GetHealth(), 100.f);
	Fixture.Advance(.9f);
	TestEqual(TEXT("No damage occurs before the first one-second period"), Target->GetHealth(), 100.f);
	Fixture.Advance(.2f);
	TestEqual(TEXT("First burning period deals its configured damage"), Target->GetHealth(), 90.f);
	const FActiveGameplayEffectHandle Refreshed = First->ApplyBurning(Target, 10.f, 2.5f);
	TestTrue(TEXT("Same source refreshes its existing effect handle"), Refreshed == Original);
	TestEqual(TEXT("Same source does not stack another burning instance"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 1);
	const FActiveGameplayEffect* RefreshedEffect = ASC->GetActiveGameplayEffect(Original);
	TestTrue(TEXT("Same-source application really renews the remaining duration"),
		RefreshedEffect && RefreshedEffect->GetTimeRemaining(Fixture.World->GetTimeSeconds()) > 2.4f);
	const FActiveGameplayEffectHandle Other = Second->ApplyBurning(Target, 20.f, 2.5f);
	TestTrue(TEXT("Different source owns a distinct active handle"), Other.IsValid() && Other != Original);
	TestEqual(TEXT("Different sources remain two independent effects"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 2);
	Fixture.Advance(.95f);
	TestEqual(TEXT("Refresh keeps the original two-second tick instead of postponing damage"), Target->GetHealth(), 80.f);
	Fixture.Advance(.1f);
	TestEqual(TEXT("The second source applies its independent first tick"), Target->GetHealth(), 60.f);
	ASC->RemoveActiveGameplayEffect(Original);
	TestEqual(TEXT("Removing one source leaves the other source active"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 1);
	Fixture.Advance(1.f);
	TestEqual(TEXT("Only the remaining source damages on its next period"), Target->GetHealth(), 40.f);
	Fixture.Advance(1.1f);
	TestEqual(TEXT("Expired burning stops all subsequent damage"), Target->GetHealth(), 40.f);
	TestEqual(TEXT("Both source effects have completed or been explicitly removed"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatMovementRestrictionOverlapTest, "Multiplayer.Combat.MovementRestrictionOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 每个控制效果只释放自己的限制；Root 只禁移动，不把它误当成眩晕而取消所有技能。
bool FCoopCombatMovementRestrictionOverlapTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	for (int32 ShortRoot = 0; ShortRoot < 2; ++ShortRoot)
	{
		auto* Target = Fixture.Character(false, FVector(0, ShortRoot * 1500.f, 100));
		Fixture.PossessFlying(Target);
		UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
		const FActiveGameplayEffectHandle Root = Target->ApplyRoot(ShortRoot ? .35f : .8f);
		TestTrue(TEXT("Root creates a real effect and unified movement restriction"), Root.IsValid() && Target->IsMovementBlocked());
		TestTrue(TEXT("Root carries its own state rather than posing as stun"), ASC->HasMatchingGameplayTag(CombatTags::State_Rooted) && !Target->IsStunned());
		const int32 BeforeAttack = Fixture.Projectiles;
		Target->Attack();
		TestEqual(TEXT("Root still permits the real GAS attack to generate a projectile"), Fixture.Projectiles, BeforeAttack + 1);
		Target->ApplyStun(ShortRoot ? .8f : .35f);
		TestEqual(TEXT("Root and stun each own one movement-blocking contribution"), ASC->GetTagCount(CombatTags::State_MovementBlocked), 2);
		Fixture.Advance(.45f);
		TestTrue(TEXT("The later control remains after the first control expires"), Target->IsMovementBlocked());
		TestEqual(TEXT("One expiration cannot prematurely restore movement"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
		TestEqual(TEXT("Only the remaining effect holds the shared restriction"), ASC->GetTagCount(CombatTags::State_MovementBlocked), 1);
		Fixture.Advance(.45f);
		TestFalse(TEXT("Final control expiration clears the unified restriction"), Target->IsMovementBlocked());
		TestEqual(TEXT("Final control expiration restores the original movement mode"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	}

	auto* First = Fixture.Character(false, FVector(-1000, 0, 100));
	auto* Second = Fixture.Character(false, FVector(-1000, 1000, 100));
	auto* Target = Fixture.Character(false, FVector(0, 3000, 100));
	Fixture.PossessFlying(Target);
	const auto FirstRoot = RootFrom(First, Target, .35f);
	const auto SecondRoot = RootFrom(Second, Target, .8f);
	TestTrue(TEXT("Different root sources create independent handles"), FirstRoot.IsValid() && SecondRoot.IsValid() && FirstRoot != SecondRoot);
	Fixture.Advance(.45f);
	TestTrue(TEXT("One source expiring cannot remove the other source's root"), Target->IsMovementBlocked());
	TestEqual(TEXT("Only one independent root remains"), CountEffects(Target, UmultiplayerRootEffect::StaticClass()), 1);
	Fixture.Advance(.45f);
	TestFalse(TEXT("All independent roots eventually release movement"), Target->IsMovementBlocked());
	TestEqual(TEXT("Independent root cleanup restores the captured mode"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatConfiguredStatusSkillIntegrationTest, "Multiplayer.Combat.ConfiguredStatusSkillIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 通过原 Q/R 技能走完整效果链；只复制内存配置，不改关卡资产，也不直接调用状态施加 API 代替技能。
bool FCoopCombatConfiguredStatusSkillIntegrationTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* Caster = Fixture.Character();
	auto* Config = DuplicateObject<UmultiplayerCombatConfig>(GetDefault<UmultiplayerCombatConfig>(), GetTransientPackage());
	Config->DefenseShieldAmount = 40.f;
	Config->BurningDamagePerTick = 5.f;
	Config->BurningDuration = 3.5f;
	Config->AreaRootDuration = 1.5f;
	Config->Area.Duration = .4f;
	Config->Area.Windup = .2f;
	Config->Area.Recovery = .15f;
	const auto* ConfigField = FindFProperty<FObjectPropertyBase>(Caster->GetClass(), TEXT("CombatConfig"));
	if (!TestNotNull(TEXT("Existing character exposes the actual combat config property"), ConfigField)) return false;
	ConfigField->SetObjectPropertyValue_InContainer(Caster, Config);
	Caster->Defend();
	TestTrue(TEXT("Original Q ability still grants its real defense effect"), Caster->IsDefending());
	TestEqual(TEXT("Successful Q also grants its configured shield"), Caster->GetShield(), 40.f);
	Caster->Defend();
	TestEqual(TEXT("Rejected repeated Q cannot grant another shield"), Caster->GetShield(), 40.f);

	const FVector Landing = Caster->GetAreaTarget();
	auto* Target = Fixture.Character(true, Landing + FVector(0, 50, 100));
	Target->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	const float InitialHealth = Target->GetHealth();
	Caster->CastArea();
	TestTrue(TEXT("Original R ability enters its real casting phase"), Caster->IsCasting());
	Fixture.Advance(Config->Area.Windup + .02f);
	TestEqual(TEXT("R settles its existing direct damage once"), Target->GetHealth(), InitialHealth - Config->Area.Amount);
	UAbilitySystemComponent* TargetASC = Target->GetAbilitySystemComponent();
	TestTrue(TEXT("R hit applies actual root and burning gameplay tags"),
		TargetASC->HasMatchingGameplayTag(CombatTags::State_Rooted)
		&& TargetASC->HasMatchingGameplayTag(CombatTags::State_Burning));
	TestTrue(TEXT("R also retains its original stun"), Target->IsStunned());
	TestEqual(TEXT("R created one real periodic burning effect"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 1);
	TestEqual(TEXT("R effects lock target movement during control overlap"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	Fixture.Advance(Config->Area.Duration + .1f);
	TestFalse(TEXT("The shorter stun expires before root"), Target->IsStunned());
	TestTrue(TEXT("Root from the same R hit still holds movement after stun ends"), Target->IsMovementBlocked()
		&& TargetASC->HasMatchingGameplayTag(CombatTags::State_Rooted));
	TestEqual(TEXT("Stun expiration cannot release the remaining root's movement lock"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	TestEqual(TEXT("The first burning tick is not executed early"), Target->GetHealth(), InitialHealth - Config->Area.Amount);
	Fixture.Advance(.6f);
	TestEqual(TEXT("R's configured burn deals real periodic damage after release"),
		Target->GetHealth(), InitialHealth - Config->Area.Amount - Config->BurningDamagePerTick);
	Fixture.Advance(.5f);
	TestFalse(TEXT("R's root eventually releases its own movement restriction"), Target->IsMovementBlocked());
	TestEqual(TEXT("Root removal restores the target's original movement mode"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_Flying);
	TestTrue(TEXT("Burn continues independently after both movement controls end"), TargetASC->HasMatchingGameplayTag(CombatTags::State_Burning));
	Fixture.Advance(2.5f);
	TestEqual(TEXT("R burn settles its three configured one-second periods and then stops"),
		Target->GetHealth(), InitialHealth - Config->Area.Amount - 3.f * Config->BurningDamagePerTick);
	TestEqual(TEXT("R burn has no active effect left after its configured duration"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatDeathClearsPeriodicStatusTest, "Multiplayer.Combat.DeathClearsPeriodicStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 不仅检查尸体血量仍为零，也观察真实周期回调是否停止，避免死亡后还在暗中执行旧灼烧。
bool FCoopCombatDeathClearsPeriodicStatusTest::RunTest(const FString& Parameters)
{
	FCombatStatusWorld Fixture;
	auto* Source = Fixture.Character(false, FVector(-1000, 0, 100));
	auto* Target = Fixture.Character(true);
	UAbilitySystemComponent* ASC = Target->GetAbilitySystemComponent();
	int32 BurningTicks = 0;
	const FDelegateHandle Periodic = ASC->OnPeriodicGameplayEffectExecuteDelegateOnSelf.AddLambda(
		[&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
		{
			if (Spec.Def && Spec.Def->IsA<UmultiplayerBurningEffect>()) ++BurningTicks;
		});
	const auto Burn = Source->ApplyBurning(Target, 10.f, 10.f);
	TestTrue(TEXT("Death fixture starts with real burning"), Burn.IsValid());
	Fixture.Advance(1.1f);
	TestEqual(TEXT("The live target executes one actual burning period"), BurningTicks, 1);
	TestEqual(TEXT("The live target takes the expected periodic damage"), Target->GetHealth(), 90.f);
	Target->ApplyRoot(5.f);
	Target->ApplyStun(5.f);
	Source->ApplyCombatDamage(Target, 1000.f);
	TestFalse(TEXT("A real lethal damage effect commits death"), Target->IsAlive());
	TestEqual(TEXT("Death removes all active burning instances"), CountEffects(Target, UmultiplayerBurningEffect::StaticClass()), 0);
	TestEqual(TEXT("Death removes root effects"), CountEffects(Target, UmultiplayerRootEffect::StaticClass()), 0);
	TestFalse(TEXT("Death clears the actual shared movement-restriction tag"), ASC->HasMatchingGameplayTag(CombatTags::State_MovementBlocked));
	TestFalse(TEXT("Dead targets cannot acquire a new burning effect"), Source->ApplyBurning(Target, 10.f, 2.f).IsValid());
	Fixture.Advance(2.5f);
	TestEqual(TEXT("Dead target never executes another burning period"), BurningTicks, 1);
	TestEqual(TEXT("Old status callbacks cannot revive the dead target"), Target->GetHealth(), 0.f);
	TestEqual(TEXT("Old status expiration cannot restore dead movement"), Target->GetCharacterMovement()->MovementMode.GetValue(), MOVE_None);
	ASC->OnPeriodicGameplayEffectExecuteDelegateOnSelf.Remove(Periodic);
	return !HasAnyErrors();
}

#endif
