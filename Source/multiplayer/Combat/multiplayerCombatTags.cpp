// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatTags.h"

// 原生标签随模块注册，无需额外维护配置文件中的同名条目。
namespace CombatTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Attack, "Ability.Combat.Attack", "普通远程攻击");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Defend, "Ability.Combat.Defend", "限时防御");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Heal, "Ability.Combat.Heal", "自身或队友治疗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Combat.Dead", "死亡，禁止继续施放技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Defending, "State.Combat.Defending", "防御期间受到一半伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Attack, "Cooldown.Combat.Attack", "普通攻击冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Defend, "Cooldown.Combat.Defend", "防御技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Heal, "Cooldown.Combat.Heal", "治疗技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Combat.Damage", "服务器填写的本次伤害数值");
}
