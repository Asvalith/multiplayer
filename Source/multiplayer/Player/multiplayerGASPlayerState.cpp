// Copyright Epic Games, Inc. All Rights Reserved.

#include "Player/multiplayerGASPlayerState.h"

#include "AbilitySystem/Abilities/multiplayerGameplayAbility.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayAbilitySpec.h"
#include "multiplayer.h"

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

	AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerAttributeSet::GetHealthAttribute()).AddUObject(
			this,
			&AmultiplayerGASPlayerState::HandleHealthChanged);
	AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(
		UmultiplayerAttributeSet::GetEnergyAttribute()).AddUObject(
			this,
			&AmultiplayerGASPlayerState::HandleEnergyChanged);
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

	if (AbilitySet != nullptr)
	{
		AbilitySet->GiveToAbilitySystem(AbilitySystemComponent, &StartupGrantedHandles, this);
	}
	else
	{
		GrantBuiltInDemoAbilities();
	}

	bStartupAbilitiesGranted = true;
	UE_LOG(
		LogMultiplayerGAS,
		Log,
		TEXT("Granted %d startup GAS abilities to %s"),
		StartupGrantedHandles.AbilitySpecHandles.Num(),
		*GetName());
}

void AmultiplayerGASPlayerState::HandleHealthChanged(const FOnAttributeChangeData& ChangeData)
{
	OnHealthChanged.Broadcast(ChangeData.OldValue, ChangeData.NewValue);
}

void AmultiplayerGASPlayerState::HandleEnergyChanged(const FOnAttributeChangeData& ChangeData)
{
	OnEnergyChanged.Broadcast(ChangeData.OldValue, ChangeData.NewValue);
}

void AmultiplayerGASPlayerState::GrantBuiltInDemoAbilities()
{
	struct FBuiltInAbility
	{
		TSubclassOf<UGameplayAbility> AbilityClass;
		FGameplayTag InputTag;
	};

	const FBuiltInAbility BuiltInAbilities[] =
	{
		{ UmultiplayerDamageAbility::StaticClass(), MultiplayerGameplayTags::InputTag_Ability_Damage },
		{ UmultiplayerHealAbility::StaticClass(), MultiplayerGameplayTags::InputTag_Ability_Heal },
		{ UmultiplayerImmunityAbility::StaticClass(), MultiplayerGameplayTags::InputTag_Ability_Immunity }
	};

	for (const FBuiltInAbility& Entry : BuiltInAbilities)
	{
		FGameplayAbilitySpec AbilitySpec(Entry.AbilityClass, 1, INDEX_NONE, this);
		AbilitySpec.GetDynamicSpecSourceTags().AddTag(Entry.InputTag);
		StartupGrantedHandles.AbilitySpecHandles.Add(
			AbilitySystemComponent->GiveAbility(AbilitySpec));
	}
}
