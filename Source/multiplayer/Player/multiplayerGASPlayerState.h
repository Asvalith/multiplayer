// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AbilitySystemInterface.h"
#include "AbilitySystem/multiplayerAbilitySet.h"
#include "GameFramework/PlayerState.h"
#include "Team/multiplayerCoopTeamAgentInterface.h"
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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FmultiplayerDeathStateChangedEvent,
	bool,
	bIsDead);

UCLASS()
class MULTIPLAYER_API AmultiplayerGASPlayerState : public APlayerState, public IAbilitySystemInterface, public ImultiplayerCoopTeamAgentInterface
{
	GENERATED_BODY()

public:
	AmultiplayerGASPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual int32 GetCoopTeamId_Implementation() const override { return CoopTeamId; }

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

	UFUNCTION(BlueprintPure, Category = "GAS|Life")
	bool IsDead() const { return bIsDead; }

	void InitializeAbilityActorInfo(AActor* AvatarActor);
	void GrantStartupAbilities(const UmultiplayerAbilitySet* AbilitySet);

	UPROPERTY(BlueprintAssignable, Category = "GAS|Attributes")
	FmultiplayerAttributeChangedEvent OnHealthChanged;

	UPROPERTY(BlueprintAssignable, Category = "GAS|Attributes")
	FmultiplayerAttributeChangedEvent OnEnergyChanged;

	UPROPERTY(BlueprintAssignable, Category = "GAS|Life")
	FmultiplayerDeathStateChangedEvent OnDeathStateChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UFUNCTION()
	void OnRep_CoopTeamId();

	void ApplyTeamIdentity();
	void EnterDeathState();
	void CompleteRespawn();
	void ApplyDeathStateToAvatar();
	void ClearTransientAbilityState();
	void HandleHealthChanged(const FOnAttributeChangeData& ChangeData);
	void HandleEnergyChanged(const FOnAttributeChangeData& ChangeData);
	void GrantBuiltInDemoAbilities();

	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UmultiplayerAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UmultiplayerAttributeSet> AttributeSet;

	bool bStartupAbilitiesGranted = false;
	FmultiplayerAbilitySetGrantedHandles StartupGrantedHandles;

	UPROPERTY(ReplicatedUsing = OnRep_CoopTeamId)
	int32 CoopTeamId = MultiplayerTeams::Players;

	UPROPERTY(ReplicatedUsing = OnRep_IsDead)
	bool bIsDead = false;

	UFUNCTION()
	void OnRep_IsDead();

	UPROPERTY(EditDefaultsOnly, Category = "GAS|Life", meta = (ClampMin = "0.1"))
	float RespawnDelaySeconds = 3.0f;

	FTimerHandle RespawnTimerHandle;
};
