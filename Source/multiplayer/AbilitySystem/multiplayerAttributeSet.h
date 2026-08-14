// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "multiplayerAttributeSet.generated.h"

#define MULTIPLAYER_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

UCLASS()
class MULTIPLAYER_API UmultiplayerAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UmultiplayerAttributeSet();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "GAS|Attributes")
	FGameplayAttributeData Health;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, Health);

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "GAS|Attributes")
	FGameplayAttributeData MaxHealth;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, MaxHealth);

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Energy, Category = "GAS|Attributes")
	FGameplayAttributeData Energy;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, Energy);

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxEnergy, Category = "GAS|Attributes")
	FGameplayAttributeData MaxEnergy;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, MaxEnergy);

	/** Additive source-side contribution captured by damage executions. */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AttackPower, Category = "GAS|Attributes|Combat")
	FGameplayAttributeData AttackPower;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, AttackPower);

	/** Non-negative target-side physical mitigation input. */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Armor, Category = "GAS|Attributes|Combat")
	FGameplayAttributeData Armor;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, Armor);

	/** Server-authoritative critical chance expressed in the inclusive range [0, 1]. */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CriticalChance, Category = "GAS|Attributes|Combat")
	FGameplayAttributeData CriticalChance;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, CriticalChance);

	/** Damage multiplier applied to critical hits; never lower than 1. */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CriticalMultiplier, Category = "GAS|Attributes|Combat")
	FGameplayAttributeData CriticalMultiplier;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, CriticalMultiplier);

	/** Target-side proportional mitigation expressed in the inclusive range [0, 0.8]. */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Resistance, Category = "GAS|Attributes|Combat")
	FGameplayAttributeData Resistance;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, Resistance);

	/** Server-only transient accumulator used by damage effects. */
	UPROPERTY(BlueprintReadOnly, Category = "GAS|Meta")
	FGameplayAttributeData IncomingDamage;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, IncomingDamage);

	/** Server-only transient accumulator used by healing effects. */
	UPROPERTY(BlueprintReadOnly, Category = "GAS|Meta")
	FGameplayAttributeData IncomingHealing;
	MULTIPLAYER_ATTRIBUTE_ACCESSORS(UmultiplayerAttributeSet, IncomingHealing);

protected:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_Energy(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_MaxEnergy(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_AttackPower(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_Armor(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_CriticalChance(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_CriticalMultiplier(const FGameplayAttributeData& OldValue);

	UFUNCTION()
	void OnRep_Resistance(const FGameplayAttributeData& OldValue);
};

#undef MULTIPLAYER_ATTRIBUTE_ACCESSORS
