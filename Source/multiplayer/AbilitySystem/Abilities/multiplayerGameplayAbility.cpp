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
#include "multiplayerCharacter.h"
#include "Team/multiplayerTeamLibrary.h"

UmultiplayerGameplayAbility::UmultiplayerGameplayAbility()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	ActivationBlockedTags.AddTag(MultiplayerGameplayTags::State_Dead);
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
	if (AbilityName == TEXT("Immunity"))
	{
		if (AmultiplayerCharacter* Character =
			Cast<AmultiplayerCharacter>(GetAvatarActorFromActorInfo()))
		{
			Character->ReconcilePredictionLabPendingPresentation(
				TEXT("Rejected"),
				ActionPredictionKey.Current);
		}
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
	if (bReconciledCatchUp && AbilityName == TEXT("Immunity"))
	{
		if (AmultiplayerCharacter* Character =
			Cast<AmultiplayerCharacter>(GetAvatarActorFromActorInfo()))
		{
			Character->ReconcilePredictionLabPendingPresentation(
				TEXT("CaughtUp"),
				PredictionKey);
		}
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
	LogPredictionState(
		bReconciledCatchUp
			? TEXT("ReconciledCaughtUp")
			: TEXT("PostRejectCatchUpIgnored"),
		AbilityName,
		PredictionKey);
}

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
	const bool bTargetIsValid = bIsAuthority
		? IsServerTargetValid(TargetActor)
		: ShotId != 0;
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
	if (!bTargetIsValid)
	{
		if (bIsAuthority && SourceASC != nullptr)
		{
			SourceASC->ClientDamageIntentResult(
				ShotId,
				EmultiplayerDamageIntentResult::InvalidTarget);
		}
		EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, true);
		return;
	}

	UAbilitySystemComponent* TargetASC = nullptr;
	FGameplayEffectSpecHandle DamageSpec;
	if (bIsAuthority)
	{
		TargetASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor);
		if (TargetASC == nullptr)
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

	EndAbility(CurrentSpecHandle, CurrentActorInfo, CurrentActivationInfo, true, false);
}

bool UmultiplayerDamageAbility::IsServerTargetValid(AActor* TargetActor) const
{
	AActor* AvatarActor = GetAvatarActorFromActorInfo();
	UWorld* World = AvatarActor != nullptr ? AvatarActor->GetWorld() : nullptr;
	if (World == nullptr
		|| TargetActor == nullptr
		|| TargetActor == AvatarActor
		|| TargetActor->IsActorBeingDestroyed())
	{
		return false;
	}

	if (FVector::DistSquared(AvatarActor->GetActorLocation(), TargetActor->GetActorLocation())
		> FMath::Square(TargetRange + 50.0f))
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

	UAbilitySystemComponent* TargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(TargetActor);
	if (TargetASC == nullptr
		|| !TargetASC->HasMatchingGameplayTag(MultiplayerGameplayTags::Team_Enemy)
		|| TargetASC->HasMatchingGameplayTag(MultiplayerGameplayTags::Team_Player)
		|| TargetASC->GetNumericAttribute(UmultiplayerAttributeSet::GetHealthAttribute()) <= 0.0f)
	{
		return false;
	}

	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GASDamageTargetValidation), false, AvatarActor);
	FHitResult HitResult;
	const bool bBlocked = World->LineTraceSingleByChannel(
		HitResult,
		AvatarActor->GetActorLocation(),
		TargetActor->GetActorLocation(),
		ECC_Visibility,
		QueryParams);

	return !bBlocked || HitResult.GetActor() == TargetActor;
}

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
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	ExecutePredictedCue(
		MultiplayerGameplayTags::GameplayCue_Coop_Heal_Cast,
		TEXT("Heal"));

	if (ActorInfo != nullptr && ActorInfo->IsNetAuthority())
	{
		FGameplayEffectSpecHandle HealSpec = MakeOutgoingGameplayEffectSpec(
			UmultiplayerHealingEffect::StaticClass(),
			GetAbilityLevel());
		if (HealSpec.IsValid())
		{
			HealSpec.Data->SetSetByCallerMagnitude(
				MultiplayerGameplayTags::Data_Heal,
				HealingAmount);
			GetAbilitySystemComponentFromActorInfo()->ApplyGameplayEffectSpecToSelf(
				*HealSpec.Data.Get());
			UE_LOG(
				LogMultiplayerGAS,
				Log,
				TEXT("Server applied %.1f healing to %s"),
				HealingAmount,
				*GetNameSafe(GetAvatarActorFromActorInfo()));
		}
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

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

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}
