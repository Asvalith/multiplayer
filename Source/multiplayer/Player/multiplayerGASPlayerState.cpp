// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerGASPlayerState.h"

#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "multiplayerGameMode.h"
#include "multiplayerCharacter.h"
#include "multiplayer.h"
#include "Net/UnrealNetwork.h"

AmultiplayerGASPlayerState::AmultiplayerGASPlayerState()
{
	AbilitySystemComponent = CreateDefaultSubobject<UmultiplayerAbilitySystemComponent>(
		TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	AttributeSet = CreateDefaultSubobject<UmultiplayerAttributeSet>(TEXT("AttributeSet"));

	// A measured baseline for a two-player demo. Do not copy Aura's unconditional 100 Hz setting.
	SetNetUpdateFrequency(30.0f);
	SetMinNetUpdateFrequency(10.0f);
}

void AmultiplayerGASPlayerState::BeginPlay()
{
	Super::BeginPlay();
	ApplyTeamIdentity();

	AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerAttributeSet::GetHealthAttribute()).AddUObject(
			this,
			&AmultiplayerGASPlayerState::HandleHealthChanged);
	AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerAttributeSet::GetEnergyAttribute()).AddUObject(
			this,
			&AmultiplayerGASPlayerState::HandleEnergyChanged);
}

void AmultiplayerGASPlayerState::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(RespawnTimerHandle);
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerGASPlayerState::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerGASPlayerState, CoopTeamId);
	DOREPLIFETIME(AmultiplayerGASPlayerState, bIsDead);
}

void AmultiplayerGASPlayerState::OnRep_CoopTeamId()
{
	ApplyTeamIdentity();
}

void AmultiplayerGASPlayerState::ApplyTeamIdentity()
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

UAbilitySystemComponent* AmultiplayerGASPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

float AmultiplayerGASPlayerState::GetHealth() const
{
	return AttributeSet != nullptr ? AttributeSet->GetHealth() : 0.0f;
}

float AmultiplayerGASPlayerState::GetMaxHealth() const
{
	return AttributeSet != nullptr ? AttributeSet->GetMaxHealth() : 0.0f;
}

float AmultiplayerGASPlayerState::GetEnergy() const
{
	return AttributeSet != nullptr ? AttributeSet->GetEnergy() : 0.0f;
}

float AmultiplayerGASPlayerState::GetMaxEnergy() const
{
	return AttributeSet != nullptr ? AttributeSet->GetMaxEnergy() : 0.0f;
}

void AmultiplayerGASPlayerState::InitializeAbilityActorInfo(AActor* AvatarActor)
{
	if (AbilitySystemComponent != nullptr && AvatarActor != nullptr)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, AvatarActor);
	}
}

void AmultiplayerGASPlayerState::GrantStartupAbilities(const UmultiplayerAbilitySet* AbilitySet)
{
	if (!HasAuthority() || bStartupAbilitiesGranted || AbilitySystemComponent == nullptr)
	{
		return;
	}

	if (AbilitySet == nullptr)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("Cannot grant startup GAS abilities to %s: StartupAbilitySet is not configured"),
			*GetName());
		return;
	}

	AbilitySet->GiveToAbilitySystem(
		AbilitySystemComponent,
		&StartupGrantedHandles,
		this);
	int32 GrantedAbilityCount = 0;
	for (const FGameplayAbilitySpecHandle& Handle : StartupGrantedHandles.AbilitySpecHandles)
	{
		GrantedAbilityCount += Handle.IsValid() ? 1 : 0;
	}
	int32 GrantedEffectCount = 0;
	for (const FActiveGameplayEffectHandle& Handle : StartupGrantedHandles.GameplayEffectHandles)
	{
		GrantedEffectCount += Handle.IsValid() ? 1 : 0;
	}
	if (GrantedAbilityCount == 0 && GrantedEffectCount == 0)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("Cannot mark startup GAS grant complete for %s: AbilitySet %s granted no valid abilities or effects"),
			*GetName(),
			*GetNameSafe(AbilitySet));
		return;
	}

	bStartupAbilitiesGranted = true;
	UE_LOG(
		LogMultiplayerGAS,
		Log,
		TEXT("Granted startup GAS set %s to %s: Abilities=%d Effects=%d"),
		*GetNameSafe(AbilitySet),
		*GetName(),
		GrantedAbilityCount,
		GrantedEffectCount);
}

void AmultiplayerGASPlayerState::HandleHealthChanged(const FOnAttributeChangeData& ChangeData)
{
	OnHealthChanged.Broadcast(ChangeData.OldValue, ChangeData.NewValue);
	if (HasAuthority() && ChangeData.NewValue <= 0.0f)
	{
		EnterDeathState();
	}
}

void AmultiplayerGASPlayerState::HandleEnergyChanged(const FOnAttributeChangeData& ChangeData)
{
	OnEnergyChanged.Broadcast(ChangeData.OldValue, ChangeData.NewValue);
}

void AmultiplayerGASPlayerState::EnterDeathState()
{
	if (!HasAuthority() || bIsDead || AbilitySystemComponent == nullptr)
	{
		return;
	}

	bIsDead = true;
	AbilitySystemComponent->AddReplicatedLooseGameplayTag(
		MultiplayerGameplayTags::State_Dead);
	AbilitySystemComponent->CancelAllAbilities();
	ClearTransientAbilityState();
	AbilitySystemComponent->ExecuteGameplayCue(
		MultiplayerGameplayTags::GameplayCue_Coop_Death);
	ApplyDeathStateToAvatar();
	OnDeathStateChanged.Broadcast(true);
	ForceNetUpdate();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			RespawnTimerHandle,
			this,
			&AmultiplayerGASPlayerState::CompleteRespawn,
			RespawnDelaySeconds,
			false);
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_DEATH PlayerState=%s Avatar=%s RespawnDelay=%.1f"),
		*GetName(),
		*GetNameSafe(GetPawn()),
		RespawnDelaySeconds);
}

void AmultiplayerGASPlayerState::CompleteRespawn()
{
	if (!HasAuthority() || !bIsDead || AbilitySystemComponent == nullptr || AttributeSet == nullptr)
	{
		return;
	}

	APlayerController* PlayerController = GetPlayerController();
	if (PlayerController == nullptr)
	{
		return;
	}

	APawn* OldPawn = GetPawn();
	if (OldPawn != nullptr)
	{
		OldPawn->DetachFromControllerPendingDestroy();
		OldPawn->Destroy();
	}

	ClearTransientAbilityState();
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetHealthAttribute(),
		AttributeSet->GetMaxHealth());
	AbilitySystemComponent->SetNumericAttributeBase(
		UmultiplayerAttributeSet::GetEnergyAttribute(),
		AttributeSet->GetMaxEnergy());
	AbilitySystemComponent->RemoveReplicatedLooseGameplayTag(
		MultiplayerGameplayTags::State_Dead);
	bIsDead = false;
	OnDeathStateChanged.Broadcast(false);
	ForceNetUpdate();

	if (AmultiplayerGameMode* GameMode =
		GetWorld() != nullptr ? GetWorld()->GetAuthGameMode<AmultiplayerGameMode>() : nullptr)
	{
		GameMode->RestartPlayer(PlayerController);
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_RESPAWN PlayerState=%s NewAvatar=%s Health=%.1f Energy=%.1f"),
		*GetName(),
		*GetNameSafe(GetPawn()),
		GetHealth(),
		GetEnergy());
}

void AmultiplayerGASPlayerState::OnRep_IsDead()
{
	ApplyDeathStateToAvatar();
	OnDeathStateChanged.Broadcast(bIsDead);
}

void AmultiplayerGASPlayerState::ApplyDeathStateToAvatar()
{
	if (AmultiplayerCharacter* Character = GetPawn<AmultiplayerCharacter>())
	{
		Character->ApplyDeathState(bIsDead);
	}
}

void AmultiplayerGASPlayerState::ClearTransientAbilityState()
{
	FGameplayTagContainer TagsToRemove;
	TagsToRemove.AddTag(MultiplayerGameplayTags::Cooldown_Ability_Damage);
	TagsToRemove.AddTag(MultiplayerGameplayTags::Cooldown_Ability_Heal);
	TagsToRemove.AddTag(MultiplayerGameplayTags::Cooldown_Ability_Immunity);
	TagsToRemove.AddTag(MultiplayerGameplayTags::Effect_Positive_Immunity);
	TagsToRemove.AddTag(MultiplayerGameplayTags::State_Immune);
	TagsToRemove.AddTag(MultiplayerGameplayTags::State_Vulnerable);
	TagsToRemove.AddTag(MultiplayerGameplayTags::Effect_Negative_Vulnerability);
	AbilitySystemComponent->RemoveActiveEffectsWithTags(TagsToRemove);
	AbilitySystemComponent->SetLooseGameplayTagCount(
		MultiplayerGameplayTags::Debug_ForceReject_Immunity,
		0);
}
