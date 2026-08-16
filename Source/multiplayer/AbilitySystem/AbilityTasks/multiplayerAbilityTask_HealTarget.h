// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "multiplayerAbilityTask_HealTarget.generated.h"

enum class EmultiplayerHealTargetResult : uint8
{
	Accepted,
	InvalidSchema,
	NoTarget,
	SourceDead,
	SelfTargetDisabled,
	UnknownTeam,
	NotFriendly,
	MissingAbilitySystem,
	TargetDead,
	OutOfRange,
	Obstructed,
	TargetDataTimeout,
	/** Owning-client summary; the detailed authority reason remains server-only. */
	AuthorityRejected
};

MULTIPLAYER_API const TCHAR* GetMultiplayerHealTargetResultName(
	EmultiplayerHealTargetResult Result);

/**
 * Accepts exactly one ActorArray entry. The client supplies only a candidate Actor;
 * gameplay legality is resolved separately on authority.
 */
MULTIPLAYER_API EmultiplayerHealTargetResult ValidateMultiplayerHealTargetDataSchema(
	const FGameplayAbilityTargetDataHandle& TargetData,
	AActor*& OutCandidate);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerHealTargetReadyEvent,
	const FGameplayAbilityTargetDataHandle&,
	EmultiplayerHealTargetResult);

DECLARE_MULTICAST_DELEGATE_OneParam(
	FmultiplayerHealAuthorityResultEvent,
	bool);

/**
 * One-shot ally targeting task for the locally predicted Heal ability.
 *
 * The owning client picks a friendly actor under the crosshair, or falls back to
 * self when configured, and sends only that actor reference under the activation
 * PredictionKey. Authority then validates source/target life state, team identity,
 * range, line of sight and target ASC before broadcasting resolved TargetData.
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerAbilityTask_HealTarget : public UAbilityTask
{
	GENERATED_BODY()

public:
	static UmultiplayerAbilityTask_HealTarget* CreateHealTargetTask(
		UGameplayAbility* OwningAbility,
		float MaxRange,
		float SweepRadius,
		bool bAllowSelf,
		bool bFallbackToSelf,
		bool bRequireLineOfSight);

	virtual void Activate() override;
	virtual void OnDestroy(bool bInOwnerFinished) override;
	/** Called synchronously by the authority ability after Commit succeeds/fails. */
	void SendAuthorityResultToOwner(bool bAccepted);

	FmultiplayerHealTargetReadyEvent TargetReady;

	FmultiplayerHealAuthorityResultEvent AuthorityResult;

private:
	void SendLocalTargetData();
	void OnTargetDataReplicated(
		const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ActivationTag);
	void HandleRemoteTargetDataTimeout();
	void HandleAuthorityAccepted();
	void HandleAuthorityRejected();
	void HandleAuthorityResultTimeout();
	void ProcessAuthorityTarget(
		const FGameplayAbilityTargetDataHandle& TargetData,
		FGameplayTag ActivationTag,
		bool bConsumeReplicatedData);
	EmultiplayerHealTargetResult ValidateAuthorityCandidate(AActor* Candidate) const;
	AActor* FindLocalCandidate() const;
	bool HasLineOfSight(AActor* SourceActor, AActor* TargetActor) const;
	FGameplayAbilityTargetDataHandle MakeActorTargetData(AActor* TargetActor) const;
	void BroadcastResult(
		const FGameplayAbilityTargetDataHandle& TargetData,
		EmultiplayerHealTargetResult Result);
	void LogAuthorityResult(
		AActor* Candidate,
		EmultiplayerHealTargetResult Result) const;

	float TargetRange = 0.0f;
	float TargetSweepRadius = 0.0f;
	bool bAllowSelfTarget = true;
	bool bUseSelfFallback = true;
	bool bCheckLineOfSight = true;
	FTimerHandle RemoteTargetDataTimeoutHandle;
	FTimerHandle AuthorityResultTimeoutHandle;
};
