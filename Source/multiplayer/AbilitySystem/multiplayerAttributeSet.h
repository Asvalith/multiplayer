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
};

#undef MULTIPLAYER_ATTRIBUTE_ACCESSORS
