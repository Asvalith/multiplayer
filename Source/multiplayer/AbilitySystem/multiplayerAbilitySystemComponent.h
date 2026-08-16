// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerGameplayAbilityTargetData.h"
#include "GameplayTagContainer.h"
#include "multiplayerAbilitySystemComponent.generated.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnPredictionLabReconciled,
	bool /* bRejected */,
	int16 /* PredictionKey */);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnDamageIntentResultReceived,
	uint32 /* ShotId */,
	EmultiplayerDamageIntentResult /* Result */);

UCLASS()
class MULTIPLAYER_API UmultiplayerAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	void AbilityInputTagPressed(const FGameplayTag& InputTag);
	void AbilityInputTagReleased(const FGameplayTag& InputTag);
	uint32 AllocateLocalDamageShotId();
	uint32 GetLastAllocatedDamageShotId() const { return NextLocalDamageShotId; }
	EmultiplayerDamageIntentResult TryConsumeDamageIntent(
		uint32 ShotId,
		double ServerNowSeconds);

	UFUNCTION(Client, Reliable)
	void ClientDamageIntentResult(
		uint32 ShotId,
		EmultiplayerDamageIntentResult Result);

	void SetDamageIntentLabMutation(EmultiplayerDamageIntentTestMutation Mutation);
	EmultiplayerDamageIntentTestMutation ConsumeDamageIntentLabMutation();
	uint32 GetDamageIntentResultSerial() const { return DamageIntentResultSerial; }
	uint32 GetLastDamageIntentResultShotId() const { return LastDamageIntentResultShotId; }
	EmultiplayerDamageIntentResult GetLastDamageIntentResult() const
	{
		return LastDamageIntentResult;
	}
	FOnDamageIntentResultReceived& OnDamageIntentResultReceived()
	{
		return DamageIntentResultReceivedEvent;
	}

	/** Arms exactly one server-side Immunity activation failure for the M6 lab. */
	bool ArmNextImmunityPredictionRejection(uint32 TrialId);

	UFUNCTION(Server, Reliable)
	void ServerArmNextImmunityPredictionRejection(uint32 TrialId);

	UFUNCTION(Client, Reliable)
	void ClientConfirmImmunityPredictionRejectionArmed(uint32 TrialId, bool bArmed);

	bool HasPredictionRejectLabArmResult(uint32 TrialId) const
	{
		return LastConfirmedPredictionRejectTrialId == TrialId;
	}
	bool WasPredictionRejectLabArmSuccessful(uint32 TrialId) const
	{
		return HasPredictionRejectLabArmResult(TrialId)
			&& bLastPredictionRejectArmSucceeded;
	}

	void RecordPredictionLabRejected(FName AbilityName, int16 PredictionKey);
	/** Returns false when an engine catch-up arrives for a key already rejected. */
	bool RecordPredictionLabCaughtUp(FName AbilityName, int16 PredictionKey);
	int16 GetLastPredictionLabRejectedKey() const { return LastPredictionLabRejectedKey; }
	int16 GetLastPredictionLabCaughtUpKey() const { return LastPredictionLabCaughtUpKey; }
	FOnPredictionLabReconciled& OnPredictionLabReconciled()
	{
		return PredictionLabReconciledEvent;
	}
	uint32 GetPredictionRejectLabTrialId() const
	{
		return LastPredictionRejectTrialId != 0
			? LastPredictionRejectTrialId
			: LastConfirmedPredictionRejectTrialId;
	}

	virtual void NotifyAbilityFailed(
		const FGameplayAbilitySpecHandle Handle,
		UGameplayAbility* Ability,
		const FGameplayTagContainer& FailureReason) override;

protected:
	virtual void InternalServerTryActivateAbility(
		FGameplayAbilitySpecHandle AbilityToActivate,
		bool bInputPressed,
		const FPredictionKey& PredictionKey,
		const FGameplayEventData* TriggerEventData) override;

private:
	uint32 ArmedPredictionRejectTrialId = 0;
	uint32 LastPredictionRejectTrialId = 0;
	uint32 LastConfirmedPredictionRejectTrialId = 0;
	int16 LastPredictionLabRejectedKey = 0;
	int16 LastPredictionLabCaughtUpKey = 0;
	bool bLastPredictionRejectArmSucceeded = false;
	uint32 NextLocalDamageShotId = 0;
	FmultiplayerDamageIntentGuard DamageIntentGuard;
	EmultiplayerDamageIntentTestMutation PendingDamageIntentLabMutation =
		EmultiplayerDamageIntentTestMutation::None;
	uint32 DamageIntentResultSerial = 0;
	uint32 LastDamageIntentResultShotId = 0;
	EmultiplayerDamageIntentResult LastDamageIntentResult =
		EmultiplayerDamageIntentResult::InvalidSchema;
	FOnDamageIntentResultReceived DamageIntentResultReceivedEvent;
	FOnPredictionLabReconciled PredictionLabReconciledEvent;
};
