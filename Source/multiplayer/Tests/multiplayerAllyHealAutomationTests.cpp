// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"
#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_HealTarget.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerAllyHealTargetDataSchemaTest,
	"multiplayer.GAS.AllyHeal.TargetDataSchema",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerAllyHealTargetDataSchemaTest::RunTest(
	const FString& Parameters)
{
	AActor* Candidate = nullptr;
	TestEqual(
		TEXT("Empty TargetData is rejected as an invalid schema"),
		ValidateMultiplayerHealTargetDataSchema(
			FGameplayAbilityTargetDataHandle(),
			Candidate),
		EmultiplayerHealTargetResult::InvalidSchema);

	FGameplayAbilityTargetDataHandle WrongTypeData(
		new FGameplayAbilityTargetData_SingleTargetHit());
	TestEqual(
		TEXT("HitResult TargetData cannot impersonate an ally actor candidate"),
		ValidateMultiplayerHealTargetDataSchema(WrongTypeData, Candidate),
		EmultiplayerHealTargetResult::InvalidSchema);

	FGameplayAbilityTargetDataHandle EmptyActorArrayData(
		new FGameplayAbilityTargetData_ActorArray());
	TestEqual(
		TEXT("ActorArray must contain exactly one candidate"),
		ValidateMultiplayerHealTargetDataSchema(
			EmptyActorArrayData,
			Candidate),
		EmultiplayerHealTargetResult::InvalidSchema);

	FGameplayAbilityTargetData_ActorArray* NullActorData =
		new FGameplayAbilityTargetData_ActorArray();
	NullActorData->TargetActorArray.Add(nullptr);
	FGameplayAbilityTargetDataHandle NullActorHandle(NullActorData);
	TestEqual(
		TEXT("A single invalid actor reference is rejected as no target"),
		ValidateMultiplayerHealTargetDataSchema(NullActorHandle, Candidate),
		EmultiplayerHealTargetResult::NoTarget);

	TestEqual(
		TEXT("Client summary keeps semantic target rejection distinct"),
		FString(GetMultiplayerHealTargetResultName(
			EmultiplayerHealTargetResult::AuthorityRejected)),
		FString(TEXT("AuthorityRejected")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerAllyHealBlueprintContractTest,
	"multiplayer.GAS.AllyHeal.BlueprintContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerAllyHealBlueprintContractTest::RunTest(
	const FString& Parameters)
{
	UClass* HealClass = UmultiplayerHealAbility::StaticClass();
	const UmultiplayerHealAbility* HealCDO =
		GetDefault<UmultiplayerHealAbility>();
	TestNotNull(TEXT("Heal ability CDO exists"), HealCDO);
	TestEqual(
		TEXT("Heal remains locally predicted"),
		HealCDO->GetNetExecutionPolicy(),
		EGameplayAbilityNetExecutionPolicy::LocalPredicted);

	const FFloatProperty* RangeProperty = FindFProperty<FFloatProperty>(
		HealClass,
		TEXT("HealTargetRange"));
	const FFloatProperty* SweepProperty = FindFProperty<FFloatProperty>(
		HealClass,
		TEXT("HealTargetSweepRadius"));
	TestNotNull(TEXT("Blueprint defaults expose heal range"), RangeProperty);
	TestNotNull(TEXT("Blueprint defaults expose target sweep radius"), SweepProperty);
	if (RangeProperty != nullptr && HealCDO != nullptr)
	{
		TestTrue(
			TEXT("Default ally heal range is positive"),
			RangeProperty->GetPropertyValue_InContainer(HealCDO) > 0.0f);
		TestTrue(
			TEXT("Heal range is editable on class defaults"),
			RangeProperty->HasAllPropertyFlags(
				CPF_Edit | CPF_DisableEditOnInstance));
		TestFalse(
			TEXT("Heal range does not create an unused Blueprint getter"),
			RangeProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));
	}
	if (SweepProperty != nullptr && HealCDO != nullptr)
	{
		TestTrue(
			TEXT("Default target sweep radius is positive"),
			SweepProperty->GetPropertyValue_InContainer(HealCDO) > 0.0f);
	}

	const UFunction* PreviewEvent = HealClass->FindFunctionByName(
		TEXT("K2_OnHealTargetPreviewed"));
	TestNotNull(TEXT("Blueprint has a presentation-only target preview event"), PreviewEvent);
	TestNull(
		TEXT("Blueprint has no authority-side heal application hook"),
		HealClass->FindFunctionByName(TEXT("K2_OnAuthoritativeHealApplied")));
	TestTrue(
		TEXT("Heal targeting is implemented by an AbilityTask"),
		UmultiplayerAbilityTask_HealTarget::StaticClass()->IsChildOf(
			UAbilityTask::StaticClass()));
	TestNull(
		TEXT("The C++-only Heal task is not exposed as an unused Blueprint node"),
		UmultiplayerAbilityTask_HealTarget::StaticClass()->FindFunctionByName(
			TEXT("CreateHealTargetTask")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
