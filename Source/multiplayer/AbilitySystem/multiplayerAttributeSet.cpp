// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerAttributeSet.h"

#include "AbilitySystem/multiplayerGameplayTags.h"
#include "AbilitySystem/multiplayerGameplayEffectContext.h"
#include "multiplayer.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

namespace
{
	float ClampFinite(float Value, float Minimum, float Maximum, float Fallback)
	{
		return FMath::IsFinite(Value)
			? FMath::Clamp(Value, Minimum, Maximum)
			: Fallback;
	}

	float ClampFiniteMinimum(float Value, float Minimum, float Fallback)
	{
		return FMath::IsFinite(Value) ? FMath::Max(Value, Minimum) : Fallback;
	}
}

UmultiplayerAttributeSet::UmultiplayerAttributeSet()
{
	InitMaxHealth(100.0f);
	InitHealth(100.0f);
	InitMaxEnergy(100.0f);
	InitEnergy(100.0f);
	InitAttackPower(0.0f);
	InitArmor(0.0f);
	InitCriticalChance(0.0f);
	InitCriticalMultiplier(1.5f);
	InitResistance(0.0f);
}

void UmultiplayerAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Energy, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, MaxEnergy, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, AttackPower, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Armor, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, CriticalChance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, CriticalMultiplier, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerAttributeSet, Resistance, COND_None, REPNOTIFY_Always);
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
	else if (Attribute == GetAttackPowerAttribute() || Attribute == GetArmorAttribute())
	{
		NewValue = ClampFiniteMinimum(NewValue, 0.0f, 0.0f);
	}
	else if (Attribute == GetCriticalChanceAttribute())
	{
		NewValue = ClampFinite(NewValue, 0.0f, 1.0f, 0.0f);
	}
	else if (Attribute == GetCriticalMultiplierAttribute())
	{
		NewValue = ClampFiniteMinimum(NewValue, 1.0f, 1.0f);
	}
	else if (Attribute == GetResistanceAttribute())
	{
		NewValue = ClampFinite(NewValue, 0.0f, 0.8f, 0.0f);
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
			const FGameplayEffectContext* BaseContext = Data.EffectSpec.GetContext().Get();
			const FmultiplayerGameplayEffectContext* CoopContext =
				BaseContext != nullptr
				&& BaseContext->GetScriptStruct()->IsChildOf(
					FmultiplayerGameplayEffectContext::StaticStruct())
					? static_cast<const FmultiplayerGameplayEffectContext*>(BaseContext)
					: nullptr;
			UE_LOG(
				LogMultiplayerGAS,
				Display,
				TEXT("GAS_DAMAGE_CONTEXT Target=%s Damage=%.1f Critical=%s HitType=%d Impulse=%s"),
				*GetNameSafe(GetOwningActor()),
				Damage,
				CoopContext != nullptr && CoopContext->IsCriticalHit() ? TEXT("true") : TEXT("false"),
				CoopContext != nullptr ? static_cast<int32>(CoopContext->GetHitType()) : -1,
				CoopContext != nullptr
					? *CoopContext->GetImpactImpulse().ToCompactString()
					: TEXT("None"));
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
	else if (Data.EvaluatedData.Attribute == GetAttackPowerAttribute())
	{
		SetAttackPower(ClampFiniteMinimum(GetAttackPower(), 0.0f, 0.0f));
	}
	else if (Data.EvaluatedData.Attribute == GetArmorAttribute())
	{
		SetArmor(ClampFiniteMinimum(GetArmor(), 0.0f, 0.0f));
	}
	else if (Data.EvaluatedData.Attribute == GetCriticalChanceAttribute())
	{
		SetCriticalChance(ClampFinite(GetCriticalChance(), 0.0f, 1.0f, 0.0f));
	}
	else if (Data.EvaluatedData.Attribute == GetCriticalMultiplierAttribute())
	{
		SetCriticalMultiplier(ClampFiniteMinimum(GetCriticalMultiplier(), 1.0f, 1.0f));
	}
	else if (Data.EvaluatedData.Attribute == GetResistanceAttribute())
	{
		SetResistance(ClampFinite(GetResistance(), 0.0f, 0.8f, 0.0f));
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

void UmultiplayerAttributeSet::OnRep_AttackPower(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, AttackPower, OldValue);
}

void UmultiplayerAttributeSet::OnRep_Armor(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, Armor, OldValue);
}

void UmultiplayerAttributeSet::OnRep_CriticalChance(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, CriticalChance, OldValue);
}

void UmultiplayerAttributeSet::OnRep_CriticalMultiplier(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, CriticalMultiplier, OldValue);
}

void UmultiplayerAttributeSet::OnRep_Resistance(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerAttributeSet, Resistance, OldValue);
}
