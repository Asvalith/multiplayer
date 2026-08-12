// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "GameFramework/PlayerState.h"
#include "multiplayerGASPlayerState.generated.h"

class UmultiplayerAbilitySet;
class UmultiplayerAbilitySystemComponent;
class UmultiplayerAttributeSet;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FmultiplayerAttributeChangedEvent,
	float,
	OldValue,
	float,
	NewValue);

UCLASS()
class MULTIPLAYER_API AmultiplayerGASPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AmultiplayerGASPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UFUNCTION(BlueprintPure, Category = "GAS")
	UmultiplayerAbilitySystemComponent* GetMultiplayerAbilitySystemComponent() const
	{
		return AbilitySystemComponent;
	}

	UFUNCTION(BlueprintPure, Category = "GAS")
	UmultiplayerAttributeSet* GetAttributeSet() const { return AttributeSet; }

	UFUNCTION(BlueprintPure, Category = "GAS|Attributes")
	float GetHealth() const;

	UFUNCTION(BlueprintPure, Category = "GAS|Attributes")
	float GetMaxHealth() const;

	UFUNCTION(BlueprintPure, Category = "GAS|Attributes")
	float GetEnergy() const;

	UFUNCTION(BlueprintPure, Category = "GAS|Attributes")
	float GetMaxEnergy() const;

	void InitializeAbilityActorInfo(AActor* AvatarActor);
	void GrantStartupAbilities(const UmultiplayerAbilitySet* AbilitySet);

	UPROPERTY(BlueprintAssignable, Category = "GAS|Attributes")
	FmultiplayerAttributeChangedEvent OnHealthChanged;

	UPROPERTY(BlueprintAssignable, Category = "GAS|Attributes")
	FmultiplayerAttributeChangedEvent OnEnergyChanged;

protected:
	virtual void BeginPlay() override;

private:
	void HandleHealthChanged(const FOnAttributeChangeData& ChangeData);
	void HandleEnergyChanged(const FOnAttributeChangeData& ChangeData);
	void GrantBuiltInDemoAbilities();

	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	bool bStartupAbilitiesGranted = false;
	FmultiplayerAbilitySetGrantedHandles StartupGrantedHandles;
};
