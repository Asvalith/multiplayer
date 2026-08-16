// Copyright Epic Games, Inc. All Rights Reserved.

#include "AI/Guardian/multiplayerGuardianCharacter.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AI/Guardian/multiplayerGuardianAIController.h"
#include "AI/Guardian/multiplayerGuardianGameplayAbility.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/GameStateBase.h"
#include "GameplayAbilitySpec.h"
#include "Net/UnrealNetwork.h"
#include "Team/multiplayerTeamLibrary.h"
#include "TimerManager.h"
#include "multiplayer.h"

AmultiplayerGuardianCharacter::AmultiplayerGuardianCharacter()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(20.0f);
	SetMinNetUpdateFrequency(5.0f);

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(
		TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Full);

	AttributeSet = CreateDefaultSubobject<UmultiplayerAttributeSet>(TEXT("AttributeSet"));

	GuardianAIControllerClass = AmultiplayerGuardianAIController::StaticClass();
	AIControllerClass = GuardianAIControllerClass;
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

	AttackAbilityClass = UmultiplayerGuardianMeleeAbility::StaticClass();
	ShieldEffectClass = UmultiplayerGuardianShieldEffect::StaticClass();
	MeleeDamageEffectClass = UmultiplayerDamageEffect::StaticClass();
	AttackAbilityTags.AddTag(MultiplayerGameplayTags::Ability_Guardian_Melee);
}

void AmultiplayerGuardianCharacter::PostInitializeComponents()
{
	// APawn may spawn its default controller during component initialization, so
	// apply the Blueprint class choice before the parent implementation runs.
	if (GuardianAIControllerClass != nullptr)
	{
		AIControllerClass = GuardianAIControllerClass;
	}
	Super::PostInitializeComponents();
}

void AmultiplayerGuardianCharacter::BeginPlay()
{
	Super::BeginPlay();

	SpawnAnchor = GetActorLocation();
	InitializeAbilitySystem();
	ApplyTeamIdentity();

	if (AbilitySystemComponent != nullptr)
	{
		AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerAttributeSet::GetHealthAttribute()).AddUObject(
			this,
			&AmultiplayerGuardianCharacter::HandleHealthChanged);
	}

	if (HasAuthority())
	{
		ApplyFallbackStats();
		GrantConfiguredAbilities();
		SetShieldStateFromObjective(bStartsShielded);
		SetAuthoritativeState(EmultiplayerGuardianAIState::Acquire);

		if (GetHealth() <= 0.0f)
		{
			EnterDeathState();
		}
		ForceNetUpdate();
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_READY Guardian=%s Role=%s Health=%.1f Shielded=%s Team=%d Controller=%s"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		GetHealth(),
		bShielded ? TEXT("true") : TEXT("false"),
		CoopTeamId,
		*GetNameSafe(GetController()));
}

void AmultiplayerGuardianCharacter::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	ClearAuthoritativeTimers();
	RemoveChannelerDestroyedBindings();
	ActiveChannelers.Reset();
	AuthorityRewardGrantedEvent.Clear();

	if (AbilitySystemComponent != nullptr)
	{
		AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
			UmultiplayerAttributeSet::GetHealthAttribute()).RemoveAll(this);
		AbilitySystemComponent->CancelAllAbilities();

		if (HasAuthority())
		{
			if (ShieldEffectHandle.IsValid())
			{
				if (FOnActiveGameplayEffectRemoved_Info* RemovedDelegate =
					AbilitySystemComponent->OnGameplayEffectRemoved_InfoDelegate(
						ShieldEffectHandle))
				{
					RemovedDelegate->RemoveAll(this);
				}
				AbilitySystemComponent->RemoveActiveGameplayEffect(ShieldEffectHandle);
				ShieldEffectHandle.Invalidate();
			}
			GrantedHandles.TakeFromAbilitySystem(AbilitySystemComponent);
		}
	}

	Super::EndPlay(EndPlayReason);
}

UAbilitySystemComponent* AmultiplayerGuardianCharacter::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

float AmultiplayerGuardianCharacter::GetHealth() const
{
	return AttributeSet != nullptr ? AttributeSet->GetHealth() : 0.0f;
}

float AmultiplayerGuardianCharacter::GetMaxHealth() const
{
	return AttributeSet != nullptr ? AttributeSet->GetMaxHealth() : 0.0f;
}

void AmultiplayerGuardianCharacter::InitializeAbilitySystem()
{
	if (AbilitySystemComponent != nullptr)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}
}

void AmultiplayerGuardianCharacter::ApplyFallbackStats()
{
	if (!HasAuthority() || AbilitySystemComponent == nullptr || AttributeSet == nullptr)
	{
		return;
	}

	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetMaxHealthAttribute(),
		FMath::Max(InitialMaxHealth, 1.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetHealthAttribute(),
		FMath::Max(InitialMaxHealth, 1.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetMaxEnergyAttribute(),
		FMath::Max(InitialMaxEnergy, 1.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetEnergyAttribute(),
		FMath::Max(InitialMaxEnergy, 1.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetAttackPowerAttribute(),
		FMath::Max(InitialAttackPower, 0.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetArmorAttribute(),
		FMath::Max(InitialArmor, 0.0f));
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetResistanceAttribute(),
		FMath::Clamp(InitialResistance, 0.0f, 0.8f));

	if (InitialStatsEffect != nullptr)
	{
		const FGameplayEffectContextHandle Context =
			AbilitySystemComponent->MakeEffectContext();
		const FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(
			InitialStatsEffect,
			1.0f,
			Context);
		if (Spec.IsValid())
		{
			const FActiveGameplayEffectHandle Handle =
				AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
			if (Handle.IsValid())
			{
				GrantedHandles.GameplayEffectHandles.Add(Handle);
			}
		}
	}
}

void AmultiplayerGuardianCharacter::GrantConfiguredAbilities()
{
	if (!HasAuthority() || AbilitySystemComponent == nullptr)
	{
		return;
	}

	if (StartupAbilitySet != nullptr)
	{
		StartupAbilitySet->GiveToAbilitySystem(
			AbilitySystemComponent,
			&GrantedHandles,
			this);
	}
	else
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GUARDIAN_CONFIG Guardian=%s Missing=StartupAbilitySet Using=C++AttackFallback"),
			*GetName());
	}

	// The C++ default makes an asset-free server build functional.  When a BP
	// attack class is assigned, the same path grants that class exactly once.
	if (AttackAbilityClass != nullptr
		&& AbilitySystemComponent->FindAbilitySpecFromClass(AttackAbilityClass) == nullptr)
	{
		const FGameplayAbilitySpecHandle Handle = AbilitySystemComponent->GiveAbility(
			FGameplayAbilitySpec(AttackAbilityClass, 1, INDEX_NONE, this));
		if (Handle.IsValid())
		{
			GrantedHandles.AbilitySpecHandles.Add(Handle);
		}
	}
}

void AmultiplayerGuardianCharacter::SetSpawnAnchor(const FVector& NewAnchor)
{
	if (HasAuthority())
	{
		SpawnAnchor = NewAnchor;
	}
}

void AmultiplayerGuardianCharacter::SetAuthoritativeTarget(AActor* NewTarget)
{
	if (!HasAuthority()
		|| (bIsDead && NewTarget != nullptr)
		|| PhaseSnapshot.Target == NewTarget)
	{
		return;
	}

	AActor* PreviousTarget = PhaseSnapshot.Target;
	PhaseSnapshot.Target = NewTarget;
	ForceNetUpdate();
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::TargetChanged,
		PhaseSnapshot.State);

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_TARGET Guardian=%s Previous=%s New=%s"),
		*GetName(),
		*GetNameSafe(PreviousTarget),
		*GetNameSafe(NewTarget));
}

void AmultiplayerGuardianCharacter::SetAuthoritativeState(
	EmultiplayerGuardianAIState NewState,
	float DurationSeconds)
{
	if (!HasAuthority()
		|| AbilitySystemComponent == nullptr
		|| (bIsDead && NewState != EmultiplayerGuardianAIState::Dead)
		|| (PhaseSnapshot.Sequence > 0 && PhaseSnapshot.State == NewState))
	{
		return;
	}

	const EmultiplayerGuardianAIState PreviousState = PhaseSnapshot.State;
	ApplyGuardianStateTags(PreviousState, NewState);
	PhaseSnapshot.State = NewState;
	PhaseSnapshot.ServerStartTimeSeconds = GetSynchronizedServerTimeSeconds();
	PhaseSnapshot.DurationSeconds = FMath::Max(DurationSeconds, 0.0f);
	PhaseSnapshot.Sequence = PhaseSnapshot.Sequence == MAX_int32
		? 1
		: PhaseSnapshot.Sequence + 1;
	if (NewState == EmultiplayerGuardianAIState::Windup)
	{
		AbilitySystemComponent->ExecuteGameplayCue(
			MultiplayerGameplayTags::GameplayCue_Coop_Guardian_Telegraph);
	}
	ForceNetUpdate();
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::PhaseChanged,
		PreviousState);

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_PHASE Guardian=%s Previous=%s New=%s Sequence=%d Start=%.3f Duration=%.3f Target=%s"),
		*GetName(),
		*UEnum::GetValueAsString(PreviousState),
		*UEnum::GetValueAsString(NewState),
		PhaseSnapshot.Sequence,
		PhaseSnapshot.ServerStartTimeSeconds,
		PhaseSnapshot.DurationSeconds,
		*GetNameSafe(PhaseSnapshot.Target));
}

void AmultiplayerGuardianCharacter::BeginAuthoritativeAttack()
{
	if (!HasAuthority()
		|| bIsDead
		|| PhaseSnapshot.State == EmultiplayerGuardianAIState::Windup
		|| PhaseSnapshot.State == EmultiplayerGuardianAIState::Attack
		|| PhaseSnapshot.State == EmultiplayerGuardianAIState::Cooldown
		|| !IsTargetCandidate(PhaseSnapshot.Target, AttackRange)
		|| !HasClearLineTo(PhaseSnapshot.Target))
	{
		return;
	}

	SetAuthoritativeState(
		EmultiplayerGuardianAIState::Windup,
		AttackWindupSeconds);
	if (AttackWindupSeconds <= KINDA_SMALL_NUMBER)
	{
		FinishAttackResolve();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			AttackResolveTimer,
			this,
			&AmultiplayerGuardianCharacter::FinishAttackResolve,
			AttackWindupSeconds,
			false);
	}
}

void AmultiplayerGuardianCharacter::FinishAttackResolve()
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	SetAuthoritativeState(
		EmultiplayerGuardianAIState::Attack,
		AttackResolveSeconds);
	const bool bActivated = TryActivateConfiguredAttackAbility();
	if (!bActivated)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GUARDIAN_ATTACK_REJECT Guardian=%s Target=%s Reason=AbilityActivationFailed"),
			*GetName(),
			*GetNameSafe(PhaseSnapshot.Target));
	}

	GetWorldTimerManager().SetTimer(
		AttackResolveTimer,
		this,
		&AmultiplayerGuardianCharacter::FinishAttackCooldown,
		FMath::Max(AttackResolveSeconds, 0.01f),
		false);
}

void AmultiplayerGuardianCharacter::FinishAttackCooldown()
{
	if (!HasAuthority() || bIsDead)
	{
		return;
	}

	SetAuthoritativeState(
		EmultiplayerGuardianAIState::Cooldown,
		AttackCooldownSeconds);
	if (AttackCooldownSeconds <= KINDA_SMALL_NUMBER)
	{
		CompleteAttackCooldown();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			AttackCooldownTimer,
			this,
			&AmultiplayerGuardianCharacter::CompleteAttackCooldown,
			AttackCooldownSeconds,
			false);
	}
}

void AmultiplayerGuardianCharacter::CompleteAttackCooldown()
{
	if (HasAuthority() && !bIsDead)
	{
		SetAuthoritativeState(EmultiplayerGuardianAIState::Acquire);
	}
}

bool AmultiplayerGuardianCharacter::TryActivateConfiguredAttackAbility()
{
	if (!HasAuthority() || bIsDead || AbilitySystemComponent == nullptr)
	{
		return false;
	}

	if (AttackAbilityClass != nullptr)
	{
		return AbilitySystemComponent->TryActivateAbilityByClass(AttackAbilityClass);
	}
	return !AttackAbilityTags.IsEmpty()
		&& AbilitySystemComponent->TryActivateAbilitiesByTag(AttackAbilityTags);
}

bool AmultiplayerGuardianCharacter::IsTargetCandidate(
	const AActor* Candidate,
	float MaxDistance) const
{
	if (!IsValid(Candidate)
		|| Candidate == this
		|| Candidate->IsActorBeingDestroyed()
		|| !UmultiplayerTeamLibrary::AreHostile(this, Candidate))
	{
		return false;
	}

	if (MaxDistance > 0.0f
		&& FVector::DistSquared(GetActorLocation(), Candidate->GetActorLocation())
			> FMath::Square(MaxDistance))
	{
		return false;
	}

	const UAbilitySystemComponent* TargetASC =
		UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(
			const_cast<AActor*>(Candidate));
	return TargetASC != nullptr
		&& !TargetASC->HasMatchingGameplayTag(MultiplayerGameplayTags::State_Dead)
		&& TargetASC->GetNumericAttribute(
			UmultiplayerAttributeSet::GetHealthAttribute()) > 0.0f;
}

bool AmultiplayerGuardianCharacter::CanCommitMeleeAttack(
	const AActor* TargetActor) const
{
	return HasAuthority()
		&& !bIsDead
		&& PhaseSnapshot.State == EmultiplayerGuardianAIState::Attack
		&& IsTargetCandidate(TargetActor, AttackRange)
		&& HasClearLineTo(TargetActor);
}

bool AmultiplayerGuardianCharacter::HasClearLineTo(
	const AActor* TargetActor) const
{
	const UWorld* World = GetWorld();
	if (World == nullptr || !IsValid(TargetActor))
	{
		return false;
	}

	const FVector Start = GetPawnViewLocation();
	const FVector End = TargetActor->GetActorLocation();
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GuardianLineOfSight), false, this);
	QueryParams.AddIgnoredActor(this);
	FHitResult Hit;
	const bool bBlockingHit = World->LineTraceSingleByChannel(
		Hit,
		Start,
		End,
		ECC_Visibility,
		QueryParams);
	return !bBlockingHit || Hit.GetActor() == TargetActor;
}

bool AmultiplayerGuardianCharacter::RegisterChannelingParticipant(
	AActor* Participant)
{
	if (!HasAuthority()
		|| bIsDead
		|| !IsChannelingParticipantValid(Participant)
		|| ActiveChannelers.Contains(Participant))
	{
		return false;
	}

	ActiveChannelers.Add(Participant);
	Participant->OnDestroyed.AddUniqueDynamic(
		this,
		&AmultiplayerGuardianCharacter::HandleChannelerDestroyed);
	RefreshChannelingState();
	ForceNetUpdate();
	return true;
}

bool AmultiplayerGuardianCharacter::UnregisterChannelingParticipant(
	AActor* Participant)
{
	if (!HasAuthority() || Participant == nullptr)
	{
		return false;
	}

	const int32 Removed = ActiveChannelers.Remove(Participant);
	Participant->OnDestroyed.RemoveDynamic(
		this,
		&AmultiplayerGuardianCharacter::HandleChannelerDestroyed);
	if (Removed <= 0)
	{
		return false;
	}

	RefreshChannelingState();
	ForceNetUpdate();
	return true;
}

void AmultiplayerGuardianCharacter::ClearChannelingParticipants()
{
	if (!HasAuthority())
	{
		return;
	}

	RemoveChannelerDestroyedBindings();
	const bool bHadChannelers = !ActiveChannelers.IsEmpty();
	ActiveChannelers.Reset();
	RefreshChannelingState();
	if (bHadChannelers)
	{
		ForceNetUpdate();
	}
}

void AmultiplayerGuardianCharacter::PruneInvalidChannelers()
{
	if (!HasAuthority())
	{
		return;
	}

	bool bRemovedAny = false;
	for (int32 Index = ActiveChannelers.Num() - 1; Index >= 0; --Index)
	{
		AActor* Participant = ActiveChannelers[Index];
		if (!IsChannelingParticipantValid(Participant))
		{
			if (IsValid(Participant))
			{
				Participant->OnDestroyed.RemoveDynamic(
					this,
					&AmultiplayerGuardianCharacter::HandleChannelerDestroyed);
			}
			ActiveChannelers.RemoveAtSwap(Index);
			bRemovedAny = true;
		}
	}

	if (bRemovedAny)
	{
		RefreshChannelingState();
		ForceNetUpdate();
	}
}

bool AmultiplayerGuardianCharacter::IsChannelingParticipantValid(
	const AActor* Participant) const
{
	return IsTargetCandidate(Participant, MaximumChannelingDistance)
		&& HasClearLineTo(Participant);
}

void AmultiplayerGuardianCharacter::RefreshChannelingState()
{
	if (!HasAuthority() || AbilitySystemComponent == nullptr)
	{
		return;
	}

	const bool bHasChannelers = !ActiveChannelers.IsEmpty();
	if (bHasChannelers != bChannelingTagApplied)
	{
		if (bHasChannelers)
		{
			AbilitySystemComponent->AddReplicatedLooseGameplayTag(
				MultiplayerGameplayTags::State_Guardian_Channeling);
		}
		else
		{
			AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(
				MultiplayerGameplayTags::State_Guardian_Channeling);
		}
		bChannelingTagApplied = bHasChannelers;
	}

	const bool bThresholdMet = ActiveChannelers.Num() >= FMath::Max(RequiredChannelers, 1);
	if (bThresholdMet && bShielded)
	{
		SetShieldStateFromObjective(false);
	}
	else if (!bThresholdMet
		&& bShieldRestoresWhenChannelingStops
		&& !bShielded
		&& !bIsDead)
	{
		SetShieldStateFromObjective(true);
	}

	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::ChannelingChanged,
		PhaseSnapshot.State);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_CHANNEL Guardian=%s Active=%d Required=%d Shielded=%s"),
		*GetName(),
		ActiveChannelers.Num(),
		RequiredChannelers,
		bShielded ? TEXT("true") : TEXT("false"));
}

void AmultiplayerGuardianCharacter::SetShieldStateFromObjective(bool bNewShielded)
{
	if (!HasAuthority()
		|| AbilitySystemComponent == nullptr
		|| (bIsDead && bNewShielded))
	{
		return;
	}

	if (!bNewShielded)
	{
		if (ShieldEffectHandle.IsValid())
		{
			AbilitySystemComponent->RemoveActiveGameplayEffect(ShieldEffectHandle);
			ShieldEffectHandle.Invalidate();
		}
	}
	else if (!ShieldEffectHandle.IsValid())
	{
		const FGameplayEffectContextHandle Context =
			AbilitySystemComponent->MakeEffectContext();
		const FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(
			ShieldEffectClass,
			1.0f,
			Context);
		if (!Spec.IsValid())
		{
			UE_LOG(
				LogMultiplayerGAS,
				Error,
				TEXT("GUARDIAN_SHIELD_REJECT Guardian=%s Reason=InvalidShieldEffect"),
				*GetName());
			return;
		}
		ShieldEffectHandle = AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(
			*Spec.Data.Get());
		if (!ShieldEffectHandle.IsValid())
		{
			UE_LOG(
				LogMultiplayerGAS,
				Error,
				TEXT("GUARDIAN_SHIELD_REJECT Guardian=%s Reason=ApplyFailed"),
				*GetName());
			return;
		}
		if (FOnActiveGameplayEffectRemoved_Info* RemovedDelegate =
			AbilitySystemComponent->OnGameplayEffectRemoved_InfoDelegate(
				ShieldEffectHandle))
		{
			RemovedDelegate->AddUObject(
				this,
				&AmultiplayerGuardianCharacter::HandleShieldEffectRemoved);
		}
	}

	if (bShielded == bNewShielded)
	{
		return;
	}

	bShielded = bNewShielded;
	ForceNetUpdate();
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::ShieldChanged,
		PhaseSnapshot.State);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_SHIELD Guardian=%s Shielded=%s"),
		*GetName(),
		bShielded ? TEXT("true") : TEXT("false"));
}

void AmultiplayerGuardianCharacter::HandleShieldEffectRemoved(
	const FGameplayEffectRemovalInfo& RemovalInfo)
{
	ShieldEffectHandle.Invalidate();
	if (!HasAuthority() || !bShielded)
	{
		return;
	}

	bShielded = false;
	if (!bEndingPlay)
	{
		ForceNetUpdate();
		DispatchPresentation(
			EmultiplayerGuardianPresentationReason::ShieldChanged,
			PhaseSnapshot.State);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GUARDIAN_SHIELD Guardian=%s Shielded=false Source=GameplayEffectRemoved"),
			*GetName());
	}
}

void AmultiplayerGuardianCharacter::HandleHealthChanged(
	const FOnAttributeChangeData& ChangeData)
{
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::HealthChanged,
		PhaseSnapshot.State);
	if (HasAuthority() && ChangeData.OldValue > 0.0f && ChangeData.NewValue <= 0.0f)
	{
		EnterDeathState();
	}
}

void AmultiplayerGuardianCharacter::EnterDeathState()
{
	if (!HasAuthority() || bIsDead || AbilitySystemComponent == nullptr)
	{
		return;
	}

	bIsDead = true;
	ClearAuthoritativeTimers();
	AbilitySystemComponent->CancelAllAbilities();
	AbilitySystemComponent->AddReplicatedLooseGameplayTag(
		MultiplayerGameplayTags::State_Dead);
	ClearChannelingParticipants();
	SetShieldStateFromObjective(false);
	SetAuthoritativeTarget(nullptr);
	SetAuthoritativeState(EmultiplayerGuardianAIState::Dead);
	AbilitySystemComponent->ExecuteGameplayCue(
		MultiplayerGameplayTags::GameplayCue_Coop_Death);
	ApplyDeathCollisionState();
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::Died,
		PhaseSnapshot.State);
	GrantAuthoritativeRewardOnce();
	ForceNetUpdate();

	if (AAIController* GuardianController = Cast<AAIController>(GetController()))
	{
		GuardianController->StopMovement();
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_DEATH Guardian=%s RewardGranted=%s"),
		*GetName(),
		bRewardGranted ? TEXT("true") : TEXT("false"));
}

void AmultiplayerGuardianCharacter::GrantAuthoritativeRewardOnce()
{
	if (!HasAuthority() || bRewardGranted)
	{
		return;
	}

	bRewardGranted = true;
	AuthorityRewardGrantedEvent.Broadcast(this);
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::RewardGranted,
		PhaseSnapshot.State);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GUARDIAN_REWARD Guardian=%s Sequence=%d"),
		*GetName(),
		PhaseSnapshot.Sequence);
}

void AmultiplayerGuardianCharacter::ClearAuthoritativeTimers()
{
	GetWorldTimerManager().ClearTimer(AttackResolveTimer);
	GetWorldTimerManager().ClearTimer(AttackCooldownTimer);
}

void AmultiplayerGuardianCharacter::ApplyTeamIdentity()
{
	if (AbilitySystemComponent == nullptr)
	{
		return;
	}

	AbilitySystemComponent->SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Team_Player,
		CoopTeamId == MultiplayerTeams::Players ? 1 : 0);
	AbilitySystemComponent->SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Team_Enemy,
		CoopTeamId == MultiplayerTeams::Enemies ? 1 : 0);
}

void AmultiplayerGuardianCharacter::ApplyGuardianStateTags(
	EmultiplayerGuardianAIState PreviousState,
	EmultiplayerGuardianAIState NewState)
{
	if (!HasAuthority() || AbilitySystemComponent == nullptr)
	{
		return;
	}

	if (PreviousState == EmultiplayerGuardianAIState::Windup)
	{
		AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(
			MultiplayerGameplayTags::State_Guardian_AI_Windup);
	}
	else if (PreviousState == EmultiplayerGuardianAIState::Attack)
	{
		AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(
			MultiplayerGameplayTags::State_Guardian_AI_Attacking);
	}

	if (NewState == EmultiplayerGuardianAIState::Windup)
	{
		AbilitySystemComponent->AddReplicatedLooseGameplayTag(
			MultiplayerGameplayTags::State_Guardian_AI_Windup);
	}
	else if (NewState == EmultiplayerGuardianAIState::Attack)
	{
		AbilitySystemComponent->AddReplicatedLooseGameplayTag(
			MultiplayerGameplayTags::State_Guardian_AI_Attacking);
	}
}

void AmultiplayerGuardianCharacter::ApplyDeathCollisionState()
{
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(
			bIsDead ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics);
	}
	if (bIsDead)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
	}
}

float AmultiplayerGuardianCharacter::GetSynchronizedServerTimeSeconds() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return 0.0f;
	}

	const AGameStateBase* GameState = World->GetGameState();
	return GameState != nullptr
		? GameState->GetServerWorldTimeSeconds()
		: World->GetTimeSeconds();
}

void AmultiplayerGuardianCharacter::RemoveChannelerDestroyedBindings()
{
	for (AActor* Participant : ActiveChannelers)
	{
		if (IsValid(Participant))
		{
			Participant->OnDestroyed.RemoveDynamic(
				this,
				&AmultiplayerGuardianCharacter::HandleChannelerDestroyed);
		}
	}
}

void AmultiplayerGuardianCharacter::HandleChannelerDestroyed(AActor* DestroyedActor)
{
	UnregisterChannelingParticipant(DestroyedActor);
}

void AmultiplayerGuardianCharacter::DispatchPresentation(
	EmultiplayerGuardianPresentationReason Reason,
	EmultiplayerGuardianAIState PreviousState)
{
	FmultiplayerGuardianPresentationEvent Event;
	Event.Reason = Reason;
	Event.PreviousState = PreviousState;
	Event.Phase = PhaseSnapshot;
	Event.Health = GetHealth();
	Event.MaxHealth = GetMaxHealth();
	Event.ActiveChannelers = ActiveChannelers.Num();
	Event.RequiredChannelers = RequiredChannelers;
	Event.bShielded = bShielded;
	Event.bDead = bIsDead;
	Event.bRewardGranted = bRewardGranted;
	ReceiveGuardianPresentation(Event);
}

void AmultiplayerGuardianCharacter::OnRep_CoopTeamId()
{
	ApplyTeamIdentity();
}

void AmultiplayerGuardianCharacter::OnRep_PhaseSnapshot(
	FmultiplayerGuardianPhaseSnapshot PreviousSnapshot)
{
	const EmultiplayerGuardianPresentationReason Reason =
		PreviousSnapshot.State != PhaseSnapshot.State
		|| PreviousSnapshot.Sequence != PhaseSnapshot.Sequence
			? EmultiplayerGuardianPresentationReason::PhaseChanged
			: EmultiplayerGuardianPresentationReason::TargetChanged;
	DispatchPresentation(Reason, PreviousSnapshot.State);
}

void AmultiplayerGuardianCharacter::OnRep_Shielded()
{
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::ShieldChanged,
		PhaseSnapshot.State);
}

void AmultiplayerGuardianCharacter::OnRep_IsDead()
{
	ApplyDeathCollisionState();
	if (bIsDead)
	{
		DispatchPresentation(
			EmultiplayerGuardianPresentationReason::Died,
			PhaseSnapshot.State);
	}
}

void AmultiplayerGuardianCharacter::OnRep_RewardGranted()
{
	if (bRewardGranted)
	{
		DispatchPresentation(
			EmultiplayerGuardianPresentationReason::RewardGranted,
			PhaseSnapshot.State);
	}
}

void AmultiplayerGuardianCharacter::OnRep_ActiveChannelers()
{
	DispatchPresentation(
		EmultiplayerGuardianPresentationReason::ChannelingChanged,
		PhaseSnapshot.State);
}

void AmultiplayerGuardianCharacter::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, CoopTeamId);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, PhaseSnapshot);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, bShielded);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, bIsDead);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, bRewardGranted);
	DOREPLIFETIME(AmultiplayerGuardianCharacter, ActiveChannelers);
}
