// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatAttributes.h"

#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

// 每次重新生成角色都会创建新的属性集，不继承上一轮的血量或能量。
UmultiplayerCombatAttributes::UmultiplayerCombatAttributes()
{
	InitHealth(100.0f);
	InitMaxHealth(100.0f);
	InitEnergy(100.0f);
	InitMaxEnergy(100.0f);
	// 默认没有护盾；技能或配置可以通过 GAS 增加，最大值独立于生命上限。
	InitShield(0.0f);
	InitMaxShield(100.0f);
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
	// 队友和敌人可观察护盾，数值只由权威属性及其预测通道产生。
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, Shield, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatAttributes, MaxShield, COND_None, REPNOTIFY_Always);
}

// 限制聚合值；死亡后保持零血量，本版本的治疗不承担复活职责。
void UmultiplayerCombatAttributes::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	ClampAttributeValue(Attribute, NewValue);
}

// SetNumericAttributeBase 和 GE 执行改变基础值时也走相同约束，不只限制最终显示值。
void UmultiplayerCombatAttributes::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const
{
	Super::PreAttributeBaseChange(Attribute, NewValue);
	ClampAttributeValue(Attribute, NewValue);
}

// 持续 GE 移除不会经过 PostGameplayEffectExecute；上限变化在这里统一钳制资源。
// 仅写一次当前资源；回调若再次降低上限，由新一轮通知处理，不用旧上限覆盖新值。
void UmultiplayerCombatAttributes::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
	Super::PostAttributeChange(Attribute, OldValue, NewValue);
	if (NewValue == OldValue) return;
	if (Attribute == GetMaxHealthAttribute() && GetHealth() > NewValue)
		SetHealth(FMath::Clamp(GetHealth(), 0.f, NewValue));
	else if (Attribute == GetMaxEnergyAttribute() && GetEnergy() > NewValue)
		SetEnergy(FMath::Clamp(GetEnergy(), 0.f, NewValue));
	else if (Attribute == GetMaxShieldAttribute() && GetShield() > NewValue)
		SetShield(FMath::Clamp(GetShield(), 0.f, NewValue));
}

// 健康、能量、护盾各自限制在上限内；死亡不通过治疗或补盾复活，攻击和护甲算式保持原样。
void UmultiplayerCombatAttributes::ClampAttributeValue(const FGameplayAttribute& Attribute, float& NewValue) const
{
	if (Attribute == GetHealthAttribute() || Attribute == GetEnergyAttribute() || Attribute == GetShieldAttribute())
	{
		const UAbilitySystemComponent* AbilitySystem = GetOwningAbilitySystemComponent();
		const bool bDead = AbilitySystem && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Dead);
		const float Maximum = Attribute == GetHealthAttribute() ? GetMaxHealth()
			: Attribute == GetShieldAttribute() ? GetMaxShield() : GetMaxEnergy();
		NewValue = !FMath::IsFinite(NewValue) || (bDead && Attribute != GetEnergyAttribute())
			? 0.f : FMath::Clamp(NewValue, 0.f, Maximum);
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		NewValue = FMath::IsFinite(NewValue) ? FMath::Max(1.f, NewValue) : 1.f;
	}
	else if (Attribute == GetMaxEnergyAttribute() || Attribute == GetMaxShieldAttribute())
	{
		NewValue = FMath::IsFinite(NewValue) ? FMath::Max(0.f, NewValue) : 0.f;
	}
}

// 先清空本次伤害再修改血量，血量委托即使触发死亡清理也不会重复消费旧伤害。
void UmultiplayerCombatAttributes::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	const bool bDead = Data.Target.HasMatchingGameplayTag(CombatTags::State_Dead);
	if (Data.EvaluatedData.Attribute == GetIncomingDamageAttribute())
	{
		float Damage = GetIncomingDamage();
		AmultiplayerCombatCharacter* Character = Cast<AmultiplayerCombatCharacter>(Data.Target.GetAvatarActor());
		// 清空伤害也可能通知外部；先固定本次格挡结果，避免同步回调改写本次方向裁决。
		const bool bBlockedDirectDamage = Character && Character->ShouldBlockDirectDamage(Data.EffectSpec);
		SetIncomingDamage(0.0f);
		// 每次外部属性通知回来都读当前资格；不能沿用通知之前的死亡或接纳快照。
		const auto CanReceiveDamage = [&Data, Character]
		{
			return !Data.Target.HasMatchingGameplayTag(CombatTags::State_Dead)
				&& (!Character || (IsValid(Character) && Character->IsCombatReady()));
		};
		if (!FMath::IsFinite(Damage) || Damage <= 0.f || !CanReceiveDamage())
		{
			return;
		}
		if (Character) Character->RecordDamageSource(Cast<AmultiplayerCombatCharacter>(Data.EffectSpec.GetContext().GetOriginalInstigator()));
		// 正面格挡在消耗护盾前结束结算；其余来向与周期伤害仍沿同一条先盾后血路径处理。
		if (bBlockedDirectDamage) return;
		// 先确定本次护盾吸收和溢出，再写盾；补盾或嵌套伤害不会让同一份伤害再次扣盾。
		const float AbsorbedDamage = FMath::Min(GetShield(), Damage);
		const float HealthDamage = Damage - AbsorbedDamage;
		if (AbsorbedDamage > 0.f) SetShield(GetShield() - AbsorbedDamage);
		// 扣盾委托可同步造成新伤害甚至死亡，使用最新血量扣本次溢出，不能写回旧血量快照。
		if (HealthDamage > 0.f && CanReceiveDamage()) SetHealth(GetHealth() - HealthDamage);
		if (GetHealth() <= 0.f && GetShield() > 0.f) SetShield(0.f);
	}
	else if (Data.EvaluatedData.Attribute == GetHealthAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		SetHealth(bDead ? 0.0f : FMath::Clamp(GetHealth(), 0.0f, GetMaxHealth()));
		// 直接生命效果致死也不能留下可见护盾；重新读死亡后状态，不依赖本函数入口时的标签。
		if (GetHealth() <= 0.f && GetShield() > 0.f) SetShield(0.f);
	}
	else if (Data.EvaluatedData.Attribute == GetEnergyAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxEnergyAttribute())
	{
		SetEnergy(FMath::Clamp(GetEnergy(), 0.0f, GetMaxEnergy()));
	}
	else if (Data.EvaluatedData.Attribute == GetShieldAttribute()
		|| Data.EvaluatedData.Attribute == GetMaxShieldAttribute())
	{
		// 修改护盾上限的效果也落实到当前值；Pre 的共享规则继续处理死亡与非法数值。
		SetShield(GetShield());
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

// 同步护盾数值，并让 GAS 处理预测值与服务器值的衔接。
void UmultiplayerCombatAttributes::OnRep_Shield(const FGameplayAttributeData& OldShield)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, Shield, OldShield);
}

// 同步护盾上限，不通过另一个界面状态保存它。
void UmultiplayerCombatAttributes::OnRep_MaxShield(const FGameplayAttributeData& OldMaxShield)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UmultiplayerCombatAttributes, MaxShield, OldMaxShield);
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

// 捕获标签用于 GAS 属性聚合；这里只输出待结算伤害，格挡及盾/血消耗仍由属性集统一提交。
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
