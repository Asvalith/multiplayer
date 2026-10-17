// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatAttributes.h"

#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

// 每次重新生成角色都会创建新的属性集，不继承上一轮的血量或能量。
UmultiplayerCombatAttributes::UmultiplayerCombatAttributes()
{
	InitHealth(100.0f);
	InitMaxHealth(100.0f);
	InitEnergy(100.0f);
	InitMaxEnergy(100.0f);
}

// 使用 Always 通知，让客户端即使数值未变也能完成预测与服务器结果的核对。
void UmultiplayerCombatAttributes::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, MaxHealth, COND_None, REPNOTIFY_Always);
	// 血量供其他玩家观察；能量只给所属玩家显示和预测，不向无关连接转发。
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, Energy, COND_OwnerOnly, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, MaxEnergy, COND_OwnerOnly, REPNOTIFY_Always);
}

// 限制聚合值；死亡后保持零血量，本版本的治疗不承担复活职责。
void UmultiplayerCombatAttributes::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	if (Attribute == GetHealthAttribute())
	{
		const UAbilitySystemComponent* AbilitySystem = GetOwningAbilitySystemComponent();
		NewValue = AbilitySystem != nullptr && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Dead)
			? 0.0f : FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
	}
	else if (Attribute == GetEnergyAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxEnergy());
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		NewValue = FMath::Max(1.0f, NewValue);
	}
	else if (Attribute == GetMaxEnergyAttribute())
	{
		NewValue = FMath::Max(0.0f, NewValue);
	}
}

// 先清空本次伤害再修改血量，血量委托即使触发死亡清理也不会重复消费旧伤害。
void UmultiplayerCombatAttributes::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	const bool bDead = Data.Target.HasMatchingGameplayTag(CombatTags::State_Dead);
	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		float Damage = FMath::Max(0.0f, GetIncomingDamage());
		SetIncomingDamage(0.0f);
		const AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(Data.Target.GetAvatarActor());
		if (bDead || Damage == 0.0f || (Character != nullptr && !Character->IsCombatReady()))
		{
			return;
		}
		if (Data.Target.HasMatchingGameplayTag(CombatTags::State_Defending))
		{
			Damage *= Character ? Character->GetCombatConfig().DefenseDamageMultiplier : .5f;
		}
		SetHealth(FMath::Clamp(GetHealth() - Damage, 0.0f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		SetHealth(bDead ? 0.0f : FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
	}
	else if (Data.EvaluatedData.Attribute == GetEnergyAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxEnergyAttribute())
	{
		SetEnergy(FMath::Clamp(GetEnergy(), 0.0f, GetMaxEnergy()));
	}
}

// RepNotify 交由 GAS 执行，属性变化订阅者由同一机制获得通知。
void UmultiplayerCombatAttributes::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, Health, OldHealth);
}

// 同步最大血量的属性聚合器。
void UmultiplayerCombatAttributes::OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, MaxHealth, OldMaxHealth);
}

// 同步能量，并完成可能的消耗预测核对。
void UmultiplayerCombatAttributes::OnRep_Energy(const FGameplayAttributeData& OldEnergy)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, Energy, OldEnergy);
}

// 同步最大能量的属性聚合器。
void UmultiplayerCombatAttributes::OnRep_MaxEnergy(const FGameplayAttributeData& OldMaxEnergy)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, MaxEnergy, OldMaxEnergy);
}

namespace
{
	struct FCombatDamageCaptures
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(AttackPower);
		DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);
		FCombatDamageCaptures()
		{
			// 攻击加成在创建效果时快照；护甲在效果实际命中时读取目标聚合值。
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerCombatAttributes, AttackPower, Source, true);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UmultiplayerCombatAttributes, Armor, Target, false);
		}
	};
	const FCombatDamageCaptures& DamageCaptures() { static const FCombatDamageCaptures Value; return Value; }
}

UmultiplayerCombatDamageExecution::UmultiplayerCombatDamageExecution()
{
	RelevantAttributesToCapture.Add(DamageCaptures().AttackPowerDef);
	RelevantAttributesToCapture.Add(DamageCaptures().ArmorDef);
}

void UmultiplayerCombatDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
	FGameplayEffectCustomExecutionOutput& Output) const
{
	const FGameplayEffectSpec& Spec = Parameters.GetOwningSpec();
	FAggregatorEvaluateParameters Evaluation;
	Evaluation.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	Evaluation.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();
	float Attack = 0, Armor = 0;
	Parameters.AttemptCalculateCapturedAttributeMagnitude(DamageCaptures().AttackPowerDef, Evaluation, Attack);
	Parameters.AttemptCalculateCapturedAttributeMagnitude(DamageCaptures().ArmorDef, Evaluation, Armor);
	const float Damage = FMath::Max(0.f, Spec.GetSetByCallerMagnitude(CombatTags::Data_Damage, false) + Attack - FMath::Max(0.f, Armor));
	if (FMath::IsFinite(Damage) && Damage > 0)
		Output.AddOutputModifier(FGameplayModifierEvaluatedData(UmultiplayerCombatAttributes::GetIncomingDamageAttribute(), EGameplayModOp::Additive, Damage));
}
