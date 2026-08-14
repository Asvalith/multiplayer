// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "AbilitySystem/multiplayerAbilitySystemGlobals.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "AbilitySystem/multiplayerGameplayAbilityTargetData.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AbilitySystem/Executions/multiplayerDamageExecution.h"
#include "AbilitySystemGlobals.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "GameplayAbilitiesDeveloperSettings.h"
#include "GameplayCueInterface.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"
#include "Input/multiplayerInputConfig.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "multiplayerCharacter.h"
#include "multiplayerGASTargetDummy.h"
#include "multiplayerVictoryPresenterComponent.h"
#include "Player/multiplayerGASPlayerState.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "UI/multiplayerGASHUDWidget.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerGASConfigurationTest,
	"multiplayer.GAS.Configuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerGASConfigurationTest::RunTest(const FString& Parameters)
{
	const auto EffectHasCue = [](const UGameplayEffect* Effect, const FGameplayTag& CueTag)
	{
		if (Effect == nullptr)
		{
			return false;
		}

		for (const FGameplayEffectCue& Cue : Effect->GameplayCues)
		{
			if (Cue.GameplayCueTags.HasTagExact(CueTag))
			{
				return true;
			}
		}
		return false;
	};

	TestTrue(
		TEXT("Damage input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Damage.GetTag().IsValid());
	TestTrue(
		TEXT("Heal input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Heal.GetTag().IsValid());
	TestTrue(
		TEXT("Immunity input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Immunity.GetTag().IsValid());
	TestTrue(
		TEXT("Cooperative player team tag is registered"),
		MultiplayerGameplayTags::Team_Player.GetTag().IsValid());
	TestTrue(
		TEXT("Hostile enemy team tag is registered"),
		MultiplayerGameplayTags::Team_Enemy.GetTag().IsValid());
	TestNotEqual(
		TEXT("Player and enemy identities are distinct"),
		MultiplayerGameplayTags::Team_Player.GetTag(),
		MultiplayerGameplayTags::Team_Enemy.GetTag());
	TestTrue(
		TEXT("Damage cast GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Damage_Cast.GetTag().IsValid());
	TestTrue(
		TEXT("Damage impact GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact.GetTag().IsValid());
	TestTrue(
		TEXT("Heal cast GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Heal_Cast.GetTag().IsValid());
	TestTrue(
		TEXT("Heal result GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Heal_Result.GetTag().IsValid());
	TestTrue(
		TEXT("Immunity GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_State_Immunity.GetTag().IsValid());
	TestTrue(
		TEXT("Vulnerability GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability.GetTag().IsValid());
	TestTrue(
		TEXT("Death GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Death.GetTag().IsValid());
	TestTrue(
		TEXT("Prediction pending GameplayCue tag is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending.GetTag().IsValid());
	TestTrue(
		TEXT("Forced rejection lab tag is registered"),
		MultiplayerGameplayTags::Debug_ForceReject_Immunity.GetTag().IsValid());
	TestTrue(
		TEXT("GAS PlayerState implements the cooperative team interface"),
		AmultiplayerGASPlayerState::StaticClass()->ImplementsInterface(
			UmultiplayerCoopTeamAgentInterface::StaticClass()));
	TestTrue(
		TEXT("GAS target dummy implements the cooperative team interface"),
		AmultiplayerGASTargetDummy::StaticClass()->ImplementsInterface(
			UmultiplayerCoopTeamAgentInterface::StaticClass()));
	TestTrue(
		TEXT("Character has a native GameplayCue presentation interface"),
		AmultiplayerCharacter::StaticClass()->ImplementsInterface(
			UGameplayCueInterface::StaticClass()));
	TestTrue(
		TEXT("Target dummy has a native GameplayCue presentation interface"),
		AmultiplayerGASTargetDummy::StaticClass()->ImplementsInterface(
			UGameplayCueInterface::StaticClass()));
	TestNotEqual(
		TEXT("Replicated player and enemy TeamIds are distinct"),
		GetDefault<AmultiplayerGASPlayerState>()->GetCoopTeamId_Implementation(),
		GetDefault<AmultiplayerGASTargetDummy>()->GetCoopTeamId_Implementation());

	const UmultiplayerDamageAbility* DamageAbility = GetDefault<UmultiplayerDamageAbility>();
	const UmultiplayerHealAbility* HealAbility = GetDefault<UmultiplayerHealAbility>();
	const UmultiplayerImmunityAbility* ImmunityAbility = GetDefault<UmultiplayerImmunityAbility>();
	TestEqual(
		TEXT("Damage ability is locally predicted"),
		DamageAbility->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestEqual(
		TEXT("Heal ability is locally predicted"),
		HealAbility->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::LocalPredicted);
	TestEqual(
		TEXT("Immunity ability is locally predicted"),
		ImmunityAbility->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::LocalPredicted);

	const UmultiplayerDamageEffect* DamageEffect = GetDefault<UmultiplayerDamageEffect>();
	TestTrue(
		TEXT("Damage effect has the negative damage asset tag"),
		DamageEffect->GetAssetTags().HasTagExact(
			MultiplayerGameplayTags::Effect_Negative_Damage));
	TestTrue(
		TEXT("Damage effect uses the server damage ExecutionCalculation"),
		DamageEffect->Executions.Num() == 1
			&& DamageEffect->Executions[0].CalculationClass
				== UmultiplayerDamageExecution::StaticClass());
	TestTrue(
		TEXT("Damage effect emits the server-confirmed impact Cue"),
		EffectHasCue(
			DamageEffect,
			MultiplayerGameplayTags::GameplayCue_Coop_Damage_Impact));

	const TArray<FGameplayEffectAttributeCaptureDefinition>& DamageCaptures =
		GetDefault<UmultiplayerDamageExecution>()->GetAttributeCaptureDefinitions();
	TestEqual(TEXT("Damage execution declares seven attribute captures"), DamageCaptures.Num(), 7);
	const auto TestCapture = [this, &DamageCaptures](
		const TCHAR* Description,
		const FGameplayAttribute& Attribute,
		EGameplayEffectAttributeCaptureSource ExpectedSource,
		bool bExpectedSnapshot)
	{
		const FGameplayEffectAttributeCaptureDefinition* Capture =
			DamageCaptures.FindByPredicate(
				[&Attribute](const FGameplayEffectAttributeCaptureDefinition& Candidate)
				{
					return Candidate.AttributeToCapture == Attribute;
				});
		TestNotNull(Description, Capture);
		if (Capture != nullptr)
		{
			TestEqual(
				FString::Printf(TEXT("%s source"), Description),
				Capture->AttributeSource,
				ExpectedSource);
			TestEqual(
				FString::Printf(TEXT("%s snapshot policy"), Description),
				Capture->bSnapshot,
				bExpectedSnapshot);
		}
	};
	TestCapture(
		TEXT("AttackPower capture"),
		UmultiplayerAttributeSet::GetAttackPowerAttribute(),
		EGameplayEffectAttributeCaptureSource::Source,
		true);
	TestCapture(
		TEXT("CriticalChance capture"),
		UmultiplayerAttributeSet::GetCriticalChanceAttribute(),
		EGameplayEffectAttributeCaptureSource::Source,
		true);
	TestCapture(
		TEXT("CriticalMultiplier capture"),
		UmultiplayerAttributeSet::GetCriticalMultiplierAttribute(),
		EGameplayEffectAttributeCaptureSource::Source,
		true);
	TestCapture(
		TEXT("Health capture"),
		UmultiplayerAttributeSet::GetHealthAttribute(),
		EGameplayEffectAttributeCaptureSource::Target,
		false);
	TestCapture(
		TEXT("MaxHealth capture"),
		UmultiplayerAttributeSet::GetMaxHealthAttribute(),
		EGameplayEffectAttributeCaptureSource::Target,
		false);
	TestCapture(
		TEXT("Armor capture"),
		UmultiplayerAttributeSet::GetArmorAttribute(),
		EGameplayEffectAttributeCaptureSource::Target,
		false);
	TestCapture(
		TEXT("Resistance capture"),
		UmultiplayerAttributeSet::GetResistanceAttribute(),
		EGameplayEffectAttributeCaptureSource::Target,
		false);

	const UmultiplayerVulnerabilityEffect* VulnerabilityEffect =
		GetDefault<UmultiplayerVulnerabilityEffect>();
	TestEqual(
		TEXT("Vulnerability aggregates by target"),
		VulnerabilityEffect->StackingType,
		EGameplayEffectStackingType::AggregateByTarget);
	TestEqual(
		TEXT("Vulnerability is limited to three stacks"),
		VulnerabilityEffect->StackLimitCount,
		3);
	TestEqual(
		TEXT("Vulnerability refreshes duration on a successful stack"),
		VulnerabilityEffect->StackDurationRefreshPolicy,
		EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication);
	TestEqual(
		TEXT("Vulnerability clears its entire stack at expiration"),
		VulnerabilityEffect->StackExpirationPolicy,
		EGameplayEffectStackingExpirationPolicy::ClearEntireStack);
	TestTrue(
		TEXT("Vulnerability grants State.Vulnerable"),
		VulnerabilityEffect->GetGrantedTags().HasTagExact(
			MultiplayerGameplayTags::State_Vulnerable));
	TestTrue(
		TEXT("Vulnerability suppresses duplicate stacking Cue activation"),
		VulnerabilityEffect->bSuppressStackingCues);
	TestTrue(
		TEXT("Vulnerability owns a persistent lifecycle Cue"),
		EffectHasCue(
			VulnerabilityEffect,
			MultiplayerGameplayTags::GameplayCue_Coop_State_Vulnerability));

	bool bCritical = false;
	TestEqual(
		TEXT("Damage formula preserves base damage at full health"),
		UmultiplayerDamageExecution::CalculateFinalDamage(
			25.0f, 100.0f, 100.0f, 0, bCritical),
		25.0f);
	TestFalse(TEXT("Full-health damage is not critical"), bCritical);
	TestEqual(
		TEXT("Three vulnerability stacks add thirty percent damage"),
		UmultiplayerDamageExecution::CalculateFinalDamage(
			25.0f, 100.0f, 100.0f, 3, bCritical),
		32.5f);
	TestEqual(
		TEXT("Low-health critical and vulnerability multipliers are deterministic"),
		UmultiplayerDamageExecution::CalculateFinalDamage(
			25.0f, 50.0f, 100.0f, 2, bCritical),
		45.0f);
	TestTrue(TEXT("Half-health damage is critical"), bCritical);

	const FmultiplayerDamageCalculationResult AttackPowerResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 15.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0, 0.0f, 1.5f, 0.5f);
	TestTrue(
		TEXT("AttackPower is added before mitigation"),
		FMath::IsNearlyEqual(AttackPowerResult.RawDamage, 40.0f));
	TestTrue(
		TEXT("AttackPower changes final damage with default defenses"),
		FMath::IsNearlyEqual(AttackPowerResult.FinalDamage, 40.0f));

	const FmultiplayerDamageCalculationResult ArmorResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 0.0f, 100.0f, 0.0f, 100.0f, 100.0f, 0, 0.0f, 1.5f, 0.5f);
	TestTrue(
		TEXT("One hundred Armor halves damage"),
		FMath::IsNearlyEqual(ArmorResult.FinalDamage, 12.5f));

	const FmultiplayerDamageCalculationResult ResistanceResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 0.0f, 0.0f, 0.2f, 100.0f, 100.0f, 0, 0.0f, 1.5f, 0.5f);
	TestTrue(
		TEXT("Twenty percent resistance reduces damage by twenty percent"),
		FMath::IsNearlyEqual(ResistanceResult.FinalDamage, 20.0f));

	const FmultiplayerDamageCalculationResult ChanceCriticalResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0, 1.0f, 2.0f, 0.999f);
	TestTrue(TEXT("CriticalChance one always crits a living target"), ChanceCriticalResult.bCritical);
	TestTrue(
		TEXT("Captured CriticalMultiplier controls critical damage"),
		FMath::IsNearlyEqual(ChanceCriticalResult.FinalDamage, 50.0f));

	const FmultiplayerDamageCalculationResult NoChanceCriticalResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 0.0f, 0.0f, 0.0f, 100.0f, 100.0f, 0, 0.0f, 3.0f, 0.0f);
	TestFalse(
		TEXT("CriticalChance zero does not crit a full-health target"),
		NoChanceCriticalResult.bCritical);
	TestTrue(
		TEXT("CriticalMultiplier is not applied to a normal hit"),
		FMath::IsNearlyEqual(NoChanceCriticalResult.FinalDamage, 25.0f));

	const FmultiplayerDamageCalculationResult CombinedResult =
		UmultiplayerDamageExecution::CalculateDamage(
			25.0f, 15.0f, 100.0f, 0.2f, 50.0f, 100.0f, 3, 0.0f, 2.0f, 1.0f);
	TestTrue(TEXT("Low health remains a deterministic critical"), CombinedResult.bCritical);
	TestTrue(
		TEXT("Combined offensive, defensive, stack and critical factors are ordered deterministically"),
		FMath::IsNearlyEqual(CombinedResult.FinalDamage, 41.6f, 0.001f));

	const FmultiplayerDamageCalculationResult ClampedResult =
		UmultiplayerDamageExecution::CalculateDamage(
			-25.0f,
			-10.0f,
			-100.0f,
			2.0f,
			100.0f,
			100.0f,
			99,
			-1.0f,
			0.2f,
			0.5f);
	TestFalse(TEXT("Clamped invalid critical inputs do not create a critical"), ClampedResult.bCritical);
	TestTrue(TEXT("Negative raw damage clamps to zero"), FMath::IsNearlyZero(ClampedResult.FinalDamage));
	TestTrue(
		TEXT("Resistance is capped at eighty percent"),
		FMath::IsNearlyEqual(ClampedResult.ResistanceMultiplier, 0.2f, 0.0001f));
	TestTrue(
		TEXT("Vulnerability stacks are capped at three"),
		FMath::IsNearlyEqual(ClampedResult.VulnerabilityMultiplier, 1.3f));

	const FmultiplayerDamageCalculationResult NonFiniteResult =
		UmultiplayerDamageExecution::CalculateDamage(
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(),
			100.0f,
			100.0f,
			0,
			std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN());
	TestTrue(TEXT("Malformed formula inputs still produce finite damage"), FMath::IsFinite(NonFiniteResult.FinalDamage));

	FGameplayEffectContext* AllocatedContext =
		UAbilitySystemGlobals::Get().AllocGameplayEffectContext();
	const UGameplayAbilitiesDeveloperSettings* AbilitySettings =
		GetDefault<UGameplayAbilitiesDeveloperSettings>();
	UE_LOG(
		LogTemp,
		Display,
		TEXT("GAS_CONFIG GlobalsPath=%s RuntimeClass=%s"),
		*AbilitySettings->AbilitySystemGlobalsClassName.ToString(),
		*GetNameSafe(UAbilitySystemGlobals::Get().GetClass()));
	TestEqual(
		TEXT("Developer settings point at project AbilitySystemGlobals"),
		AbilitySettings->AbilitySystemGlobalsClassName.ToString(),
		FString(TEXT("/Script/multiplayer.multiplayerAbilitySystemGlobals")));
	TestTrue(
		TEXT("GameplayCue scan path is restricted to the project Cue folder"),
		AbilitySettings->GameplayCueNotifyPaths.Contains(TEXT("/Game/GAS/GameplayCues")));
	TestEqual(
		TEXT("Runtime uses project AbilitySystemGlobals class"),
		UAbilitySystemGlobals::Get().GetClass(),
		UmultiplayerAbilitySystemGlobals::StaticClass());
	TestNotNull(TEXT("AbilitySystemGlobals allocates an EffectContext"), AllocatedContext);
	if (AllocatedContext != nullptr)
	{
		TestEqual(
			TEXT("Configured globals allocate the project EffectContext"),
			AllocatedContext->GetScriptStruct(),
			FmultiplayerGameplayEffectContext::StaticStruct());
		delete AllocatedContext;
	}

	FmultiplayerGameplayEffectContext SourceContext;
	SourceContext.SetCriticalHit(true);
	SourceContext.SetHitType(EmultiplayerHitType::Critical);
	SourceContext.SetImpactImpulse(FVector(123.4f, -56.7f, 8.9f));
	TArray<uint8> SerializedContext;
	FMemoryWriter ContextWriter(SerializedContext, true);
	bool bContextWriteSucceeded = false;
	SourceContext.NetSerialize(ContextWriter, nullptr, bContextWriteSucceeded);
	TestTrue(TEXT("Project EffectContext serializes"), bContextWriteSucceeded);

	FmultiplayerGameplayEffectContext LoadedContext;
	FMemoryReader ContextReader(SerializedContext, true);
	bool bContextReadSucceeded = false;
	LoadedContext.NetSerialize(ContextReader, nullptr, bContextReadSucceeded);
	TestTrue(TEXT("Project EffectContext deserializes"), bContextReadSucceeded);
	TestTrue(TEXT("Critical flag survives NetSerialize"), LoadedContext.IsCriticalHit());
	TestEqual(
		TEXT("Hit type survives NetSerialize"),
		LoadedContext.GetHitType(),
		EmultiplayerHitType::Critical);
	TestTrue(
		TEXT("Impact impulse survives quantized NetSerialize"),
		FVector::Dist(
			LoadedContext.GetImpactImpulse(),
			SourceContext.GetImpactImpulse()) <= 0.2f);

	const UmultiplayerImmunityEffect* ImmunityEffect = GetDefault<UmultiplayerImmunityEffect>();
	TestTrue(
		TEXT("Immunity effect grants State.Immune"),
		ImmunityEffect->GetGrantedTags().HasTagExact(MultiplayerGameplayTags::State_Immune));
	TestNotNull(
		TEXT("Immunity effect contains an immunity component"),
		ImmunityEffect->FindComponent<UImmunityGameplayEffectComponent>());
	TestTrue(
		TEXT("Immunity effect owns a predicted persistent lifecycle Cue"),
		EffectHasCue(
			ImmunityEffect,
			MultiplayerGameplayTags::GameplayCue_Coop_State_Immunity));

	const UmultiplayerPredictionPendingEffect* PredictionPendingEffect =
		GetDefault<UmultiplayerPredictionPendingEffect>();
	TestEqual(
		TEXT("Prediction lab marker is a reversible duration effect"),
		PredictionPendingEffect->DurationPolicy,
		EGameplayEffectDurationType::HasDuration);
	float PredictionMarkerDuration = 0.0f;
	TestTrue(
		TEXT("Prediction lab marker duration has a static magnitude"),
		PredictionPendingEffect->DurationMagnitude.GetStaticMagnitudeIfPossible(
			1.0f,
			PredictionMarkerDuration));
	TestEqual(
		TEXT("Prediction lab marker cannot naturally expire during the rollback timeout"),
		PredictionMarkerDuration,
		30.0f);
	TestTrue(
		TEXT("Prediction lab marker has a queryable effect identity"),
		PredictionPendingEffect->GetAssetTags().HasTagExact(
			MultiplayerGameplayTags::Effect_Debug_PredictionPending));
	TestTrue(
		TEXT("Prediction lab marker owns the pending lifecycle Cue"),
		EffectHasCue(
			PredictionPendingEffect,
			MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending));

	const UmultiplayerHealingEffect* HealingEffect =
		GetDefault<UmultiplayerHealingEffect>();
	TestTrue(
		TEXT("Healing effect emits the server-confirmed result Cue"),
		EffectHasCue(
			HealingEffect,
			MultiplayerGameplayTags::GameplayCue_Coop_Heal_Result));

	const UmultiplayerDamageCooldownEffect* DamageCooldown =
		GetDefault<UmultiplayerDamageCooldownEffect>();
	TestTrue(
		TEXT("Damage cooldown grants its cooldown tag"),
		DamageCooldown->GetGrantedTags().HasTagExact(
			MultiplayerGameplayTags::Cooldown_Ability_Damage));

	const UmultiplayerInitStatsEffect* InitStats = GetDefault<UmultiplayerInitStatsEffect>();
	TestEqual(TEXT("Init stats effect configures nine attributes"), InitStats->Modifiers.Num(), 9);
	const auto TestInitModifier = [this, InitStats](
		const TCHAR* Description,
		const FGameplayAttribute& Attribute,
		float ExpectedMagnitude)
	{
		const FGameplayModifierInfo* Modifier = InitStats->Modifiers.FindByPredicate(
			[&Attribute](const FGameplayModifierInfo& Candidate)
			{
				return Candidate.Attribute == Attribute;
			});
		TestNotNull(Description, Modifier);
		if (Modifier != nullptr)
		{
			TestEqual(
				FString::Printf(TEXT("%s operation"), Description),
				Modifier->ModifierOp,
				EGameplayModOp::Override);
			float Magnitude = 0.0f;
			TestTrue(
				FString::Printf(TEXT("%s uses a static value"), Description),
				Modifier->ModifierMagnitude.GetStaticMagnitudeIfPossible(1.0f, Magnitude));
			TestTrue(
				FString::Printf(TEXT("%s value"), Description),
				FMath::IsNearlyEqual(Magnitude, ExpectedMagnitude));
		}
	};
	TestInitModifier(TEXT("MaxHealth initializer"), UmultiplayerAttributeSet::GetMaxHealthAttribute(), 100.0f);
	TestInitModifier(TEXT("Health initializer"), UmultiplayerAttributeSet::GetHealthAttribute(), 100.0f);
	TestInitModifier(TEXT("MaxEnergy initializer"), UmultiplayerAttributeSet::GetMaxEnergyAttribute(), 100.0f);
	TestInitModifier(TEXT("Energy initializer"), UmultiplayerAttributeSet::GetEnergyAttribute(), 100.0f);
	TestInitModifier(TEXT("AttackPower initializer"), UmultiplayerAttributeSet::GetAttackPowerAttribute(), 0.0f);
	TestInitModifier(TEXT("Armor initializer"), UmultiplayerAttributeSet::GetArmorAttribute(), 0.0f);
	TestInitModifier(TEXT("CriticalChance initializer"), UmultiplayerAttributeSet::GetCriticalChanceAttribute(), 0.0f);
	TestInitModifier(TEXT("CriticalMultiplier initializer"), UmultiplayerAttributeSet::GetCriticalMultiplierAttribute(), 1.5f);
	TestInitModifier(TEXT("Resistance initializer"), UmultiplayerAttributeSet::GetResistanceAttribute(), 0.0f);

	const UmultiplayerAttributeSet* AttributeDefaults = GetDefault<UmultiplayerAttributeSet>();
	TestTrue(TEXT("AttackPower default preserves legacy damage"), FMath::IsNearlyZero(AttributeDefaults->GetAttackPower()));
	TestTrue(TEXT("Armor default preserves legacy damage"), FMath::IsNearlyZero(AttributeDefaults->GetArmor()));
	TestTrue(TEXT("CriticalChance default preserves deterministic tests"), FMath::IsNearlyZero(AttributeDefaults->GetCriticalChance()));
	TestTrue(TEXT("CriticalMultiplier defaults to one point five"), FMath::IsNearlyEqual(AttributeDefaults->GetCriticalMultiplier(), 1.5f));
	TestTrue(TEXT("Resistance default preserves legacy damage"), FMath::IsNearlyZero(AttributeDefaults->GetResistance()));
	const auto TestReplicatedCombatAttribute = [this](
		const TCHAR* Description,
		FName PropertyName)
	{
		const FProperty* Property = FindFProperty<FProperty>(
			UmultiplayerAttributeSet::StaticClass(),
			PropertyName);
		TestNotNull(Description, Property);
		if (Property != nullptr)
		{
			TestTrue(
				FString::Printf(TEXT("%s is replicated"), Description),
				Property->HasAnyPropertyFlags(CPF_Net));
			TestTrue(
				FString::Printf(TEXT("%s uses RepNotify"), Description),
				Property->HasAnyPropertyFlags(CPF_RepNotify));
		}
	};
	TestReplicatedCombatAttribute(
		TEXT("AttackPower"),
		GET_MEMBER_NAME_CHECKED(UmultiplayerAttributeSet, AttackPower));
	TestReplicatedCombatAttribute(
		TEXT("Armor"),
		GET_MEMBER_NAME_CHECKED(UmultiplayerAttributeSet, Armor));
	TestReplicatedCombatAttribute(
		TEXT("CriticalChance"),
		GET_MEMBER_NAME_CHECKED(UmultiplayerAttributeSet, CriticalChance));
	TestReplicatedCombatAttribute(
		TEXT("CriticalMultiplier"),
		GET_MEMBER_NAME_CHECKED(UmultiplayerAttributeSet, CriticalMultiplier));
	TestReplicatedCombatAttribute(
		TEXT("Resistance"),
		GET_MEMBER_NAME_CHECKED(UmultiplayerAttributeSet, Resistance));

	UmultiplayerAttributeSet* AttributeClampProbe = NewObject<UmultiplayerAttributeSet>();
	float AttributeProbeValue = std::numeric_limits<float>::quiet_NaN();
	AttributeClampProbe->PreAttributeChange(
		UmultiplayerAttributeSet::GetAttackPowerAttribute(),
		AttributeProbeValue);
	TestTrue(TEXT("AttackPower rejects non-finite values"), FMath::IsNearlyZero(AttributeProbeValue));
	AttributeProbeValue = 2.0f;
	AttributeClampProbe->PreAttributeChange(
		UmultiplayerAttributeSet::GetCriticalChanceAttribute(),
		AttributeProbeValue);
	TestTrue(TEXT("CriticalChance clamps to one"), FMath::IsNearlyEqual(AttributeProbeValue, 1.0f));
	AttributeProbeValue = -1.0f;
	AttributeClampProbe->PreAttributeChange(
		UmultiplayerAttributeSet::GetCriticalMultiplierAttribute(),
		AttributeProbeValue);
	TestTrue(TEXT("CriticalMultiplier clamps to one"), FMath::IsNearlyEqual(AttributeProbeValue, 1.0f));
	AttributeProbeValue = std::numeric_limits<float>::infinity();
	AttributeClampProbe->PreAttributeChange(
		UmultiplayerAttributeSet::GetResistanceAttribute(),
		AttributeProbeValue);
	TestTrue(TEXT("Resistance rejects non-finite values"), FMath::IsNearlyZero(AttributeProbeValue));
	AttributeProbeValue = 1.0f;
	AttributeClampProbe->PreAttributeChange(
		UmultiplayerAttributeSet::GetResistanceAttribute(),
		AttributeProbeValue);
	TestTrue(
		TEXT("Resistance clamps to eighty percent"),
		FMath::IsNearlyEqual(AttributeProbeValue, 0.8f));

	const UmultiplayerInputConfig* InputConfig = LoadObject<UmultiplayerInputConfig>(
		nullptr,
		TEXT("/Game/GAS/M1/DA_GAS_InputConfig.DA_GAS_InputConfig"));
	TestNotNull(TEXT("M1 InputConfig asset exists"), InputConfig);
	if (InputConfig != nullptr)
	{
		TestEqual(
			TEXT("M1 InputConfig contains three ability actions"),
			InputConfig->GetAbilityInputActions().Num(),
			3);
		TestNotNull(
			TEXT("Damage InputAction is mapped to its InputTag"),
			InputConfig->FindAbilityInputActionForTag(
				MultiplayerGameplayTags::InputTag_Ability_Damage));
		TestNotNull(
			TEXT("Heal InputAction is mapped to its InputTag"),
			InputConfig->FindAbilityInputActionForTag(
				MultiplayerGameplayTags::InputTag_Ability_Heal));
		TestNotNull(
			TEXT("Immunity InputAction is mapped to its InputTag"),
			InputConfig->FindAbilityInputActionForTag(
				MultiplayerGameplayTags::InputTag_Ability_Immunity));
	}

	const UInputMappingContext* AbilityMapping = LoadObject<UInputMappingContext>(
		nullptr,
		TEXT("/Game/GAS/M1/IMC_GAS_Abilities.IMC_GAS_Abilities"));
	TestNotNull(TEXT("M1 ability MappingContext asset exists"), AbilityMapping);
	if (AbilityMapping != nullptr)
	{
		TestEqual(
			TEXT("M1 ability MappingContext contains three key mappings"),
			AbilityMapping->GetMappings().Num(),
			3);
	}

	const UmultiplayerAbilitySet* AbilitySet = LoadObject<UmultiplayerAbilitySet>(
		nullptr,
		TEXT("/Game/GAS/M1/DA_GAS_DefaultAbilitySet.DA_GAS_DefaultAbilitySet"));
	TestNotNull(TEXT("M1 default AbilitySet asset exists"), AbilitySet);
	if (AbilitySet != nullptr)
	{
		TestEqual(
			TEXT("M1 AbilitySet grants three abilities"),
			AbilitySet->GetGrantedAbilities().Num(),
			3);
		TestEqual(
			TEXT("M1 AbilitySet grants one initialization effect"),
			AbilitySet->GetGrantedEffects().Num(),
			1);
	}

	const UBlueprint* CharacterBlueprint = LoadObject<UBlueprint>(
		nullptr,
		TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter.BP_ThirdPersonCharacter"));
	TestNotNull(TEXT("Third-person Character Blueprint exists"), CharacterBlueprint);
	if (CharacterBlueprint != nullptr && CharacterBlueprint->GeneratedClass != nullptr)
	{
		const AmultiplayerCharacter* CharacterCDO =
			Cast<AmultiplayerCharacter>(CharacterBlueprint->GeneratedClass->GetDefaultObject());
		TestNotNull(TEXT("Character Blueprint derives from GAS character"), CharacterCDO);
		if (CharacterCDO != nullptr)
		{
			TestTrue(
				TEXT("Character uses M1 InputConfig"),
				CharacterCDO->GetAbilityInputConfig() == InputConfig);
			TestTrue(
				TEXT("Character uses M1 MappingContext"),
				CharacterCDO->GetAbilityMappingContext() == AbilityMapping);
			TestTrue(
				TEXT("Character uses M1 AbilitySet"),
				CharacterCDO->GetStartupAbilitySet() == AbilitySet);

			const UmultiplayerVictoryPresenterComponent* VictoryPresenter =
				CharacterCDO->FindComponentByClass<UmultiplayerVictoryPresenterComponent>();
			TestNotNull(TEXT("Character owns the victory presenter"), VictoryPresenter);
			const TSubclassOf<UUserWidget> ExpectedVictoryWidget = LoadClass<UUserWidget>(
				nullptr,
				TEXT("/Game/UI/winandquit.winandquit_C"));
			TestNotNull(TEXT("Victory widget Blueprint class exists"), ExpectedVictoryWidget.Get());
			if (VictoryPresenter != nullptr && ExpectedVictoryWidget != nullptr)
			{
				TestEqual(
					TEXT("Victory presenter is configured with winandquit"),
					VictoryPresenter->GetVictoryWidgetClass().Get(),
					ExpectedVictoryWidget.Get());
				TestEqual(
					TEXT("Victory presenter targets the existing restart button"),
					VictoryPresenter->GetRestartButtonName(),
					FName(TEXT("\u91cd\u65b0\u5f00\u59cb")));

				UUserWidget* VictoryWidgetProbe = NewObject<UUserWidget>(
					GetTransientPackage(),
					ExpectedVictoryWidget);
				TestNotNull(
					TEXT("Victory widget can be instantiated for configuration validation"),
					VictoryWidgetProbe);
				if (VictoryWidgetProbe != nullptr)
				{
					TestTrue(
						TEXT("Victory widget tree initializes"),
						VictoryWidgetProbe->Initialize());
					TestNotNull(
						TEXT("Configured restart button exists in the victory widget tree"),
						Cast<UButton>(VictoryWidgetProbe->GetWidgetFromName(
							VictoryPresenter->GetRestartButtonName())));
				}
			}
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerGASDamageIntentUnitTest,
	"multiplayer.GAS.DamageIntent.Unit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerGASDamageIntentUnitTest::RunTest(const FString& Parameters)
{
	FmultiplayerGameplayAbilityTargetData_DamageIntent* SourceIntent =
		new FmultiplayerGameplayAbilityTargetData_DamageIntent();
	SourceIntent->ShotId = 42;
	SourceIntent->Origin = FVector(123.4f, -56.7f, 89.1f);
	SourceIntent->Direction = FVector(0.3f, 0.4f, 0.8660254f).GetSafeNormal();
	SourceIntent->ClientFireServerTimeSeconds = 1234.5f;
	FGameplayAbilityTargetDataHandle SourceHandle(SourceIntent);

	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes, true);
	bool bWriteSucceeded = false;
	SourceHandle.NetSerialize(Writer, nullptr, bWriteSucceeded);
	TestTrue(TEXT("Damage Intent polymorphic handle serializes"), bWriteSucceeded);

	FGameplayAbilityTargetDataHandle LoadedHandle;
	FMemoryReader Reader(Bytes, true);
	bool bReadSucceeded = false;
	LoadedHandle.NetSerialize(Reader, nullptr, bReadSucceeded);
	TestTrue(TEXT("Damage Intent polymorphic handle deserializes"), bReadSucceeded);
	TestEqual(TEXT("Damage Intent schema contains exactly one entry"), LoadedHandle.Num(), 1);
	TestTrue(
		TEXT("Damage Intent keeps its exact ScriptStruct identity"),
		LoadedHandle.Get(0) != nullptr
			&& LoadedHandle.Get(0)->GetScriptStruct()
				== FmultiplayerGameplayAbilityTargetData_DamageIntent::StaticStruct());

	if (LoadedHandle.Num() == 1 && LoadedHandle.Get(0) != nullptr
		&& LoadedHandle.Get(0)->GetScriptStruct()
			== FmultiplayerGameplayAbilityTargetData_DamageIntent::StaticStruct())
	{
		const FmultiplayerGameplayAbilityTargetData_DamageIntent* LoadedIntent =
			static_cast<const FmultiplayerGameplayAbilityTargetData_DamageIntent*>(
				LoadedHandle.Get(0));
		TestEqual(TEXT("ShotId survives NetSerialize"), LoadedIntent->ShotId, 42u);
		TestTrue(
			TEXT("Origin survives NetQuantize10 within tolerance"),
			FVector::Distance(FVector(LoadedIntent->Origin), FVector(SourceIntent->Origin))
				<= 0.2f);
		TestTrue(
			TEXT("Direction survives NetQuantizeNormal within tolerance"),
			FVector::Distance(
				FVector(LoadedIntent->Direction),
				FVector(SourceIntent->Direction)) <= 0.01f);
		TestTrue(
			TEXT("Estimated server time survives NetSerialize"),
			FMath::IsNearlyEqual(
				LoadedIntent->ClientFireServerTimeSeconds,
				SourceIntent->ClientFireServerTimeSeconds));
		TestTrue(
			TEXT("Damage Intent exposes neither HitResult nor target Actor"),
			!LoadedIntent->HasHitResult()
				&& LoadedIntent->GetActors().IsEmpty());
	}

	const FmultiplayerGameplayAbilityTargetData_DamageIntent* SchemaIntent = nullptr;
	TestEqual(
		TEXT("Exact Damage Intent schema is accepted"),
		ValidateMultiplayerDamageIntentSchema(SourceHandle, SchemaIntent),
		EmultiplayerDamageIntentResult::Accepted);
	TestTrue(TEXT("Schema validator returns the exact intent"), SchemaIntent == SourceIntent);
	FGameplayAbilityTargetDataHandle EmptyHandle;
	TestEqual(
		TEXT("Empty target data is an invalid schema"),
		ValidateMultiplayerDamageIntentSchema(EmptyHandle, SchemaIntent),
		EmultiplayerDamageIntentResult::InvalidSchema);

	FGameplayAbilityTargetDataHandle WrongSchema(
		new FGameplayAbilityTargetData_SingleTargetHit());
	TestFalse(
		TEXT("SingleTargetHit is not accepted as the Damage Intent schema"),
		WrongSchema.Get(0)->GetScriptStruct()
			== FmultiplayerGameplayAbilityTargetData_DamageIntent::StaticStruct());
	TestEqual(
		TEXT("SingleTargetHit is rejected by the schema validator"),
		ValidateMultiplayerDamageIntentSchema(WrongSchema, SchemaIntent),
		EmultiplayerDamageIntentResult::InvalidSchema);
	FGameplayAbilityTargetDataHandle TwoIntentHandle;
	TwoIntentHandle.Add(new FmultiplayerGameplayAbilityTargetData_DamageIntent(*SourceIntent));
	TwoIntentHandle.Add(new FmultiplayerGameplayAbilityTargetData_DamageIntent(*SourceIntent));
	TestEqual(
		TEXT("Two intent entries are rejected"),
		ValidateMultiplayerDamageIntentSchema(TwoIntentHandle, SchemaIntent),
		EmultiplayerDamageIntentResult::InvalidSchema);
	FmultiplayerGameplayAbilityTargetData_DamageIntent* ZeroShotIntent =
		new FmultiplayerGameplayAbilityTargetData_DamageIntent(*SourceIntent);
	ZeroShotIntent->ShotId = 0;
	FGameplayAbilityTargetDataHandle ZeroShotHandle(ZeroShotIntent);
	TestEqual(
		TEXT("ShotId zero is rejected after exact schema validation"),
		ValidateMultiplayerDamageIntentSchema(ZeroShotHandle, SchemaIntent),
		EmultiplayerDamageIntentResult::InvalidShotId);

	FmultiplayerDamageIntentServerContext ValidationContext;
	ValidationContext.ServerEyeOrigin = FVector(1000.0f, 0.0f, 100.0f);
	ValidationContext.ServerAimDirection = FVector::ForwardVector;
	ValidationContext.ServerNowSeconds = 100.0;
	FmultiplayerDamageIntentValidationConfig ValidationConfig;
	ValidationConfig.MaxOriginErrorCm = 150.0f;
	ValidationConfig.MinAimDot = FMath::Cos(FMath::DegreesToRadians(25.0f));
	ValidationConfig.MaxPastAgeSeconds = 2.0f;
	ValidationConfig.MaxFutureLeadSeconds = 0.25f;
	FmultiplayerGameplayAbilityTargetData_DamageIntent ValidFieldsIntent;
	ValidFieldsIntent.ShotId = 1;
	ValidFieldsIntent.Origin = ValidationContext.ServerEyeOrigin;
	ValidFieldsIntent.Direction = FVector::ForwardVector;
	ValidFieldsIntent.ClientFireServerTimeSeconds = 100.0f;
	TestEqual(
		TEXT("Valid intent fields pass"),
		ValidateMultiplayerDamageIntentFields(
			ValidFieldsIntent, ValidationContext, ValidationConfig),
		EmultiplayerDamageIntentResult::Accepted);
	auto TestFieldResult = [this, &ValidationContext, &ValidationConfig](
		const TCHAR* What,
		const FmultiplayerGameplayAbilityTargetData_DamageIntent& Intent,
		EmultiplayerDamageIntentResult Expected)
	{
		TestEqual(
			What,
			ValidateMultiplayerDamageIntentFields(Intent, ValidationContext, ValidationConfig),
			Expected);
	};
	FmultiplayerGameplayAbilityTargetData_DamageIntent BoundaryIntent = ValidFieldsIntent;
	BoundaryIntent.Origin = ValidationContext.ServerEyeOrigin + FVector(150.0f, 0.0f, 0.0f);
	TestFieldResult(TEXT("Origin exactly at tolerance passes"), BoundaryIntent, EmultiplayerDamageIntentResult::Accepted);
	BoundaryIntent.Origin = ValidationContext.ServerEyeOrigin + FVector(150.2f, 0.0f, 0.0f);
	TestFieldResult(TEXT("Origin beyond tolerance is rejected"), BoundaryIntent, EmultiplayerDamageIntentResult::InvalidOrigin);
	BoundaryIntent = ValidFieldsIntent;
	BoundaryIntent.Direction = -FVector::ForwardVector;
	TestFieldResult(TEXT("Opposite aim direction is rejected"), BoundaryIntent, EmultiplayerDamageIntentResult::InvalidDirection);
	BoundaryIntent = ValidFieldsIntent;
	BoundaryIntent.Direction = FVector::ZeroVector;
	TestFieldResult(TEXT("Zero aim direction is rejected"), BoundaryIntent, EmultiplayerDamageIntentResult::InvalidDirection);
	BoundaryIntent = ValidFieldsIntent;
	BoundaryIntent.ClientFireServerTimeSeconds = 98.0f;
	TestFieldResult(TEXT("Intent exactly at maximum age passes"), BoundaryIntent, EmultiplayerDamageIntentResult::Accepted);
	BoundaryIntent.ClientFireServerTimeSeconds = 97.9f;
	TestFieldResult(TEXT("Stale intent is rejected"), BoundaryIntent, EmultiplayerDamageIntentResult::InvalidTime);
	BoundaryIntent.ClientFireServerTimeSeconds = 100.25f;
	TestFieldResult(TEXT("Intent exactly at future tolerance passes"), BoundaryIntent, EmultiplayerDamageIntentResult::Accepted);
	BoundaryIntent.ClientFireServerTimeSeconds = 100.3f;
	TestFieldResult(TEXT("Future intent beyond tolerance is rejected"), BoundaryIntent, EmultiplayerDamageIntentResult::InvalidTime);

	FmultiplayerDamageIntentGuard Guard;
	TestEqual(
		TEXT("ShotId zero is invalid"),
		Guard.TryConsume(0, 10.0),
		EmultiplayerDamageIntentResult::InvalidShotId);
	TestEqual(
		TEXT("First valid ShotId is accepted"),
		Guard.TryConsume(1, 10.0),
		EmultiplayerDamageIntentResult::Accepted);
	TestEqual(
		TEXT("Same ShotId is rejected as duplicate"),
		Guard.TryConsume(1, 11.0),
		EmultiplayerDamageIntentResult::Duplicate);
	TestEqual(
		TEXT("New ShotId inside query-rate window is rejected"),
		Guard.TryConsume(2, 10.01),
		EmultiplayerDamageIntentResult::RateLimited);
	TestEqual(
		TEXT("Rate-limited ShotId remains consumed"),
		Guard.TryConsume(2, 11.0),
		EmultiplayerDamageIntentResult::Duplicate);
	TestEqual(
		TEXT("Next monotonic ShotId recovers"),
		Guard.TryConsume(3, 11.0),
		EmultiplayerDamageIntentResult::Accepted);
	TestEqual(
		TEXT("Old ShotId is rejected as stale"),
		Guard.TryConsume(2, 12.0),
		EmultiplayerDamageIntentResult::StaleSequence);
	TestEqual(
		TEXT("Excessive forward jump is rejected"),
		Guard.TryConsume(100, 12.0),
		EmultiplayerDamageIntentResult::StaleSequence);
	TestEqual(
		TEXT("Rejected jump does not advance the high-water mark"),
		Guard.TryConsume(4, 12.0),
		EmultiplayerDamageIntentResult::Accepted);
	TestEqual(
		TEXT("Non-finite server time is rejected"),
		Guard.TryConsume(5, std::numeric_limits<double>::quiet_NaN()),
		EmultiplayerDamageIntentResult::InvalidTime);
	Guard.Reset();
	TestEqual(
		TEXT("Reset permits a new session starting at ShotId one"),
		Guard.TryConsume(1, 20.0),
		EmultiplayerDamageIntentResult::Accepted);

	return true;
}

#endif
