// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAbilitySystemComponent.h"

#include "Abilities/GameplayAbility.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "multiplayer.h"

namespace
{
	FPredictionKey GetActivePredictionKey(const FGameplayAbilitySpec& AbilitySpec)
	{
		if (const UGameplayAbility* AbilityInstance = AbilitySpec.GetPrimaryInstance())
		{
			return AbilityInstance->GetCurrentActivationInfo().GetActivationPredictionKey();
		}

		return FPredictionKey();
	}
}

void UmultiplayerAbilitySystemComponent::AbilityInputTagPressed(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	FScopedAbilityListLock AbilityListLock(*this);
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag))
		{
			continue;
		}

		AbilitySpecInputPressed(AbilitySpec);
		if (AbilitySpec.IsActive())
		{
			InvokeReplicatedEvent(
				EAbilityGenericReplicatedEvent::InputPressed,
				AbilitySpec.Handle,
				GetActivePredictionKey(AbilitySpec));
		}
		else
		{
			TryActivateAbility(AbilitySpec.Handle);
		}
	}
}

void UmultiplayerAbilitySystemComponent::AbilityInputTagReleased(const FGameplayTag& InputTag)
{
	if (!InputTag.IsValid())
	{
		return;
	}

	FScopedAbilityListLock AbilityListLock(*this);
	for (FGameplayAbilitySpec& AbilitySpec : GetActivatableAbilities())
	{
		if (!AbilitySpec.GetDynamicSpecSourceTags().HasTagExact(InputTag) || !AbilitySpec.IsActive())
		{
			continue;
		}

		AbilitySpecInputReleased(AbilitySpec);
		InvokeReplicatedEvent(
			EAbilityGenericReplicatedEvent::InputReleased,
			AbilitySpec.Handle,
			GetActivePredictionKey(AbilitySpec));
	}
}

uint32 UmultiplayerAbilitySystemComponent::AllocateLocalDamageShotId()
{
	do
	{
		++NextLocalDamageShotId;
	}
	while (NextLocalDamageShotId == 0);
	return NextLocalDamageShotId;
}

EmultiplayerDamageIntentResult
UmultiplayerAbilitySystemComponent::TryConsumeDamageIntent(
	uint32 ShotId,
	double ServerNowSeconds)
{
	if (!IsOwnerActorAuthoritative() || ShotId == 0)
	{
		return EmultiplayerDamageIntentResult::InvalidShotId;
	}
	return DamageIntentGuard.TryConsume(ShotId, ServerNowSeconds);
}

void UmultiplayerAbilitySystemComponent::ClientDamageIntentResult_Implementation(
	uint32 ShotId,
	EmultiplayerDamageIntentResult Result)
{
	++DamageIntentResultSerial;
	LastDamageIntentResultShotId = ShotId;
	LastDamageIntentResult = Result;
	if (Result == EmultiplayerDamageIntentResult::Accepted)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT Phase=ClientResult ShotId=%u Result=%s Owner=%s"),
			ShotId,
			GetMultiplayerDamageIntentResultName(Result),
			*GetNameSafe(GetOwnerActor()));
	}
	else
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M6_INTENT Phase=ClientResult ShotId=%u Result=%s Owner=%s"),
			ShotId,
			GetMultiplayerDamageIntentResultName(Result),
			*GetNameSafe(GetOwnerActor()));
	}
}

void UmultiplayerAbilitySystemComponent::SetDamageIntentLabMutation(
	EmultiplayerDamageIntentTestMutation Mutation)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (FParse::Param(FCommandLine::Get(), TEXT("GASM6IntentLab")))
	{
		PendingDamageIntentLabMutation = Mutation;
	}
#endif
}

EmultiplayerDamageIntentTestMutation
UmultiplayerAbilitySystemComponent::ConsumeDamageIntentLabMutation()
{
	EmultiplayerDamageIntentTestMutation Mutation =
		EmultiplayerDamageIntentTestMutation::None;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (FParse::Param(FCommandLine::Get(), TEXT("GASM6IntentLab")))
	{
		Mutation = PendingDamageIntentLabMutation;
		PendingDamageIntentLabMutation =
			EmultiplayerDamageIntentTestMutation::None;
	}
#endif
	return Mutation;
}

bool UmultiplayerAbilitySystemComponent::ArmNextImmunityPredictionRejection(uint32 TrialId)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (!IsOwnerActorAuthoritative()
		|| !FParse::Param(FCommandLine::Get(), TEXT("GASM6Lab"))
		|| TrialId == 0)
	{
		return false;
	}

	ArmedPredictionRejectTrialId = TrialId;
	LastPredictionRejectTrialId = 0;
	SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Debug_ForceReject_Immunity,
		1);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_REJECT Phase=AuthorityArmed TrialId=%u Ability=Immunity Owner=%s Tag=%s"),
		TrialId,
		*GetNameSafe(GetOwnerActor()),
		*MultiplayerGameplayTags::Debug_ForceReject_Immunity.GetTag().ToString());
	return true;
#else
	return false;
#endif
}

void UmultiplayerAbilitySystemComponent::ServerArmNextImmunityPredictionRejection_Implementation(
	uint32 TrialId)
{
	const bool bArmed = ArmNextImmunityPredictionRejection(TrialId);
	if (bArmed)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_REJECT Phase=ArmResult TrialId=%u Ability=Immunity Owner=%s Armed=true"),
			TrialId,
			*GetNameSafe(GetOwnerActor()));
	}
	else
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M6_REJECT Phase=ArmResult TrialId=%u Ability=Immunity Owner=%s Armed=false"),
			TrialId,
			*GetNameSafe(GetOwnerActor()));
	}
	ClientConfirmImmunityPredictionRejectionArmed(TrialId, bArmed);
}

void UmultiplayerAbilitySystemComponent::ClientConfirmImmunityPredictionRejectionArmed_Implementation(
	uint32 TrialId,
	bool bArmed)
{
	LastConfirmedPredictionRejectTrialId = TrialId;
	bLastPredictionRejectArmSucceeded = bArmed;
	LastPredictionLabRejectedKey = 0;
	LastPredictionLabCaughtUpKey = 0;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_REJECT Phase=ClientArmConfirmed TrialId=%u Ability=Immunity Armed=%s"),
		TrialId,
		bArmed ? TEXT("true") : TEXT("false"));
}

void UmultiplayerAbilitySystemComponent::RecordPredictionLabRejected(
	FName AbilityName,
	int16 PredictionKey)
{
	if (AbilityName == TEXT("Immunity"))
	{
		LastPredictionLabRejectedKey = PredictionKey;
	}
}

bool UmultiplayerAbilitySystemComponent::RecordPredictionLabCaughtUp(
	FName AbilityName,
	int16 PredictionKey)
{
	if (AbilityName != TEXT("Immunity"))
	{
		return true;
	}

	// ClientActivateAbilityFailed broadcasts the rejection delegates, but the
	// engine may later catch up the same key as its replicated key map advances.
	// That second notification is reconciliation bookkeeping, not acceptance.
	if (PredictionKey == LastPredictionLabRejectedKey)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_RECONCILE Phase=RejectedKeyCatchUpIgnored TrialId=%u Ability=Immunity PredictionKey=%d"),
			GetPredictionRejectLabTrialId(),
			static_cast<int32>(PredictionKey));
		return false;
	}

	LastPredictionLabCaughtUpKey = PredictionKey;
	return true;
}

void UmultiplayerAbilitySystemComponent::InternalServerTryActivateAbility(
	FGameplayAbilitySpecHandle AbilityToActivate,
	bool bInputPressed,
	const FPredictionKey& PredictionKey,
	const FGameplayEventData* TriggerEventData)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const FGameplayAbilitySpec* AbilitySpec = FindAbilitySpecFromHandle(AbilityToActivate);
	const bool bIsRemotePredictedImmunity =
		PredictionKey.IsLocalClientKey()
		&& PredictionKey.WasReceived()
		&& AbilitySpec != nullptr
		&& AbilitySpec->Ability != nullptr
		&& AbilitySpec->Ability->GetAssetTags().HasTagExact(
			MultiplayerGameplayTags::Ability_Immunity);
	if (FParse::Param(FCommandLine::Get(), TEXT("GASM6Lab"))
		&& bIsRemotePredictedImmunity
		&& HasMatchingGameplayTag(
			MultiplayerGameplayTags::Debug_ForceReject_Immunity))
	{
		// Mirror the engine's non-Shipping DenyClientActivations test path: reject
		// before Super creates a server prediction window, so no acceptance ack can race.
		SetLooseGameplayTagCount(
			MultiplayerGameplayTags::Debug_ForceReject_Immunity,
			0);
		const uint32 TrialId = ArmedPredictionRejectTrialId;
		ArmedPredictionRejectTrialId = 0;
		LastPredictionRejectTrialId = TrialId;
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M6_REJECT Phase=AuthorityRejected TrialId=%u Ability=Immunity Reason=ForcedNextActivation Spec=%s PredictionKey=%s PredictionKeyCurrent=%d TagRemaining=%d"),
			TrialId,
			*AbilityToActivate.ToString(),
			*PredictionKey.ToString(),
			static_cast<int32>(PredictionKey.Current),
			GetTagCount(
				MultiplayerGameplayTags::Debug_ForceReject_Immunity));
		ClientActivateAbilityFailed(
			AbilityToActivate,
			PredictionKey.Current);
		return;
	}
#endif

	Super::InternalServerTryActivateAbility(
		AbilityToActivate,
		bInputPressed,
		PredictionKey,
		TriggerEventData);
}

void UmultiplayerAbilitySystemComponent::NotifyAbilityFailed(
	const FGameplayAbilitySpecHandle Handle,
	UGameplayAbility* Ability,
	const FGameplayTagContainer& FailureReason)
{
	Super::NotifyAbilityFailed(Handle, Ability, FailureReason);
	UE_LOG(
		LogMultiplayerGAS,
		Verbose,
		TEXT("GAS_ABILITY_FAILED Spec=%s Ability=%s FailureTags=%s Authority=%s"),
		*Handle.ToString(),
		*GetNameSafe(Ability),
		*FailureReason.ToStringSimple(),
		IsOwnerActorAuthoritative() ? TEXT("true") : TEXT("false"));
}
