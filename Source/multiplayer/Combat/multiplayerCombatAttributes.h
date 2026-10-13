// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "multiplayerCombatAttributes.generated.h"

#define MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UmultiplayerCombatAttributes, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

// 血量和能量是持续状态；IncomingDamage 仅用于服务器一次伤害结算，不向客户端复制。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatAttributes : public UAttributeSet
{
	GENERATED_BODY()

public:
	// 为玩家与敌人提供相同的基础属性默认值。
	UmultiplayerCombatAttributes();

	// 注册持续属性复制，并保证预测属性也收到 RepNotify。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// 属性聚合值发生变化前限制范围，避免血量或能量越界。
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	// 效果执行后消费伤害、应用防御减伤并约束治疗结果。
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Combat|Attributes")
	FGameplayAttributeData Health;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(Health)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Combat|Attributes")
	FGameplayAttributeData MaxHealth;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(MaxHealth)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Energy, Category = "Combat|Attributes")
	FGameplayAttributeData Energy;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(Energy)

	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxEnergy, Category = "Combat|Attributes")
	FGameplayAttributeData MaxEnergy;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(MaxEnergy)

	UPROPERTY()
	FGameplayAttributeData IncomingDamage;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(IncomingDamage)

private:
	// 将复制后的血量交回 GAS，更新属性委托及预测账本。
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldHealth);

	// 通知 GAS 最大血量已经同步。
	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth);

	// 通知 GAS 能量已经同步。
	UFUNCTION()
	void OnRep_Energy(const FGameplayAttributeData& OldEnergy);

	// 通知 GAS 最大能量已经同步。
	UFUNCTION()
	void OnRep_MaxEnergy(const FGameplayAttributeData& OldMaxEnergy);
};

#undef MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS
