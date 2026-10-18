// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatConfig.h"

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
