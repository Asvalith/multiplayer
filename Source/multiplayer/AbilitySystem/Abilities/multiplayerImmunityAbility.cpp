// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/AbilityTasks/multiplayerAbilityTask_TargetActor.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayAbilityTargetData.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayEffectTypes.h"
#include "GameplayPrediction.h"
#include "multiplayer.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "Team/multiplayerTeamLibrary.h"
#include "TimerManager.h"

UmultiplayerImmunityAbility::UmultiplayerImmunityAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Immunity);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerImmunityCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerImmunityCooldownEffect::StaticClass();
}

void UmultiplayerImmunityAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	BeginAbilityPresentation(MultiplayerGameplayTags::Ability_Immunity);
	ApplyPredictionLabPendingEffect();
	ApplyGameplayEffectToOwner(
		Handle,
		ActorInfo,
		ActivationInfo,
		UmultiplayerImmunityEffect::StaticClass()->GetDefaultObject<UGameplayEffect>(),
		GetAbilityLevel());
	TrackPrediction(TEXT("Immunity"));

	const UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	const FPredictionKey PredictionKey = SourceASC != nullptr
		? SourceASC->GetPredictionKeyForNewAction()
		: FPredictionKey();
	const bool bIsAuthority = ActorInfo != nullptr && ActorInfo->IsNetAuthority();
	LogPredictionState(
		bIsAuthority ? TEXT("AuthorityCommitted") : TEXT("PredictedCommitted"),
		TEXT("Immunity"),
		PredictionKey.Current);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_IMMUNITY Phase=%s Ability=Immunity Spec=%s PredictionKey=%s Avatar=%s"),
		bIsAuthority ? TEXT("AuthorityCommitted") : TEXT("PredictedCommitted"),
		*Handle.ToString(),
		*PredictionKey.ToString(),
		*GetNameSafe(GetAvatarActorFromActorInfo()));

	UE_LOG(
		LogMultiplayerGAS,
		Log,
		TEXT("%s applied predicted immunity to %s"),
		ActorInfo != nullptr && ActorInfo->IsNetAuthority() ? TEXT("Server") : TEXT("Client"),
		*GetNameSafe(GetAvatarActorFromActorInfo()));

	CompleteAbilityPresentation();
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
