// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "GameplayEffectExecutionCalculation.h"
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
	// 基础值与聚合值使用同一份范围规则，直接设置属性也不能把越界基础值留在聚合器中。
	virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
	// 上限已提交后再压回现有护盾；直接 Base setter 与 GE 修改上限走同一个收尾。
	virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;

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

	// 护盾先吸收减伤后的伤害；与血量一样复制给观察者，不另存一份客户端护盾状态。
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Shield, Category = "Combat|Attributes")
	FGameplayAttributeData Shield;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(Shield)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxShield, Category = "Combat|Attributes")
	FGameplayAttributeData MaxShield;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(MaxShield)

	UPROPERTY()
	FGameplayAttributeData IncomingDamage;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(IncomingDamage)

	// 来源攻击加成与目标护甲参与同一服务器结算；零值保留当前 Demo 的默认伤害。
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Attributes") FGameplayAttributeData AttackPower;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(AttackPower)
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Attributes") FGameplayAttributeData Armor;
	MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS(Armor)

private:
	// 纯数值钳制，不在 Pre 回调中发送事件、扣除其他属性或执行玩法。
	void ClampAttributeValue(const FGameplayAttribute& Attribute, float& NewValue) const;

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

	// 护盾及上限都通过 GAS RepNotify 更新订阅者与预测账本。
	UFUNCTION()
	void OnRep_Shield(const FGameplayAttributeData& OldShield);
	UFUNCTION()
	void OnRep_MaxShield(const FGameplayAttributeData& OldMaxShield);
};

// 只在服务器执行：伤害基础值 + 来源攻击加成 - 目标护甲；再交属性集处理防御和死亡。
UCLASS()
class MULTIPLAYER_API UmultiplayerCombatDamageExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()
public:
	UmultiplayerCombatDamageExecution();
	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
		FGameplayEffectCustomExecutionOutput& Output) const override;
};

#undef MULTIPLAYER_COMBAT_ATTRIBUTE_ACCESSORS
