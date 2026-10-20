// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatTags.h"

// 旧资产未序列化关系字段时也继承这些默认值；不重写地图配置、数值或表现资源。
UmultiplayerCombatConfig::UmultiplayerCombatConfig()
{
	const auto Add = [this](FGameplayTag AbilityTag) -> FmultiplayerAbilityTagRelationship&
	{
		auto& Relationship = AbilityRelationships.AddDefaulted_GetRef();
		Relationship.AbilityTag = AbilityTag;
		return Relationship;
	};
	auto& Common = Add(CombatTags::Ability_Combat);
	for (FGameplayTag Tag : {CombatTags::State_Dead.GetTag(), CombatTags::State_Downed.GetTag(),
		CombatTags::State_Stunned.GetTag(), CombatTags::State_Casting.GetTag(), CombatTags::State_Rescuing.GetTag()})
		Common.ActivationBlockedTags.AddTag(Tag);
	Add(CombatTags::Ability_Defend).ActivationBlockedTags.AddTag(CombatTags::State_Defending);
	auto& DashRelationship = Add(CombatTags::Ability_Dash);
	for (FGameplayTag Tag : {CombatTags::State_Dashing.GetTag(), CombatTags::State_MovementBlocked.GetTag(), CombatTags::State_Rooted.GetTag()})
		DashRelationship.ActivationBlockedTags.AddTag(Tag);
	for (FGameplayTag Tag : {CombatTags::Ability_Heal.GetTag(), CombatTags::Ability_Area.GetTag()})
		Add(Tag).AbilityTagsToCancel.AddTag(CombatTags::Ability_Attack);
	auto& Rescue = Add(CombatTags::Ability_Rescue);
	for (FGameplayTag Tag : {CombatTags::State_Dashing.GetTag(), CombatTags::State_MovementBlocked.GetTag(), CombatTags::State_Rooted.GetTag()})
		Rescue.ActivationBlockedTags.AddTag(Tag);
	// 救援引导替换尚未出手的普攻；阻断由 GAS 计数，结束时不会清掉另一来源的阻断。
	Rescue.AbilityTagsToCancel.AddTag(CombatTags::Ability_Attack);
	Rescue.AbilityTagsToBlock.AddTag(CombatTags::Ability_Combat);
}

// 父类规则与精确技能规则共同生效；只追加，保留调用者已有的原生/蓝图约束。
void UmultiplayerCombatConfig::GetAbilityActivationTagRequirements(const FGameplayTagContainer& AbilityTags,
	FGameplayTagContainer& OutRequired, FGameplayTagContainer& OutBlocked) const
{
	for (const auto& Relationship : AbilityRelationships)
		if (AbilityTags.HasTag(Relationship.AbilityTag))
		{
			OutRequired.AppendTags(Relationship.ActivationRequiredTags);
			OutBlocked.AppendTags(Relationship.ActivationBlockedTags);
		}
}

// 激活与结束使用同一映射，只把成对增减交给 ASC，不另存 bool 或手动清空阻断容器。
void UmultiplayerCombatConfig::GetAbilityBlockAndCancelTags(const FGameplayTagContainer& AbilityTags,
	FGameplayTagContainer& OutBlock, FGameplayTagContainer& OutCancel) const
{
	for (const auto& Relationship : AbilityRelationships)
		if (AbilityTags.HasTag(Relationship.AbilityTag))
		{
			OutBlock.AppendTags(Relationship.AbilityTagsToBlock);
			OutCancel.AppendTags(Relationship.AbilityTagsToCancel);
		}
}

const FmultiplayerCombatSkillRule& UmultiplayerCombatConfig::GetSkill(EMultiplayerCombatSkill Skill) const
{
	switch (Skill)
	{
	case EMultiplayerCombatSkill::Defend: return Defend;
	case EMultiplayerCombatSkill::Heal: return Heal;
	case EMultiplayerCombatSkill::Area: return Area;
	case EMultiplayerCombatSkill::Dash: return Dash;
	default: return Attack;
	}
}

void UmultiplayerCombatConfig::NormalizeRules()
{
	for (FmultiplayerCombatSkillRule* Rule : {&Attack, &Defend, &Heal, &Area, &Dash})
	{
		Rule->Cost = FMath::Clamp(Rule->Cost, 0.f, 100.f);
		Rule->Cooldown = FMath::Clamp(Rule->Cooldown, .05f, 120.f);
		Rule->Amount = FMath::Clamp(Rule->Amount, 0.f, 1000.f);
		Rule->Range = FMath::Clamp(Rule->Range, 0.f, 3000.f);
		Rule->Duration = FMath::Clamp(Rule->Duration, 0.f, 30.f);
		Rule->Windup = FMath::Clamp(Rule->Windup, .05f, 10.f);
		Rule->Recovery = FMath::Clamp(Rule->Recovery, .05f, 5.f);
	}
	AreaRadius = FMath::Clamp(AreaRadius, 10.f, 1000.f);
	AreaEffectReferenceRadius = FMath::Clamp(AreaEffectReferenceRadius, 10.f, 5000.f);
	ProjectileSpeed = FMath::Clamp(ProjectileSpeed, 100.f, 10000.f);
	DefenseDamageMultiplier = FMath::Clamp(DefenseDamageMultiplier, 0.f, 1.f);
	DefenseShieldAmount = FMath::Clamp(DefenseShieldAmount, 0.f, 1000.f);
	BurningDamagePerTick = FMath::Clamp(BurningDamagePerTick, 0.f, 1000.f);
	BurningDuration = FMath::Clamp(BurningDuration, .05f, 120.f);
	AreaRootDuration = FMath::Clamp(AreaRootDuration, 0.f, 120.f);
	Dash.Duration = FMath::Clamp(Dash.Duration, .05f, 2.f);
	ParryWindow = FMath::Clamp(ParryWindow, .05f, 1.f);
	ParryFrontDot = FMath::Clamp(ParryFrontDot, -1.f, 1.f);
	MaxProjectileReflections = FMath::Clamp(MaxProjectileReflections, 0, 8);
	BleedoutDuration = FMath::Clamp(BleedoutDuration, 1.f, 120.f);
	RescueDuration = FMath::Clamp(RescueDuration, .1f, 10.f);
	RescueRange = FMath::Clamp(RescueRange, 50.f, 500.f);
}

void UmultiplayerCombatConfig::PostLoad() { Super::PostLoad(); NormalizeRules(); }
#if WITH_EDITOR
void UmultiplayerCombatConfig::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	NormalizeRules();
}
#endif
