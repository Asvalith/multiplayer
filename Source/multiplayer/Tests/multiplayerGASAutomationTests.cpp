// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayEffectComponents/ImmunityGameplayEffectComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerGASConfigurationTest,
	"multiplayer.GAS.Configuration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerGASConfigurationTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Damage input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Damage.GetTag().IsValid());
	TestTrue(
		TEXT("Heal input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Heal.GetTag().IsValid());
	TestTrue(
		TEXT("Immunity input tag is registered"),
		MultiplayerGameplayTags::InputTag_Ability_Immunity.GetTag().IsValid());

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
		TEXT("Damage effect writes to IncomingDamage"),
		DamageEffect->Modifiers.Num() == 1
			&& DamageEffect->Modifiers[0].Attribute
				== UmultiplayerAttributeSet::GetIncomingDamageAttribute());

	const UmultiplayerImmunityEffect* ImmunityEffect = GetDefault<UmultiplayerImmunityEffect>();
	TestTrue(
		TEXT("Immunity effect grants State.Immune"),
		ImmunityEffect->GetGrantedTags().HasTagExact(MultiplayerGameplayTags::State_Immune));
	TestNotNull(
		TEXT("Immunity effect contains an immunity component"),
		ImmunityEffect->FindComponent<UImmunityGameplayEffectComponent>());

	const UmultiplayerDamageCooldownEffect* DamageCooldown =
		GetDefault<UmultiplayerDamageCooldownEffect>();
	TestTrue(
		TEXT("Damage cooldown grants its cooldown tag"),
		DamageCooldown->GetGrantedTags().HasTagExact(
			MultiplayerGameplayTags::Cooldown_Ability_Damage));

	return true;
}

#endif
