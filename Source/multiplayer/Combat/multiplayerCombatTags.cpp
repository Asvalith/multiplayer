// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatTags.h"

// 原生标签随模块注册，无需额外维护配置文件中的同名条目。
namespace CombatTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Attack, "Ability.Combat.Attack", "普通远程攻击");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Defend, "Ability.Combat.Defend", "限时防御");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Heal, "Ability.Combat.Heal", "自身或队友治疗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Area, "Ability.Combat.Area", "可打断的范围施法");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Casting, "State.Combat.Casting", "施法期间禁止移动和其他技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Stunned, "State.Combat.Stunned", "眩晕期间停止行动");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Combat.Dead", "死亡，禁止继续施放技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Defending, "State.Combat.Defending", "防御期间受到一半伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Attack, "Cooldown.Combat.Attack", "普通攻击冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Defend, "Cooldown.Combat.Defend", "防御技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Heal, "Cooldown.Combat.Heal", "治疗技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Area, "Cooldown.Combat.Area", "范围技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Combat.Damage", "服务器填写的本次伤害数值");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Cost, "Data.Combat.Cost", "技能配置中的资源消耗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Heal, "Data.Combat.Heal", "技能配置中的治疗量");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Attack, "GameplayCue.Combat.Attack");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Impact, "GameplayCue.Combat.Impact");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Heal, "GameplayCue.Combat.Heal");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Defense, "GameplayCue.Combat.Defense");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Casting, "GameplayCue.Combat.Casting");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Area, "GameplayCue.Combat.Area");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Stun, "GameplayCue.Combat.Stun");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Failure, "GameplayCue.Combat.Failure");
}
