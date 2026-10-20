// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Abilities/GameplayAbility.h"
#include "multiplayerCombatCoop.generated.h"

class AmultiplayerCombatCharacter;

// 只管理合作生命流程：倒地、失血期限、救援占用；数值与技能仍通过角色 ASC。
UCLASS(ClassGroup=(Combat), meta=(BlueprintSpawnableComponent))
class MULTIPLAYER_API UmultiplayerCombatCoopComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UmultiplayerCombatCoopComponent();
	UPROPERTY(EditDefaultsOnly, Category="Combat") bool bEnableDowned = false;
	UFUNCTION(BlueprintPure) bool IsDowned() const { return bDowned; }
	UFUNCTION(BlueprintPure) bool IsRescuing() const { return RescueTarget != nullptr; }
	UFUNCTION(BlueprintPure) float GetDownedRemaining() const;
	UFUNCTION(BlueprintPure) float GetRescueProgress() const;
	bool TryEnterDowned();
	void RestoreDowned(float Remaining);
	bool CanStartRescue() const;
	bool BeginRescue();
	bool IsRescueValid() const;
	bool CompleteRescue();
	void EndRescue();
	void ClearLifeState();
protected:
	virtual void EndPlay(EEndPlayReason::Type Reason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Props) const override;
private:
	AmultiplayerCombatCharacter* OwnerCharacter() const;
	AmultiplayerCombatCharacter* FindRescueTarget() const;
	bool HasActivePartner() const;
	bool CanReach(const AmultiplayerCombatCharacter* Target) const;
	void BleedOut();
	UFUNCTION() void NotifyLifeState();
	UPROPERTY(ReplicatedUsing=NotifyLifeState) bool bDowned = false;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) double DownedUntil = 0;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) TObjectPtr<AmultiplayerCombatCharacter> RescueTarget;
	UPROPERTY(ReplicatedUsing=NotifyLifeState) double RescueUntil = 0;
	TWeakObjectPtr<AmultiplayerCombatCharacter> CurrentRescuer;
	FTimerHandle BleedoutTimer;
};

// 引导是服务器 GAS 能力；任务、占用和移动限制在同一个 EndAbility 收尾。
UCLASS()
class MULTIPLAYER_API UmultiplayerRescueAbility : public UGameplayAbility
{
	GENERATED_BODY()
public:
	UmultiplayerRescueAbility();
	virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		const FGameplayTagContainer* Source=nullptr, const FGameplayTagContainer* Target=nullptr,
		FGameplayTagContainer* Reasons=nullptr) const override;
	virtual bool DoesAbilitySatisfyTagRequirements(const UAbilitySystemComponent& ASC,
		const FGameplayTagContainer* Source=nullptr, const FGameplayTagContainer* Target=nullptr,
		FGameplayTagContainer* Reasons=nullptr) const override;
protected:
	virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		FGameplayAbilityActivationInfo Activation, const FGameplayEventData* Event) override;
	virtual void EndAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
		FGameplayAbilityActivationInfo Activation, bool bReplicate, bool bCancelled) override;
private:
	UFUNCTION() void FinishRescue();
	void CheckRescue();
	FTimerHandle ValidationTimer;
	bool bEndingRescue = false;
};
