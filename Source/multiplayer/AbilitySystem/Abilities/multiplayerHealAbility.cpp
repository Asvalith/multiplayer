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

UmultiplayerHealAbility::UmultiplayerHealAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Heal);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerHealCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerHealCooldownEffect::StaticClass();
}

void UmultiplayerHealAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	ActiveHealTargetTask = nullptr;
	PendingHealTarget.Reset();
	UmultiplayerAbilityTask_HealTarget* TargetTask =
		UmultiplayerAbilityTask_HealTarget::CreateHealTargetTask(
			this,
			HealTargetRange,
			HealTargetSweepRadius,
			bAllowSelfTarget,
			bFallbackToSelf,
			bRequireHealLineOfSight);
	if (TargetTask == nullptr)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	TargetTask->TargetReady.AddUObject(
		this,
		&UmultiplayerHealAbility::HandleHealTargetData);
	TargetTask->AuthorityResult.AddUObject(
		this,
		&UmultiplayerHealAbility::HandleHealAuthorityResult);
	ActiveHealTargetTask = TargetTask;
	TargetTask->ReadyForActivation();
}

void UmultiplayerHealAbility::HandleHealTargetData(
	const FGameplayAbilityTargetDataHandle& TargetData,
	EmultiplayerHealTargetResult Result)
{
	AActor* TargetActor = nullptr;
	const EmultiplayerHealTargetResult SchemaResult =
		ValidateMultiplayerHealTargetDataSchema(TargetData, TargetActor);
	if (Result != EmultiplayerHealTargetResult::Accepted
		|| SchemaResult != EmultiplayerHealTargetResult::Accepted)
	{
		if (ActiveHealTargetTask != nullptr)
		{
			ActiveHealTargetTask->SendAuthorityResultToOwner(false);
		}
		ActiveHealTargetTask = nullptr;
		PendingHealTarget.Reset();
		EndAbility(
			CurrentSpecHandle,
			CurrentActorInfo,
			CurrentActivationInfo,
			true,
			true);
		return;
	}

	const bool bIsAuthority = CurrentActorInfo != nullptr
		&& CurrentActorInfo->IsNetAuthority();
	UAbilitySystemComponent* TargetASC = nullptr;
	if (bIsAuthority
		&& !IsResolvedHealTargetStillValid(TargetActor, TargetASC))
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_HEAL_TARGET Phase=CommitRejected Reason=InvariantChanged Source=%s Target=%s"),
			*GetNameSafe(GetAvatarActorFromActorInfo()),
			*GetNameSafe(TargetActor));
		if (ActiveHealTargetTask != nullptr)
		{
			ActiveHealTargetTask->SendAuthorityResultToOwner(false);
		}
		ActiveHealTargetTask = nullptr;
		EndAbility(
			CurrentSpecHandle,
			CurrentActorInfo,
			CurrentActivationInfo,
			true,
			true);
		return;
	}

	FGameplayEffectSpecHandle HealSpec;
	if (bIsAuthority)
	{
		HealSpec = MakeOutgoingGameplayEffectSpec(
			UmultiplayerHealingEffect::StaticClass(),
			GetAbilityLevel());
		if (!HealSpec.IsValid())
		{
			if (ActiveHealTargetTask != nullptr)
			{
				ActiveHealTargetTask->SendAuthorityResultToOwner(false);
			}
			ActiveHealTargetTask = nullptr;
			EndAbility(
				CurrentSpecHandle,
				CurrentActorInfo,
				CurrentActivationInfo,
				true,
				true);
			return;
		}
	}

	if (!CommitAbility(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo))
	{
		if (bIsAuthority && ActiveHealTargetTask != nullptr)
		{
			ActiveHealTargetTask->SendAuthorityResultToOwner(false);
		}
		ActiveHealTargetTask = nullptr;
		PendingHealTarget.Reset();
		EndAbility(
			CurrentSpecHandle,
			CurrentActorInfo,
			CurrentActivationInfo,
			true,
			true);
		return;
	}

	if (CurrentActorInfo != nullptr && CurrentActorInfo->IsLocallyControlled())
	{
		PendingHealTarget = TargetActor;
		K2_OnHealTargetPreviewed(TargetActor);
	}
	BeginAbilityPresentation(MultiplayerGameplayTags::Ability_Heal);
	ExecutePredictedCue(
		MultiplayerGameplayTags::GameplayCue_Coop_Heal_Cast,
		TEXT("Heal"));

	if (!bIsAuthority)
	{
		// Keep the predicting instance alive until GameCustom1/2 reports the
		// authority TargetData + Commit result. This is a semantic target verdict,
		// distinct from ClientActivateAbilityFailed activation rejection.
		return;
	}

	if (bIsAuthority)
	{
		HealSpec.Data->SetSetByCallerMagnitude(
			MultiplayerGameplayTags::Data_Heal,
			HealingAmount);
		GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToTarget(
			*HealSpec.Data.Get(),
			TargetASC);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_HEAL_TARGET Phase=Committed Amount=%.1f Source=%s Target=%s Spec=%s PredictionKey=%s"),
			HealingAmount,
			*GetNameSafe(GetAvatarActorFromActorInfo()),
			*GetNameSafe(TargetActor),
			*CurrentSpecHandle.ToString(),
			*CurrentActivationInfo.GetActivationPredictionKey().ToString());
	}
	if (ActiveHealTargetTask != nullptr)
	{
		ActiveHealTargetTask->SendAuthorityResultToOwner(true);
	}
	ActiveHealTargetTask = nullptr;
	PendingHealTarget.Reset();

	CompleteAbilityPresentation();
	EndAbility(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo,
		true,
		false);
}

void UmultiplayerHealAbility::HandleHealAuthorityResult(bool bAccepted)
{
	const FString Message = FString::Printf(
		TEXT("GAS_HEAL_TARGET Phase=OwningClientReconciled Result=%s Target=%s Spec=%s PredictionKey=%s"),
		bAccepted ? TEXT("Accepted") : TEXT("AuthorityRejected"),
		*GetNameSafe(PendingHealTarget.Get()),
		*CurrentSpecHandle.ToString(),
		*CurrentActivationInfo.GetActivationPredictionKey().ToString());
	if (bAccepted)
	{
		UE_LOG(LogMultiplayerGAS, Display, TEXT("%s"), *Message);
	}
	else
	{
		UE_LOG(LogMultiplayerGAS, Warning, TEXT("%s"), *Message);
	}

	ActiveHealTargetTask = nullptr;
	PendingHealTarget.Reset();
	if (bAccepted)
	{
		CompleteAbilityPresentation();
	}
	else
	{
		RejectAbilityPresentation(
			MultiplayerGameplayTags::Ability_Heal,
			GetActivePresentationPredictionKey());
	}
	EndAbility(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo,
		true,
		!bAccepted);
}

bool UmultiplayerHealAbility::IsResolvedHealTargetStillValid(
	AActor* TargetActor,
	UAbilitySystemComponent*& OutTargetASC) const
{
	OutTargetASC = nullptr;
	AActor* SourceActor = GetAvatarActorFromActorInfo();
	if (!IsValid(SourceActor)
		|| !IsValid(TargetActor)
		|| TargetActor->IsActorBeingDestroyed())
	{
		return false;
	}

	const int32 SourceTeam = UmultiplayerTeamLibrary::ResolveTeamId(SourceActor);
	if (SourceTeam == MultiplayerTeams::NoTeam
		|| UmultiplayerTeamLibrary::ResolveTeamId(TargetActor) != SourceTeam)
	{
		return false;
	}

	OutTargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor);
	if (!IsValid(OutTargetASC)
		|| OutTargetASC->HasMatchingGameplayTag(
			MultiplayerGameplayTags::State_Dead)
		|| OutTargetASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		OutTargetASC = nullptr;
		return false;
	}

	return true;
}
