// Copyright Epic Games, Inc. All Rights Reserved.

// 回归持续上限 GE 移除、嵌套属性通知以及成长／被动效果替换时的资源不变量。
// 检查实际聚合值和基础值，不能只钳制显示结果而保留隐藏超额资源。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace
{
	// 使用真实角色、属性聚合器和活动 GE；不经过成长组件的额外补偿，直接验证属性集自己的不变量。
	struct FSettlementAuditWorld
	{
		UWorld* World;

		// 建立仅需真实角色、ASC 和属性聚合器的独立世界，隔离 AI 与导航。
		FSettlementAuditWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &Values);
			GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
		}

		// 结束世界中的真实效果与属性订阅，不让持续修饰器跨用例保留。
		~FSettlementAuditWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}

		// 创建使用正式属性集与成长初始化的角色，不提供额外测试补偿逻辑。
		AmultiplayerCombatCharacter* Character()
		{
			return World->SpawnActor<AmultiplayerCombatCharacter>();
		}
	};

	// 把资源与对应上限配对，让生命、能量、护盾接受同一组不变量断言。
	struct FCapacityAttributes
	{
		const TCHAR* Name;
		FGameplayAttribute Value;
		FGameplayAttribute Maximum;
	};

	// 返回三种持续资源的真实属性标识，不创建另一份属性数值模型。
	TArray<FCapacityAttributes> CapacityAttributes()
	{
		return {
			{ TEXT("Health"), UmultiplayerCombatAttributes::GetHealthAttribute(), UmultiplayerCombatAttributes::GetMaxHealthAttribute() },
			{ TEXT("Energy"), UmultiplayerCombatAttributes::GetEnergyAttribute(), UmultiplayerCombatAttributes::GetMaxEnergyAttribute() },
			{ TEXT("Shield"), UmultiplayerCombatAttributes::GetShieldAttribute(), UmultiplayerCombatAttributes::GetMaxShieldAttribute() }
		};
	}

	// 挂载真实无限上限增益，移除时必须由属性集处理资源钳制而非成长组件兜底。
	FActiveGameplayEffectHandle ApplyCapacityBonus(UAbilitySystemComponent& ASC, const FGameplayAttribute& Maximum)
	{
		auto* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
		Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;
		auto& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
		Modifier.Attribute = Maximum;
		Modifier.ModifierOp = EGameplayModOp::Additive;
		Modifier.ModifierMagnitude = FScalableFloat(100.f);
		return ASC.ApplyGameplayEffectSpecToSelf(FGameplayEffectSpec(Effect, ASC.MakeEffectContext(), 1.f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCapacityEffectRemovalTest, "Multiplayer.Combat.Settlement.CapacityEffectRemoval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 持续加上限不执行 PostGameplayEffectExecute；撤销效果后仍必须钳住 current 和 base，避免隐藏超额资源。
bool FCoopCombatCapacityEffectRemovalTest::RunTest(const FString& Parameters)
{
	FSettlementAuditWorld Fixture;
	for (const auto& Attributes : CapacityAttributes())
	{
		auto* Pawn = Fixture.Character();
		auto* ASC = Pawn->GetAbilitySystemComponent();
		const auto Bonus = ApplyCapacityBonus(*ASC, Attributes.Maximum);
		if (!TestTrue(*FString::Printf(TEXT("%s has a real active capacity effect"), Attributes.Name), Bonus.IsValid())) return false;
		TestEqual(*FString::Printf(TEXT("%s capacity bonus raises the actual aggregate"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Maximum), 200.f);
		ASC->SetNumericAttributeBase(Attributes.Value, 180.f);
		int32 Changes = 0;
		const auto Delegate = ASC->GetGameplayAttributeValueChangeDelegate(Attributes.Value).AddLambda(
			[&Changes](const FOnAttributeChangeData& Change) { if (Change.NewValue != Change.OldValue) ++Changes; });
		ASC->RemoveActiveGameplayEffect(Bonus);
		TestEqual(*FString::Printf(TEXT("%s capacity returns to the original maximum"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Maximum), 100.f);
		TestEqual(*FString::Printf(TEXT("%s current is clamped when its capacity effect is removed"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Value), 100.f);
		TestEqual(*FString::Printf(TEXT("%s base cannot retain hidden overflow"), Attributes.Name), ASC->GetNumericAttributeBase(Attributes.Value), 100.f);
		TestEqual(*FString::Printf(TEXT("%s capacity reduction emits one actual value change"), Attributes.Name), Changes, 1);
		const auto SecondBonus = ApplyCapacityBonus(*ASC, Attributes.Maximum);
		TestEqual(*FString::Printf(TEXT("%s restoring capacity never restores discarded overflow"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Value), 100.f);
		TestEqual(*FString::Printf(TEXT("%s capacity growth does not emit a redundant value change"), Attributes.Name), Changes, 1);
		ASC->GetGameplayAttributeValueChangeDelegate(Attributes.Value).Remove(Delegate);
		ASC->RemoveActiveGameplayEffect(SecondBonus);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatNestedCapacityReductionTest, "Multiplayer.Combat.Settlement.NestedCapacityReduction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 第一次钳制的属性通知可再降低上限；旧调用栈不能回写旧上限，也不能再次发送同值资源通知。
bool FCoopCombatNestedCapacityReductionTest::RunTest(const FString& Parameters)
{
	FSettlementAuditWorld Fixture;
	for (const auto& Attributes : CapacityAttributes())
	{
		auto* Pawn = Fixture.Character();
		auto* ASC = Pawn->GetAbilitySystemComponent();
		const auto Bonus = ApplyCapacityBonus(*ASC, Attributes.Maximum);
		if (!TestTrue(TEXT("Nested test owns its actual capacity effect"), Bonus.IsValid())) return false;
		ASC->SetNumericAttributeBase(Attributes.Value, 180.f);
		int32 Changes = 0;
		bool bReducedAgain = false;
		const auto Delegate = ASC->GetGameplayAttributeValueChangeDelegate(Attributes.Value).AddLambda(
			[ASC, &Attributes, &Changes, &bReducedAgain](const FOnAttributeChangeData& Change)
			{
				if (Change.NewValue == Change.OldValue) return;
				++Changes;
				if (!bReducedAgain)
				{
					bReducedAgain = true;
					ASC->SetNumericAttributeBase(Attributes.Maximum, 80.f);
				}
			});
		ASC->RemoveActiveGameplayEffect(Bonus);
		TestTrue(*FString::Printf(TEXT("%s real clamp notification triggers the nested change"), Attributes.Name), bReducedAgain);
		TestEqual(*FString::Printf(TEXT("%s nested capacity reduction keeps the latest maximum"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Maximum), 80.f);
		TestEqual(*FString::Printf(TEXT("%s outer cleanup cannot overwrite the nested value"), Attributes.Name), ASC->GetNumericAttribute(Attributes.Value), 80.f);
		TestEqual(*FString::Printf(TEXT("%s nested clamp also updates the underlying base"), Attributes.Name), ASC->GetNumericAttributeBase(Attributes.Value), 80.f);
		TestEqual(*FString::Printf(TEXT("%s emits only the two distinct reductions"), Attributes.Name), Changes, 2);
		ASC->GetGameplayAttributeValueChangeDelegate(Attributes.Value).Remove(Delegate);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCapacityBonusReplacementTest, "Multiplayer.Combat.Settlement.CapacityBonusReplacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 替换成长/被动效果是一笔装配修改；不能把移除旧效果时的中间上限当成最终上限永久扣血。
bool FCoopCombatCapacityBonusReplacementTest::RunTest(const FString& Parameters)
{
	FSettlementAuditWorld Fixture;
	auto* Pawn = Fixture.Character();
	auto* ASC = Pawn->GetAbilitySystemComponent();
	auto* Progression = Pawn->GetProgressionComponent();
	if (!TestTrue(TEXT("Real growth reaches level two"), Progression->AwardExperience(100, FGuid::NewGuid()))) return false;
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 104.f);
	TestEqual(TEXT("Level-two fixture starts above the base capacity"), Pawn->GetMaxHealth(), 105.f);
	if (!TestTrue(TEXT("Real growth replaces its existing level bonus"), Progression->AwardExperience(200, FGuid::NewGuid()))) return false;
	TestEqual(TEXT("Level-three growth has the correct final maximum"), Pawn->GetMaxHealth(), 110.f);
	TestEqual(TEXT("Growing capacity neither heals nor damages the existing health"), Pawn->GetHealth(), 104.f);
	TestEqual(TEXT("Growth replacement preserves the actual health base"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute()), 104.f);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::Fortitude);
	TestEqual(TEXT("Real Fortitude increases the final capacity without healing"), Pawn->GetMaxHealth(), 130.f);
	TestEqual(TEXT("Equipping capacity never fills the added health"), Pawn->GetHealth(), 104.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 129.f);
	Progression->RequestLearnPassive(EMultiplayerCombatPassive::Fortitude);
	TestEqual(TEXT("The equipped Fortitude really upgrades to level two"), Progression->GetPassiveLevel(EMultiplayerCombatPassive::Fortitude), 2);
	TestEqual(TEXT("Upgraded Fortitude has the correct final capacity"), Pawn->GetMaxHealth(), 150.f);
	TestEqual(TEXT("Replacing the passive cannot damage health against a temporary lower maximum"), Pawn->GetHealth(), 129.f);
	Progression->RequestEquipPassive(EMultiplayerCombatPassive::None);
	TestEqual(TEXT("Actual unequip still lowers the final capacity"), Pawn->GetMaxHealth(), 110.f);
	TestEqual(TEXT("Actual final capacity reduction still discards excess health"), Pawn->GetHealth(), 110.f);
	TestEqual(TEXT("Final unequip clamps base as well as current"), ASC->GetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute()), 110.f);
	return !HasAnyErrors();
}

#endif
