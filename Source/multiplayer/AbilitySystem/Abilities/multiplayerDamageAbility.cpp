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

UmultiplayerDamageAbility::UmultiplayerDamageAbility()
{
	FGameplayTagContainer AssetTags;
	AssetTags.AddTag(MultiplayerGameplayTags::Ability_Damage);
	SetAssetTags(AssetTags);

	CostGameplayEffectClass = UmultiplayerDamageCostEffect::StaticClass();
	CooldownGameplayEffectClass = UmultiplayerDamageCooldownEffect::StaticClass();
}

void UmultiplayerDamageAbility::ActivateAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	ActiveTargetTask = nullptr;
	UmultiplayerAbilityTask_TargetActor* TargetTask =
		UmultiplayerAbilityTask_TargetActor::CreateTargetActorTask(this, TargetRange);
	if (TargetTask == nullptr)
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	TargetTask->ValidData.AddDynamic(this, &UmultiplayerDamageAbility::HandleTargetData);
	ActiveTargetTask = TargetTask;
	TargetTask->ReadyForActivation();
}

void UmultiplayerDamageAbility::HandleTargetData(
	const FGameplayAbilityTargetDataHandle& TargetData)
{
	AActor* TargetActor = nullptr;
	if (TargetData.Num() == 1 && TargetData.Get(0) != nullptr)
	{
		const TArray<TWeakObjectPtr<AActor>> Actors = TargetData.Get(0)->GetActors();
		TargetActor = Actors.Num() > 0 ? Actors[0].Get() : nullptr;
	}
	const uint32 ShotId = ActiveTargetTask != nullptr
		? ActiveTargetTask->GetResolvedShotId()
		: 0;
	// The task is single-shot. Clear our transient reference before any branch
	// can end the ability, including semantic rejection and Commit failure.
	ActiveTargetTask = nullptr;

	const bool bIsAuthority = CurrentActorInfo != nullptr && CurrentActorInfo->IsNetAuthority();
	UmultiplayerAbilitySystemComponent* SourceASC =
		Cast<UmultiplayerAbilitySystemComponent>(
			GetAbilitySystemComponentFromActorInfo());
	if ((bIsAuthority && (TargetData.Num() != 1 || TargetData.Get(0) == nullptr))
		|| ShotId == 0)
	{
		// An empty resolved handle means the server task already rejected the
		// request and sent the precise result. Do not emit a second generic verdict.
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	UAbilitySystemComponent* TargetASC = nullptr;
	// The authority task already owns range, aim, obstruction, and hit resolution.
	// Recheck only invariants that can change before this authoritative commit.
	if (bIsAuthority && !IsResolvedTargetStillValid(TargetActor, TargetASC))
	{
		if (SourceASC != nullptr)
		{
			SourceASC->ClientDamageIntentResult(
				ShotId,
				EmultiplayerDamageIntentResult::InvalidTarget);
		}
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	FGameplayEffectSpecHandle DamageSpec;
	if (bIsAuthority)
	{
		DamageSpec = MakeOutgoingGameplayEffectSpec(
			UmultiplayerDamageEffect::StaticClass(),
			GetAbilityLevel());
		if (!DamageSpec.IsValid())
		{
			if (SourceASC != nullptr)
			{
				SourceASC->ClientDamageIntentResult(
					ShotId,
					EmultiplayerDamageIntentResult::CommitFailed);
			}
			EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
			return;
		}
	}
	if (!CommitAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo))
	{
		if (bIsAuthority && SourceASC != nullptr)
		{
			SourceASC->ClientDamageIntentResult(
				ShotId,
				EmultiplayerDamageIntentResult::CommitFailed);
		}
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	BeginAbilityPresentation(MultiplayerGameplayTags::Ability_Damage);
	ExecutePredictedCue(
		MultiplayerGameplayTags::GameplayCue_Coop_Damage_Cast,
		TEXT("Damage"));

	if (bIsAuthority)
	{
		if (const FHitResult* TargetHit = TargetData.Get(0)->GetHitResult())
		{
			// On authority this SingleTargetHit was created after the server trace;
			// the replicated DamageIntent itself contains no HitResult or Actor.
			DamageSpec.Data->GetContext().AddHitResult(*TargetHit, true);
		}
		DamageSpec.Data->SetSetByCallerMagnitude(
			MultiplayerGameplayTags::Data_Damage,
			DamageAmount);
		GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToTarget(
			*DamageSpec.Data.Get(),
			TargetASC);

		const float RemainingHealth = TargetASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute());
		FGameplayEffectSpecHandle VulnerabilitySpec = RemainingHealth > 0.0f
			? MakeOutgoingGameplayEffectSpec(
				UmultiplayerVulnerabilityEffect::StaticClass(),
				GetAbilityLevel())
			: FGameplayEffectSpecHandle();
		if (VulnerabilitySpec.IsValid())
		{
			const FActiveGameplayEffectHandle VulnerabilityHandle =
				GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToTarget(
					*VulnerabilitySpec.Data.Get(),
					TargetASC);
			FGameplayTagContainer VulnerabilityTags;
			VulnerabilityTags.AddTag(MultiplayerGameplayTags::State_Vulnerable);
			const int32 ActiveStacks = TargetASC->GetAggregatedStackCount(
				FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(VulnerabilityTags));
			UE_LOG(
				LogMultiplayerGAS,
				Display,
				TEXT("GAS_VULNERABILITY Target=%s Stacks=%d Effect=%s"),
				*GetNameSafe(TargetActor),
				ActiveStacks,
				*TargetASC->GetActiveGEDebugString(VulnerabilityHandle));
		}
		else
		{
			UE_LOG(
				LogMultiplayerGAS,
				Display,
				TEXT("GAS_VULNERABILITY Target=%s Skipped=TargetDead"),
				*GetNameSafe(TargetActor));
		}

		UE_LOG(
			LogMultiplayerGAS,
			Log,
			TEXT("Server applied %.1f damage: %s -> %s"),
			DamageAmount,
			*GetNameSafe(GetAvatarActorFromActorInfo()),
			*GetNameSafe(TargetActor));
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT Phase=Committed ShotId=%u Spec=%s PredictionKey=%s Target=%s RemainingHealth=%.1f"),
			ShotId,
			*CurrentSpecHandle.ToString(),
			*CurrentActivationInfo.GetActivationPredictionKey().ToString(),
			*GetNameSafe(TargetActor),
			RemainingHealth);
		if (SourceASC != nullptr)
		{
			SourceASC->ClientDamageIntentResult(
				ShotId,
				EmultiplayerDamageIntentResult::Accepted);
		}
	}
	else
	{
		if (SourceASC == nullptr)
		{
			RejectAbilityPresentation(
				MultiplayerGameplayTags::Ability_Damage,
				GetActivePresentationPredictionKey());
			EndAbility(
				CurrentSpecHandle,
				CurrentActorInfo,
				CurrentActivationInfo,
				true,
				true);
			return;
		}

		ClearDamageIntentResultWait();
		PendingDamageIntentShotId = ShotId;
		DamageIntentResultHandle = SourceASC->OnDamageIntentResultReceived().AddUObject(
			this,
			&UmultiplayerDamageAbility::HandleDamageIntentResult);
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				DamageIntentResultTimeoutHandle,
				this,
				&UmultiplayerDamageAbility::HandleDamageIntentResultTimeout,
				5.0f,
				false);
		}
		return;
	}

	CompleteAbilityPresentation();
	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

void UmultiplayerDamageAbility::HandleDamageIntentResult(
	uint32 ShotId,
	EmultiplayerDamageIntentResult Result)
{
	if (ShotId == 0 || ShotId != PendingDamageIntentShotId)
	{
		return;
	}

	const bool bAccepted = Result == EmultiplayerDamageIntentResult::Accepted;
	ClearDamageIntentResultWait();
	if (bAccepted)
	{
		CompleteAbilityPresentation();
	}
	else
	{
		RejectAbilityPresentation(
			MultiplayerGameplayTags::Ability_Damage,
			GetActivePresentationPredictionKey());
	}

	EndAbility(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo,
		true,
		!bAccepted);
}

void UmultiplayerDamageAbility::HandleDamageIntentResultTimeout()
{
	if (PendingDamageIntentShotId == 0)
	{
		return;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Warning,
		TEXT("GAS_M6_INTENT Phase=ClientResultTimeout ShotId=%u Spec=%s Avatar=%s"),
		PendingDamageIntentShotId,
		*CurrentSpecHandle.ToString(),
		*GetNameSafe(GetAvatarActorFromActorInfo()));
	ClearDamageIntentResultWait();
	RejectAbilityPresentation(
		MultiplayerGameplayTags::Ability_Damage,
		GetActivePresentationPredictionKey());
	EndAbility(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo,
		true,
		true);
}

void UmultiplayerDamageAbility::ClearDamageIntentResultWait()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(DamageIntentResultTimeoutHandle);
	}
	if (UmultiplayerAbilitySystemComponent* SourceASC =
		Cast<UmultiplayerAbilitySystemComponent>(
			GetAbilitySystemComponentFromActorInfo()))
	{
		SourceASC->OnDamageIntentResultReceived().Remove(DamageIntentResultHandle);
	}
	DamageIntentResultHandle.Reset();
	PendingDamageIntentShotId = 0;
}

void UmultiplayerDamageAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility,
	bool bWasCancelled)
{
	ClearDamageIntentResultWait();
	Super::EndAbility(
		Handle,
		ActorInfo,
		ActivationInfo,
		bReplicateEndAbility,
		bWasCancelled);
}

bool UmultiplayerDamageAbility::IsResolvedTargetStillValid(
	AActor* TargetActor,
	UAbilitySystemComponent*& OutTargetASC) const
{
	OutTargetASC = nullptr;
	AActor* AvatarActor = GetAvatarActorFromActorInfo();
	if (!IsValid(AvatarActor)
		|| !IsValid(TargetActor)
		|| TargetActor == AvatarActor
		|| TargetActor->IsActorBeingDestroyed())
	{
		return false;
	}

	if (!UmultiplayerTeamLibrary::AreHostile(AvatarActor, TargetActor))
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("Damage target rejected: source=%s target=%s reason=NotHostile"),
			*GetNameSafe(AvatarActor),
			*GetNameSafe(TargetActor));
		return false;
	}

	OutTargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor);
	if (!IsValid(OutTargetASC)
		|| OutTargetASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		OutTargetASC = nullptr;
		return false;
	}

	return true;
}
