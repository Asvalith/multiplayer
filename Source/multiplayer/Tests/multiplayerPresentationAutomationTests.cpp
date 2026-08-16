// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "AbilitySystem/Abilities/multiplayerAbilityPresentationInterface.h"
#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayCueInterface.h"
#include "multiplayerCharacter.h"
#include "UI/multiplayerGASCuePresenterComponent.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FmultiplayerPresentationCppContractTest,
	"multiplayer.GAS.Presentation.CppContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FmultiplayerPresentationCppContractTest::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("Character implements the presentation-only Ability interface"),
		AmultiplayerCharacter::StaticClass()->ImplementsInterface(
			UmultiplayerAbilityPresentationInterface::StaticClass()));

	const UFunction* PresentationFunction =
		UmultiplayerAbilityPresentationInterface::StaticClass()->FindFunctionByName(
			TEXT("HandleAbilityPresentation"));
	TestNotNull(TEXT("Ability presentation hook exists"), PresentationFunction);
	if (PresentationFunction != nullptr)
	{
		TestTrue(
			TEXT("Ability presentation hook is a Blueprint event"),
			PresentationFunction->HasAnyFunctionFlags(FUNC_BlueprintEvent));
		TestFalse(
			TEXT("Blueprints implement but cannot invoke the presentation hook"),
			PresentationFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const FStructProperty* MontageProperty = FindFProperty<FStructProperty>(
		UmultiplayerGameplayAbility::StaticClass(),
		TEXT("PresentationMontage"));
	TestNotNull(TEXT("GameplayAbility exposes one shared montage config"), MontageProperty);
	if (MontageProperty != nullptr)
	{
		TestTrue(
			TEXT("Montage config uses the typed presentation struct"),
			MontageProperty->Struct.Get()
				== FmultiplayerAbilityMontageConfig::StaticStruct());
		TestTrue(
			TEXT("Montage config is editable on an Ability Blueprint CDO"),
			MontageProperty->HasAnyPropertyFlags(CPF_Edit));
		TestFalse(
			TEXT("Montage config is data-only and does not add a Blueprint getter"),
			MontageProperty->HasAnyPropertyFlags(CPF_BlueprintVisible));

		const UmultiplayerDamageAbility* DamageAbility =
			GetDefault<UmultiplayerDamageAbility>();
		const FmultiplayerAbilityMontageConfig* Config =
			MontageProperty->ContainerPtrToValuePtr<FmultiplayerAbilityMontageConfig>(
				DamageAbility);
		TestNotNull(TEXT("Damage ability has a montage config value"), Config);
		if (Config != nullptr)
		{
			TestNull(
				TEXT("C++ gameplay remains valid before a montage asset is assigned"),
				Config->Montage.Get());
			TestTrue(
				TEXT("Default presentation play rate is valid"),
				FMath::IsNearlyEqual(Config->PlayRate, 1.0f));
			TestTrue(
				TEXT("Prediction rejection stops a configured montage by default"),
				Config->bStopOnRejected);
		}
	}

	UmultiplayerGASCuePresenterComponent* CuePresenter =
		NewObject<UmultiplayerGASCuePresenterComponent>();
	TestNotNull(TEXT("Cue presentation adapter can be created without assets"), CuePresenter);
	if (CuePresenter != nullptr)
	{
		TestEqual(
			TEXT("Native debug fallback remains the pre-asset default"),
			CuePresenter->GetPresentationOwner(),
			EmultiplayerCuePresentationOwner::NativeDebugFallback);

		FGameplayCueParameters CueParameters;
		CueParameters.OriginalTag = MultiplayerGameplayTags::GameplayCue_Coop_Damage_Cast;
		TestTrue(
			TEXT("Native mode consumes a known Cue exactly once"),
			CuePresenter->HandleGameplayCue(
				EGameplayCueEvent::Executed,
				CueParameters));

		FEnumProperty* OwnerProperty = FindFProperty<FEnumProperty>(
			UmultiplayerGASCuePresenterComponent::StaticClass(),
			TEXT("PresentationOwner"));
		TestNotNull(TEXT("Cue presentation owner is an explicit enum contract"), OwnerProperty);
		if (OwnerProperty != nullptr)
		{
			void* OwnerValue = OwnerProperty->ContainerPtrToValuePtr<void>(CuePresenter);
			OwnerProperty->GetUnderlyingProperty()->SetIntPropertyValue(
				OwnerValue,
				static_cast<int64>(
					EmultiplayerCuePresentationOwner::GameplayCueAssets));
			TestFalse(
				TEXT("Asset mode never consumes a formal gameplay Cue in native lights"),
				CuePresenter->HandleGameplayCue(
					EGameplayCueEvent::Executed,
					CueParameters));

			CueParameters.OriginalTag =
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending;
			TestTrue(
				TEXT("Prediction lab marker remains an isolated native debug Cue"),
				CuePresenter->HandleGameplayCue(
					EGameplayCueEvent::OnActive,
					CueParameters));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
