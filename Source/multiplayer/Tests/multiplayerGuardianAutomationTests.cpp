// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AbilitySystemInterface.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AI/Guardian/multiplayerGuardianCharacter.h"
#include "GameplayEffect.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerGuardianCppContractTest,
	"multiplayer.GAS.Guardian.CppContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerGuardianCppContractTest::RunTest(const FString& Parameters)
{
	const AmultiplayerGuardianCharacter* Guardian =
		GetDefault<AmultiplayerGuardianCharacter>();
	TestNotNull(TEXT("Guardian CDO exists"), Guardian);
	if (Guardian == nullptr)
	{
		return false;
	}

	TestTrue(
		TEXT("Guardian exposes an AbilitySystemComponent"),
		Guardian->GetAbilitySystemComponent() != nullptr);
	TestTrue(
		TEXT("Guardian implements the GAS interface"),
		AmultiplayerGuardianCharacter::StaticClass()->ImplementsInterface(
			UAbilitySystemInterface::StaticClass()));
	TestTrue(
		TEXT("Guardian implements the co-op team interface"),
		AmultiplayerGuardianCharacter::StaticClass()->ImplementsInterface(
			UmultiplayerCoopTeamAgentInterface::StaticClass()));
	TestEqual(
		TEXT("Guardian defaults to the hostile team"),
		ImultiplayerCoopTeamAgentInterface::Execute_GetCoopTeamId(
			const_cast<AmultiplayerGuardianCharacter*>(Guardian)),
		MultiplayerTeams::Enemies);
	const FIntProperty* RequiredChannelersProperty = FindFProperty<FIntProperty>(
		AmultiplayerGuardianCharacter::StaticClass(),
		TEXT("RequiredChannelers"));
	TestNotNull(
		TEXT("Guardian exposes channel count as data-only class configuration"),
		RequiredChannelersProperty);
	if (RequiredChannelersProperty != nullptr)
	{
		TestEqual(
			TEXT("One player channels while the partner attacks by default"),
			RequiredChannelersProperty->GetPropertyValue_InContainer(Guardian),
			1);
		TestFalse(
			TEXT("Channel count does not create a duplicate Blueprint getter"),
			RequiredChannelersProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
	}
	const FBoolProperty* RestoreShieldProperty = FindFProperty<FBoolProperty>(
		AmultiplayerGuardianCharacter::StaticClass(),
		TEXT("bShieldRestoresWhenChannelingStops"));
	TestNotNull(
		TEXT("Guardian exposes a data-only sustained-channel configuration"),
		RestoreShieldProperty);
	if (RestoreShieldProperty != nullptr)
	{
		TestTrue(
			TEXT("Leaving the channel restores the shield by default"),
			RestoreShieldProperty->GetPropertyValue_InContainer(Guardian));
	}
	const FClassProperty* MeleeEffectProperty = FindFProperty<FClassProperty>(
		AmultiplayerGuardianCharacter::StaticClass(),
		TEXT("MeleeDamageEffectClass"));
	const FFloatProperty* MeleeDamageProperty = FindFProperty<FFloatProperty>(
		AmultiplayerGuardianCharacter::StaticClass(),
		TEXT("MeleeDamage"));
	TestNotNull(TEXT("Guardian melee GE is data-configurable"), MeleeEffectProperty);
	TestNotNull(TEXT("Guardian melee magnitude is data-configurable"), MeleeDamageProperty);
	if (MeleeEffectProperty != nullptr)
	{
		TestNotNull(
			TEXT("The C++ melee path has a configured damage GE"),
			MeleeEffectProperty->GetObjectPropertyValue_InContainer(Guardian));
	}
	if (MeleeDamageProperty != nullptr)
	{
		TestTrue(
			TEXT("Guardian melee damage is positive"),
			MeleeDamageProperty->GetPropertyValue_InContainer(Guardian) > 0.0f);
	}
	const FStructProperty* PhaseProperty = FindFProperty<FStructProperty>(
		AmultiplayerGuardianCharacter::StaticClass(),
		TEXT("PhaseSnapshot"));
	TestNotNull(TEXT("Guardian owns one replicated phase snapshot"), PhaseProperty);
	if (PhaseProperty != nullptr)
	{
		const FmultiplayerGuardianPhaseSnapshot* Phase =
			PhaseProperty->ContainerPtrToValuePtr<FmultiplayerGuardianPhaseSnapshot>(
				Guardian);
		TestEqual(
			TEXT("The replicated phase starts in Acquire"),
			Phase->State,
			EmultiplayerGuardianAIState::Acquire);
	}

	const UFunction* PresentationEvent =
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("ReceiveGuardianPresentation"));
	TestNotNull(
		TEXT("Guardian exposes one consolidated Blueprint presentation event"),
		PresentationEvent);
	if (PresentationEvent != nullptr)
	{
		TestTrue(
			TEXT("Guardian presentation is cosmetic and Blueprint-implemented"),
			PresentationEvent->HasAllFunctionFlags(
				FUNC_BlueprintEvent | FUNC_BlueprintCosmetic));
	}
	TestNull(
		TEXT("Guardian health stays behind the consolidated presentation event"),
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("GetHealth")));
	TestNull(
		TEXT("Guardian phase snapshot has no duplicate Blueprint getter"),
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("GetGuardianPhaseSnapshot")));
	const UFunction* RegisterChannelerFunction =
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("RegisterChannelingParticipant"));
	const UFunction* UnregisterChannelerFunction =
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("UnregisterChannelingParticipant"));
	TestNotNull(
		TEXT("Blueprint objective actor can register one channeling participant"),
		RegisterChannelerFunction);
	TestNotNull(
		TEXT("Blueprint objective actor can unregister one channeling participant"),
		UnregisterChannelerFunction);
	if (RegisterChannelerFunction != nullptr && UnregisterChannelerFunction != nullptr)
	{
		TestTrue(
			TEXT("Register entry point is Blueprint callable"),
			RegisterChannelerFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
		TestTrue(
			TEXT("Unregister entry point is Blueprint callable"),
			UnregisterChannelerFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}
	TestNull(
		TEXT("Legacy per-signal Blueprint presentation hooks stay removed"),
		AmultiplayerGuardianCharacter::StaticClass()->FindFunctionByName(
			TEXT("ReceiveGuardianShieldPresentation")));
	TestNull(
		TEXT("Guardian does not expose duplicate per-signal Blueprint delegates"),
		FindFProperty<FProperty>(
			AmultiplayerGuardianCharacter::StaticClass(),
			TEXT("OnGuardianPhaseChanged")));

	TestTrue(
		TEXT("Guardian melee native tag is registered"),
		MultiplayerGameplayTags::Ability_Guardian_Melee.GetTag().IsValid());
	TestTrue(
		TEXT("Guardian shield native tag is registered"),
		MultiplayerGameplayTags::State_Guardian_Shielded.GetTag().IsValid());
	TestTrue(
		TEXT("Guardian channeling native tag is registered"),
		MultiplayerGameplayTags::State_Guardian_Channeling.GetTag().IsValid());
	TestTrue(
		TEXT("Guardian shield cue contract is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Guardian_State_Shield.GetTag().IsValid());
	TestTrue(
		TEXT("Guardian telegraph cue contract is registered"),
		MultiplayerGameplayTags::GameplayCue_Coop_Guardian_Telegraph.GetTag().IsValid());

	return true;
}

#endif
