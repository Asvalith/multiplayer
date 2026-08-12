// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAttributeSet.h"

#include "AbilitySystem/multiplayerGameplayTags.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

UmultiplayerAttributeSet::UmultiplayerAttributeSet()
{
	InitMaxHealth(100.0f);
	InitHealth(100.0f);
	InitMaxEnergy(100.0f);
	InitEnergy(100.0f);
}

void UmultiplayerAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Energy, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, MaxEnergy, COND_None, REPNOTIFY_Always);
}

void UmultiplayerAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetEnergyAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxEnergy());
	}
	else if (Attribute == GetMaxHealthAttribute() || Attribute == GetMaxEnergyAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
}

void UmultiplayerAttributeSet::PostAttributeChange(
	const FGameplayAttribute& Attribute,
	float OldValue,
	float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);

	if (Attribute == GetMaxHealthAttribute() && GetHealth() > NewValue)
	{
		SetHealth(NewValue);
	}
	else if (Attribute == GetMaxEnergyAttribute() && GetEnergy() > NewValue)
	{
		SetEnergy(NewValue);
	}
}

void UmultiplayerAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		const float Damage = FMath::Max(GetIncomingDamage(), 0.0f);
		SetIncomingDamage(0.0f);

		UAbilitySystemComponent* TargetASC = GetOwningAbilitySystemComponent();
		const bool bImmune = TargetASC != nullptr
			&& TargetASC->HasMatchingGameplayTag(MultiplayerGameplayTags::State_Immune);
		if (Damage > 0.0f && !bImmune)
		{
			SetHealth(FMath::Clamp(GetHealth() - Damage, 0.0f, GetMaxHealth()));
		}
	}
	else if (Data.EvaluatedData.Attribute == GetIncomingHealingAttribute())
	{
		const float Healing = FMath::Max(GetIncomingHealing(), 0.0f);
		SetIncomingHealing(0.0f);
		if (Healing > 0.0f)
		{
			SetHealth(FMath::Clamp(GetHealth() + Healing, 0.0f, GetMaxHealth()));
		}
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		SetHealth(FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetEnergyAttribute())
	{
		SetEnergy(FMath::Clamp(GetEnergy(), 0.0f, GetMaxEnergy()));
	}
}

void UmultiplayerAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, Health, OldValue);
}

void UmultiplayerAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, MaxHealth, OldValue);
}

void UmultiplayerAttributeSet::OnRep_Energy(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, Energy, OldValue);
}

void UmultiplayerAttributeSet::OnRep_MaxEnergy(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, MaxEnergy, OldValue);
}
