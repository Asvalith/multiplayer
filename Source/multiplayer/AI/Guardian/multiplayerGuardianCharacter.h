// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
#include "multiplayerGuardianCharacter.generated.h"

class AAIController;
class AmultiplayerGuardianAIController;
class UGameplayAbility;
class UGameplayEffect;
class UAbilitySystemComponent;
class UmultiplayerAttributeSet;
class UmultiplayerGuardianMeleeAbility;
struct FOnAttributeChangeData;

UENUM(BlueprintType)
enum class EmultiplayerGuardianAIState : uint8
{
	Acquire,
	Chase,
	Leash,
	Windup,
	Attack,
	Cooldown,
	Dead
};

/** Why the replicated Guardian presentation snapshot was refreshed. */
UENUM(BlueprintType)
enum class EmultiplayerGuardianPresentationReason : uint8
{
	PhaseChanged,
	TargetChanged,
	ShieldChanged,
	ChannelingChanged,
	HealthChanged,
	Died,
	RewardGranted
};

/**
 * Replicated presentation clock.  Blueprint animation/VFX may derive remaining
 * phase time from the synchronized server clock, but never owns gameplay time.
 */
USTRUCT(BlueprintType)
struct FmultiplayerGuardianPhaseSnapshot
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|AI")
	EmultiplayerGuardianAIState State = EmultiplayerGuardianAIState::Acquire;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|AI")
	float ServerStartTimeSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|AI")
	float DurationSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|AI")
	int32 Sequence = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|AI")
	TObjectPtr<AActor> Target = nullptr;
};

/** One read-only payload for the future EnemyBP/AnimBP/VFX layer. */
USTRUCT(BlueprintType)
struct FmultiplayerGuardianPresentationEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	EmultiplayerGuardianPresentationReason Reason =
		EmultiplayerGuardianPresentationReason::PhaseChanged;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	EmultiplayerGuardianAIState PreviousState =
		EmultiplayerGuardianAIState::Acquire;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	FmultiplayerGuardianPhaseSnapshot Phase;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	float Health = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	float MaxHealth = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	int32 ActiveChannelers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	int32 RequiredChannelers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	bool bShielded = false;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	bool bDead = false;

	UPROPERTY(BlueprintReadOnly, Category = "Guardian|Presentation")
	bool bRewardGranted = false;
};

DECLARE_MULTICAST_DELEGATE_OneParam(
	FmultiplayerGuardianAuthorityRewardEvent,
	AActor* /* Guardian */);

/**
 * Server-authoritative GAS enemy for the focused two-player co-op loop.
 *
 * C++ owns target validity, AI timing, shield/channeling rules, death cleanup
 * and reward idempotency.  A Blueprint child supplies skeletal mesh, AnimBP,
 * montages, materials, cues and level references through the hooks below.
 */
UCLASS(Blueprintable)
class MULTIPLAYER_API AmultiplayerGuardianCharacter
	: public ACharacter,
	  public IAbilitySystemInterface,
	  public ImultiplayerCoopTeamAgentInterface
{
	GENERATED_BODY()

public:
	AmultiplayerGuardianCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual int32 GetCoopTeamId_Implementation() const override { return CoopTeamId; }
	virtual void GetLifetimeReplicatedProps(
		TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostInitializeComponents() override;

	/** Called by a server-owned overlap/interaction actor when channeling begins. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Guardian|Objective")
	bool RegisterChannelingParticipant(AActor* Participant);

	/** Called by the same server-owned overlap/interaction actor when channeling ends. */
	UFUNCTION(BlueprintCallable, BlueprintAuthorityOnly, Category = "Guardian|Objective")
	bool UnregisterChannelingParticipant(AActor* Participant);

	/**
	 * The only Blueprint presentation hook. It is a read-only projection of
	 * authoritative replicated state and must never advance AI or apply GAS state.
	 */
	UFUNCTION(BlueprintImplementableEvent, BlueprintCosmetic, Category = "Guardian|Presentation", meta = (DisplayName = "On Guardian Presentation"))
	void ReceiveGuardianPresentation(
		const FmultiplayerGuardianPresentationEvent& Event);

	/** Native-only authority hook for a C++ objective/reward subsystem. */
	FmultiplayerGuardianAuthorityRewardEvent& OnAuthorityRewardGranted()
	{
		return AuthorityRewardGrantedEvent;
	}

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Data-only GAS grant authored later as DA_Guardian_AbilitySet. */
	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS")
	TObjectPtr<UmultiplayerAbilitySet> StartupAbilitySet;

	/** Applied after fallback stats; use Override modifiers when replacing them. */
	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS")
	TSubclassOf<UGameplayEffect> InitialStatsEffect;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS")
	TSubclassOf<UGameplayEffect> ShieldEffectClass;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS")
	TSubclassOf<UGameplayEffect> MeleeDamageEffectClass;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS")
	TSubclassOf<UGameplayAbility> AttackAbilityClass;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|GAS", meta = (Categories = "Ability.Guardian"))
	FGameplayTagContainer AttackAbilityTags;

	/** May be replaced by BP_GuardianAIController without changing gameplay code. */
	UPROPERTY(EditDefaultsOnly, Category = "Guardian|AI")
	TSubclassOf<AAIController> GuardianAIControllerClass;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Stats", meta = (ClampMin = "1.0"))
	float InitialMaxHealth = 350.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Stats", meta = (ClampMin = "0.0"))
	float InitialMaxEnergy = 100.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Stats", meta = (ClampMin = "0.0"))
	float InitialAttackPower = 10.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Stats", meta = (ClampMin = "0.0"))
	float InitialArmor = 25.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Stats", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float InitialResistance = 0.1f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Combat", meta = (ClampMin = "0.0"))
	float MeleeDamage = 18.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|AI", meta = (ClampMin = "100.0"))
	float AcquisitionRadius = 1800.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|AI", meta = (ClampMin = "100.0"))
	float LoseTargetRadius = 2400.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|AI", meta = (ClampMin = "100.0"))
	float LeashRadius = 2600.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Combat", meta = (ClampMin = "50.0"))
	float AttackRange = 180.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|AI", meta = (ClampMin = "0.05"))
	float AIDecisionInterval = 0.2f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Combat", meta = (ClampMin = "0.0"))
	float AttackWindupSeconds = 0.6f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Combat", meta = (ClampMin = "0.01"))
	float AttackResolveSeconds = 0.1f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Combat", meta = (ClampMin = "0.0"))
	float AttackCooldownSeconds = 1.2f;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Shield")
	bool bStartsShielded = true;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Objective", meta = (ClampMin = "1"))
	int32 RequiredChannelers = 1;

	/** The portfolio loop keeps one player channeling while the other attacks. */
	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Objective")
	bool bShieldRestoresWhenChannelingStops = true;

	UPROPERTY(EditDefaultsOnly, Category = "Guardian|Objective", meta = (ClampMin = "100.0"))
	float MaximumChannelingDistance = 700.0f;

private:
	friend class AmultiplayerGuardianAIController;
	friend class UmultiplayerGuardianMeleeAbility;

	float GetHealth() const;
	float GetMaxHealth() const;
	EmultiplayerGuardianAIState GetGuardianState() const { return PhaseSnapshot.State; }
	AActor* GetCurrentTarget() const { return PhaseSnapshot.Target; }
	bool IsGuardianShielded() const { return bShielded; }
	bool IsGuardianDead() const { return bIsDead; }
	bool IsGuardianRewardGranted() const { return bRewardGranted; }
	int32 GetActiveChannelerCount() const { return ActiveChannelers.Num(); }
	int32 GetRequiredChannelerCount() const { return RequiredChannelers; }

	void SetSpawnAnchor(const FVector& NewAnchor);
	const FVector& GetSpawnAnchor() const { return SpawnAnchor; }
	void SetAuthoritativeTarget(AActor* NewTarget);
	void SetAuthoritativeState(
		EmultiplayerGuardianAIState NewState,
		float DurationSeconds = 0.0f);
	void BeginAuthoritativeAttack();
	void PruneInvalidChannelers();
	bool IsTargetCandidate(const AActor* Candidate, float MaxDistance) const;
	bool CanCommitMeleeAttack(const AActor* TargetActor) const;
	bool HasClearLineTo(const AActor* TargetActor) const;
	bool TryActivateConfiguredAttackAbility();

	const TArray<TObjectPtr<AActor>>& GetActiveChannelers() const
	{
		return ActiveChannelers;
	}

	float GetAcquisitionRadius() const { return AcquisitionRadius; }
	float GetLoseTargetRadius() const { return LoseTargetRadius; }
	float GetLeashRadius() const { return LeashRadius; }
	float GetAttackRange() const { return AttackRange; }
	float GetAIDecisionInterval() const { return AIDecisionInterval; }
	float GetAttackWindupSeconds() const { return AttackWindupSeconds; }
	float GetAttackResolveSeconds() const { return AttackResolveSeconds; }
	float GetAttackCooldownSeconds() const { return AttackCooldownSeconds; }
	float GetMeleeDamage() const { return MeleeDamage; }
	TSubclassOf<UGameplayEffect> GetMeleeDamageEffectClass() const
	{
		return MeleeDamageEffectClass;
	}

	void InitializeAbilitySystem();
	void ApplyFallbackStats();
	void GrantConfiguredAbilities();
	void ClearChannelingParticipants();
	void SetShieldStateFromObjective(bool bNewShielded);
	void EnterDeathState();
	void GrantAuthoritativeRewardOnce();
	void FinishAttackResolve();
	void FinishAttackCooldown();
	void CompleteAttackCooldown();
	void ClearAuthoritativeTimers();
	void ApplyTeamIdentity();
	void RefreshChannelingState();
	void ApplyGuardianStateTags(
		EmultiplayerGuardianAIState PreviousState,
		EmultiplayerGuardianAIState NewState);
	void ApplyDeathCollisionState();
	void DispatchPresentation(
		EmultiplayerGuardianPresentationReason Reason,
		EmultiplayerGuardianAIState PreviousState =
			EmultiplayerGuardianAIState::Acquire);
	float GetSynchronizedServerTimeSeconds() const;
	bool IsChannelingParticipantValid(const AActor* Participant) const;
	void RemoveChannelerDestroyedBindings();

	void HandleHealthChanged(const FOnAttributeChangeData& ChangeData);
	void HandleShieldEffectRemoved(const FGameplayEffectRemovalInfo& RemovalInfo);

	UFUNCTION()
	void HandleChannelerDestroyed(AActor* DestroyedActor);

	UFUNCTION()
	void OnRep_CoopTeamId();

	UFUNCTION()
	void OnRep_PhaseSnapshot(FmultiplayerGuardianPhaseSnapshot PreviousSnapshot);

	UFUNCTION()
	void OnRep_Shielded();

	UFUNCTION()
	void OnRep_IsDead();

	UFUNCTION()
	void OnRep_RewardGranted();

	UFUNCTION()
	void OnRep_ActiveChannelers();

	UPROPERTY(VisibleAnywhere, Category = "Guardian|GAS", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, Category = "Guardian|GAS", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	UPROPERTY(EditDefaultsOnly, ReplicatedUsing = OnRep_CoopTeamId, Category = "Guardian|Team")
	int32 CoopTeamId = MultiplayerTeams::Enemies;

	UPROPERTY(ReplicatedUsing = OnRep_PhaseSnapshot)
	FmultiplayerGuardianPhaseSnapshot PhaseSnapshot;

	UPROPERTY(ReplicatedUsing = OnRep_Shielded)
	bool bShielded = false;

	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;

	UPROPERTY(ReplicatedUsing = OnRep_RewardGranted)
	bool bRewardGranted = false;

	UPROPERTY(ReplicatedUsing = OnRep_ActiveChannelers)
	TArray<TObjectPtr<AActor>> ActiveChannelers;

	FVector SpawnAnchor = FVector::ZeroVector;
	FmultiplayerAbilitySetGrantedHandles GrantedHandles;
	FActiveGameplayEffectHandle ShieldEffectHandle;
	FTimerHandle AttackResolveTimer;
	FTimerHandle AttackCooldownTimer;
	bool bChannelingTagApplied = false;
	bool bEndingPlay = false;
	FmultiplayerGuardianAuthorityRewardEvent AuthorityRewardGrantedEvent;
};
