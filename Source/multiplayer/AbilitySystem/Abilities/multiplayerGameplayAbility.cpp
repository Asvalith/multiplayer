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
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "multiplayer.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "Team/multiplayerTeamLibrary.h"

UmultiplayerGameplayAbility::UmultiplayerGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationBlockedTags.AddTag(MultiplayerGameplayTags::State_Dead);
}

void UmultiplayerGameplayAbility::EndAbility(
	const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility,
	bool bWasCancelled)
{
	if (bWasCancelled
		&& bPresentationStarted
		&& !bPresentationCompleted
		&& !bPresentationRejected)
	{
		if (PresentationMontage.bStopOnCancelled)
		{
			StopPresentationMontage(PresentationMontage.RejectBlendOutSeconds);
		}
		DispatchAbilityPresentation(
			EmultiplayerAbilityPresentationPhase::Cancelled,
			ActivePresentationTag,
			ActivePresentationPredictionKey);
	}

	Super::EndAbility(
		Handle,
		ActorInfo,
		ActivationInfo,
		bReplicateEndAbility,
		bWasCancelled);
}

void UmultiplayerGameplayAbility::BeginAbilityPresentation(
	const FGameplayTag& AbilityTag)
{
	ActivePresentationTag = AbilityTag;
	bPresentationStarted = true;
	bPresentationCompleted = false;
	bPresentationRejected = false;

	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	const FPredictionKey PredictionKey = SourceASC != nullptr
		? SourceASC->GetPredictionKeyForNewAction()
		: FPredictionKey();
	ActivePresentationPredictionKey = PredictionKey.Current;

	const bool bAuthority = SourceASC != nullptr
		&& SourceASC->IsOwnerActorAuthoritative();
	DispatchAbilityPresentation(
		bAuthority
			? EmultiplayerAbilityPresentationPhase::AuthorityStarted
			: EmultiplayerAbilityPresentationPhase::PredictedStarted,
		AbilityTag,
		ActivePresentationPredictionKey);

	if (SourceASC != nullptr && PresentationMontage.Montage != nullptr)
	{
		SourceASC->PlayMontage(
			this,
			CurrentActivationInfo,
			PresentationMontage.Montage,
			FMath::Max(PresentationMontage.PlayRate, 0.01f),
			PresentationMontage.StartSection);
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_PRESENTATION Phase=%s Ability=%s PredictionKey=%d Montage=%s Avatar=%s"),
		bAuthority ? TEXT("AuthorityStarted") : TEXT("PredictedStarted"),
		*AbilityTag.ToString(),
		static_cast<int32>(ActivePresentationPredictionKey),
		*GetNameSafe(PresentationMontage.Montage),
		*GetNameSafe(GetAvatarActorFromActorInfo()));
}

void UmultiplayerGameplayAbility::CompleteAbilityPresentation()
{
	if (!bPresentationStarted || bPresentationCompleted || bPresentationRejected)
	{
		return;
	}

	bPresentationCompleted = true;
	DispatchAbilityPresentation(
		EmultiplayerAbilityPresentationPhase::Completed,
		ActivePresentationTag,
		ActivePresentationPredictionKey);
}

void UmultiplayerGameplayAbility::RejectAbilityPresentation(
	const FGameplayTag& AbilityTag,
	int16 PredictionKey)
{
	if (PredictionKey != 0 && RejectedPresentationKeys.Contains(PredictionKey))
	{
		return;
	}
	if (PredictionKey != 0)
	{
		RejectedPresentationKeys.Add(PredictionKey);
	}

	const bool bMatchesCurrentPresentation = bPresentationStarted
		&& AbilityTag.MatchesTagExact(ActivePresentationTag)
		&& (PredictionKey == 0
			|| ActivePresentationPredictionKey == 0
			|| PredictionKey == ActivePresentationPredictionKey);
	if (bMatchesCurrentPresentation)
	{
		bPresentationRejected = true;
		if (PresentationMontage.bStopOnRejected)
		{
			StopPresentationMontage(PresentationMontage.RejectBlendOutSeconds);
		}
	}

	DispatchAbilityPresentation(
		EmultiplayerAbilityPresentationPhase::Rejected,
		AbilityTag,
		PredictionKey);
}

void UmultiplayerGameplayAbility::DispatchAbilityPresentation(
	EmultiplayerAbilityPresentationPhase Phase,
	const FGameplayTag& AbilityTag,
	int16 PredictionKey) const
{
	AActor* AvatarActor = GetAvatarActorFromActorInfo();
	if (!IsValid(AvatarActor)
		|| CurrentActorInfo == nullptr
		|| !CurrentActorInfo->IsLocallyControlled()
		|| (AvatarActor->GetWorld() != nullptr
			&& AvatarActor->GetWorld()->GetNetMode() == NM_DedicatedServer)
		|| !AvatarActor->GetClass()->ImplementsInterface(
			UmultiplayerAbilityPresentationInterface::StaticClass()))
	{
		return;
	}

	FmultiplayerAbilityPresentationEvent Event;
	Event.AbilityTag = AbilityTag;
	Event.Phase = Phase;
	Event.PredictionKey = static_cast<int32>(PredictionKey);
	Event.bLocallyControlled = true;
	ImultiplayerAbilityPresentationInterface::Execute_HandleAbilityPresentation(
		AvatarActor,
		Event);
}

void UmultiplayerGameplayAbility::StopPresentationMontage(float BlendOutSeconds)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (SourceASC != nullptr
		&& PresentationMontage.Montage != nullptr
		&& SourceASC->GetCurrentMontage() == PresentationMontage.Montage)
	{
		SourceASC->CurrentMontageStop(FMath::Max(BlendOutSeconds, 0.0f));
	}
}

FGameplayTag UmultiplayerGameplayAbility::ResolveAbilityPresentationTag(
	FName AbilityName)
{
	if (AbilityName == TEXT("Damage"))
	{
		return MultiplayerGameplayTags::Ability_Damage;
	}
	if (AbilityName == TEXT("Heal"))
	{
		return MultiplayerGameplayTags::Ability_Heal;
	}
	if (AbilityName == TEXT("Immunity"))
	{
		return MultiplayerGameplayTags::Ability_Immunity;
	}
	return FGameplayTag();
}

void UmultiplayerGameplayAbility::ExecutePredictedCue(
	const FGameplayTag& CueTag,
	FName AbilityName)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (SourceASC == nullptr || !CueTag.IsValid())
	{
		return;
	}

	TrackPrediction(AbilityName);

	FGameplayCueParameters CueParameters;
	SourceASC->InitDefaultGameplayCueParameters(CueParameters);
	CueParameters.Location = GetAvatarActorFromActorInfo() != nullptr
		? GetAvatarActorFromActorInfo()->GetActorLocation()
		: FVector::ZeroVector;
	CueParameters.AbilityLevel = GetAbilityLevel();
	CueParameters.NormalizedMagnitude = 1.0f;
	CueParameters.RawMagnitude = 1.0f;
	SourceASC->ExecuteGameplayCue(CueTag, CueParameters);

	const FPredictionKey PredictionKey = SourceASC->GetPredictionKeyForNewAction();
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_CUE_EMIT Phase=%s Ability=%s Cue=%s PredictionKey=%s Avatar=%s"),
		SourceASC->IsOwnerActorAuthoritative()
			? TEXT("AuthorityEmit")
			: TEXT("PredictEmit"),
		*AbilityName.ToString(),
		*CueTag.ToString(),
		*PredictionKey.ToString(),
		*GetNameSafe(GetAvatarActorFromActorInfo()));
}

void UmultiplayerGameplayAbility::TrackPrediction(FName AbilityName)
{
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (SourceASC == nullptr)
	{
		return;
	}

	FPredictionKey PredictionKey = SourceASC->GetPredictionKeyForNewAction();
	if (SourceASC->IsOwnerActorAuthoritative() || !PredictionKey.IsLocalClientKey())
	{
		return;
	}

	PredictionKey.NewRejectedDelegate().BindUObject(
		this,
		&UmultiplayerGameplayAbility::HandlePredictionRejected,
		AbilityName,
		CurrentSpecHandle,
		CurrentActivationInfo.GetActivationPredictionKey(),
		PredictionKey);
	PredictionKey.NewCaughtUpDelegate().BindUObject(
		this,
		&UmultiplayerGameplayAbility::HandlePredictionCaughtUp,
		AbilityName,
		CurrentSpecHandle,
		PredictionKey.Current);
}

bool UmultiplayerGameplayAbility::IsPredictionLabEnabled() const
{
#if !UE_BUILD_SHIPPING
	return FParse::Param(FCommandLine::Get(), TEXT("GASM6Lab"));
#else
	return false;
#endif
}

void UmultiplayerGameplayAbility::ApplyPredictionLabPendingEffect()
{
	if (!IsPredictionLabEnabled())
	{
		return;
	}
	UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (SourceASC == nullptr || SourceASC->IsOwnerActorAuthoritative())
	{
		return;
	}

	ApplyGameplayEffectToOwner(
		CurrentSpecHandle,
		CurrentActorInfo,
		CurrentActivationInfo,
		UmultiplayerPredictionPendingEffect::StaticClass()->GetDefaultObject<UGameplayEffect>(),
		GetAbilityLevel());
}

void UmultiplayerGameplayAbility::LogPredictionState(
	const TCHAR* Phase,
	FName AbilityName,
	int16 PredictionKey) const
{
	// M6_STATE is a focused reject-lab probe. Damage and Heal also register
	// prediction delegates, but their costs and persistent tags are different;
	// emitting the Immunity schema for them would create misleading telemetry.
	if (AbilityName != TEXT("Immunity") || !IsPredictionLabEnabled())
	{
		return;
	}

	const UAbilitySystemComponent* SourceASC = GetAbilitySystemComponentFromActorInfo();
	if (SourceASC == nullptr)
	{
		return;
	}

	FGameplayTag CooldownTag;
	if (AbilityName == TEXT("Damage"))
	{
		CooldownTag = MultiplayerGameplayTags::Cooldown_Ability_Damage;
	}
	else if (AbilityName == TEXT("Heal"))
	{
		CooldownTag = MultiplayerGameplayTags::Cooldown_Ability_Heal;
	}
	else if (AbilityName == TEXT("Immunity"))
	{
		CooldownTag = MultiplayerGameplayTags::Cooldown_Ability_Immunity;
	}

	const float Energy = SourceASC->GetNumericAttribute(
		UmultiplayerAttributeSet::GetEnergyAttribute());
	const float EnergyBase = SourceASC->GetNumericAttributeBase(
		UmultiplayerAttributeSet::GetEnergyAttribute());
	auto CountEffectsWithEffectTag = [SourceASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return SourceASC->GetActiveEffectsWithAllTags(Tags).Num();
	};
	auto CountEffectsWithOwningTag = [SourceASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return SourceASC->GetActiveEffects(
			FGameplayEffectQuery::MakeQuery_MatchAllOwningTags(Tags)).Num();
	};
	const UmultiplayerAbilitySystemComponent* ProjectASC =
		Cast<UmultiplayerAbilitySystemComponent>(SourceASC);
	const uint32 TrialId = AbilityName == TEXT("Immunity") && ProjectASC != nullptr
		? ProjectASC->GetPredictionRejectLabTrialId()
		: 0;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_STATE Phase=%s TrialId=%u Ability=%s Spec=%s PredictionKey=%d Role=%s EnergyBase=%.1f EnergyCurrent=%.1f CostGECount=%d CooldownGECount=%d CooldownTagCount=%d PersistentGECount=%d ImmuneCount=%d PendingGECount=%d PendingCueCount=%d"),
		Phase,
		TrialId,
		*AbilityName.ToString(),
		*CurrentSpecHandle.ToString(),
		static_cast<int32>(PredictionKey),
		SourceASC->IsOwnerActorAuthoritative() ? TEXT("Authority") : TEXT("PredictingClient"),
		EnergyBase,
		Energy,
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Cost_Immunity),
		CooldownTag.IsValid() ? CountEffectsWithOwningTag(CooldownTag) : 0,
		CooldownTag.IsValid() ? SourceASC->GetTagCount(CooldownTag) : 0,
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Positive_Immunity),
		SourceASC->GetTagCount(MultiplayerGameplayTags::State_Immune),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Debug_PredictionPending),
		SourceASC->GetTagCount(
			MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending));
}

void UmultiplayerGameplayAbility::HandlePredictionRejected(
	FName AbilityName,
	FGameplayAbilitySpecHandle SpecHandle,
	FPredictionKey ActivationPredictionKey,
	FPredictionKey ActionPredictionKey)
{
	UmultiplayerAbilitySystemComponent* SourceASC =
		Cast<UmultiplayerAbilitySystemComponent>(
			GetAbilitySystemComponentFromActorInfo());
	if (SourceASC != nullptr)
	{
		SourceASC->RecordPredictionLabRejected(
			AbilityName,
			ActionPredictionKey.Current);
	}
	UE_LOG(
		LogMultiplayerGAS,
		Warning,
		TEXT("GAS_PREDICTION Phase=Rejected Ability=%s Spec=%s ActivationKey=%d ActionKey=%d ActionBaseKey=%d Avatar=%s"),
		*AbilityName.ToString(),
		*SpecHandle.ToString(),
		static_cast<int32>(ActivationPredictionKey.Current),
		static_cast<int32>(ActionPredictionKey.Current),
		static_cast<int32>(ActionPredictionKey.Base),
		*GetNameSafe(GetAvatarActorFromActorInfo()));
	RejectAbilityPresentation(
		ResolveAbilityPresentationTag(AbilityName),
		ActionPredictionKey.Current);
	LogPredictionState(
		TEXT("RejectedReconciled"),
		AbilityName,
		ActionPredictionKey.Current);
}

void UmultiplayerGameplayAbility::HandlePredictionCaughtUp(
	FName AbilityName,
	FGameplayAbilitySpecHandle SpecHandle,
	int16 PredictionKey)
{
	bool bReconciledCatchUp = true;
	if (UmultiplayerAbilitySystemComponent* SourceASC =
		Cast<UmultiplayerAbilitySystemComponent>(
			GetAbilitySystemComponentFromActorInfo()))
	{
		bReconciledCatchUp = SourceASC->RecordPredictionLabCaughtUp(
			AbilityName,
			PredictionKey);
	}
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_PREDICTION Phase=%s Ability=%s Spec=%s PredictionKey=%d Avatar=%s"),
		bReconciledCatchUp ? TEXT("CaughtUp") : TEXT("PostRejectCatchUp"),
		*AbilityName.ToString(),
		*SpecHandle.ToString(),
		static_cast<int32>(PredictionKey),
		*GetNameSafe(GetAvatarActorFromActorInfo()));
	const bool bWasRejected = RejectedPresentationKeys.Remove(PredictionKey) > 0;
	if (bReconciledCatchUp && !bWasRejected)
	{
		DispatchAbilityPresentation(
			EmultiplayerAbilityPresentationPhase::Reconciled,
			ResolveAbilityPresentationTag(AbilityName),
			PredictionKey);
	}
	LogPredictionState(
		bReconciledCatchUp
			? TEXT("ReconciledCaughtUp")
			: TEXT("PostRejectCatchUpIgnored"),
		AbilityName,
		PredictionKey);
}
