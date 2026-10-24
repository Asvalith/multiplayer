// Copyright Epic Games, Inc. All Rights Reserved.

// 在模块启动时注册原生标签，并定义技能到分类／事件／冷却的只读对应关系。
// 此文件不授予标签，不判断当前可用性，也不因查询而激活技能。

#include "Combat/multiplayerCombatTags.h"
#include "Combat/multiplayerCombatConfig.h"

// 原生标签随模块注册，无需额外维护配置文件中的同名条目。
namespace CombatTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Combat, "Ability.Combat", "战斗能力的共用关系分类，不代表正在执行");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Rescue, "Ability.Combat.Rescue", "服务器引导救援");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Requirements, "Failure.Combat.Requirements");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Conflict, "Failure.Combat.Conflict");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Failure_MuzzleBlocked, "Failure.Combat.MuzzleBlocked", "实际发射点被场景遮挡");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Attack, "Ability.Combat.Attack", "普通远程攻击");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Defend, "Ability.Combat.Defend", "限时防御");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Heal, "Ability.Combat.Heal", "自身或队友治疗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Area, "Ability.Combat.Area", "可打断的范围施法");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Dash, "Ability.Combat.Dash", "CMC 根运动预测冲刺");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Attack, "Event.Combat.Attack", "携带本次瞄准的普通攻击请求");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Defend, "Event.Combat.Defend", "携带正面防御朝向的技能请求");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Area, "Event.Combat.Area", "携带本次瞄准的范围施法请求");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Event_Dash, "Event.Combat.Dash", "携带水平朝向的预测冲刺请求");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Casting, "State.Combat.Casting", "施法期间禁止移动和其他技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Attacking, "State.Combat.Attacking", "普通攻击期间保留本次出手朝向，不禁止横向移动");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dashing, "State.Combat.Dashing", "本次预测冲刺拥有的临时状态");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Parrying, "State.Combat.Parrying", "防御开始时的短弹反窗口");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Downed, "State.Combat.Downed", "倒地等待救援，禁止战斗和机关占用");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Rescuing, "State.Combat.Rescuing", "正在引导救援，不能同时施放技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Stunned, "State.Combat.Stunned", "眩晕期间停止行动");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_MovementBlocked, "State.Combat.MovementBlocked", "所有来源的禁行效果解除后才恢复移动");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Rooted, "State.Combat.Rooted", "禁锢只阻止移动，不禁止原地施法");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Burning, "State.Combat.Burning", "服务器周期结算的燃烧，同来源刷新、不同来源独立");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Combat.Dead", "死亡，禁止继续施放技能");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Defending, "State.Combat.Defending", "限时正面完全格挡直接攻击，不保护侧后或持续伤害");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Attack, "Cooldown.Combat.Attack", "普通攻击冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Defend, "Cooldown.Combat.Defend", "防御技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Heal, "Cooldown.Combat.Heal", "治疗技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Area, "Cooldown.Combat.Area", "范围技能冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Dash, "Cooldown.Combat.Dash", "冲刺冷却");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Combat.Damage", "服务器填写的本次伤害数值");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Cost, "Data.Combat.Cost", "技能配置中的资源消耗");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Heal, "Data.Combat.Heal", "技能配置中的治疗量");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Shield, "Data.Combat.Shield", "服务器授予的本次护盾量");
	UE_DEFINE_GAMEPLAY_TAG(Failure_NotReady, "Failure.Combat.NotReady");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Dead, "Failure.Combat.Dead");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Downed, "Failure.Combat.Downed");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Rescuing, "Failure.Combat.Rescuing");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Stunned, "Failure.Combat.Stunned");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Casting, "Failure.Combat.Casting");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Dashing, "Failure.Combat.Dashing");
	UE_DEFINE_GAMEPLAY_TAG(Failure_AlreadyActive, "Failure.Combat.AlreadyActive");
	UE_DEFINE_GAMEPLAY_TAG(Failure_NotLearned, "Failure.Combat.NotLearned");
	UE_DEFINE_GAMEPLAY_TAG(Failure_InsufficientEnergy, "Failure.Combat.InsufficientEnergy");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Cooldown, "Failure.Combat.Cooldown");
	UE_DEFINE_GAMEPLAY_TAG(Failure_NoHealTarget, "Failure.Combat.NoHealTarget");
	UE_DEFINE_GAMEPLAY_TAG(Failure_InvalidGround, "Failure.Combat.InvalidGround");
	UE_DEFINE_GAMEPLAY_TAG(Failure_DashBlocked, "Failure.Combat.DashBlocked");
	UE_DEFINE_GAMEPLAY_TAG(Failure_MovementBlocked, "Failure.Combat.MovementBlocked");
	UE_DEFINE_GAMEPLAY_TAG(Failure_InvalidAim, "Failure.Combat.InvalidAim");
	UE_DEFINE_GAMEPLAY_TAG(Failure_Rejected, "Failure.Combat.Rejected");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Attack, "GameplayCue.Combat.Attack");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Impact, "GameplayCue.Combat.Impact");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Heal, "GameplayCue.Combat.Heal");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Defense, "GameplayCue.Combat.Defense");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Casting, "GameplayCue.Combat.Casting");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Area, "GameplayCue.Combat.Area");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Stun, "GameplayCue.Combat.Stun");
	UE_DEFINE_GAMEPLAY_TAG(Cue_Failure, "GameplayCue.Combat.Failure");

	// 返回静态配对，避免输入、准入和冷却各维护一张表；治疗不需要定向事件。
	const FmultiplayerCombatSkillTags& GetSkillTags(EMultiplayerCombatSkill Skill)
	{
		static const TArray<FmultiplayerCombatSkillTags> Tags = {
			{Ability_Attack, Event_Attack, Cooldown_Attack},
			{Ability_Defend, Event_Defend, Cooldown_Defend},
			{Ability_Heal, {}, Cooldown_Heal},
			{Ability_Area, Event_Area, Cooldown_Area},
			{Ability_Dash, Event_Dash, Cooldown_Dash}};
		static const FmultiplayerCombatSkillTags Invalid;
		return Tags.IsValidIndex(int32(Skill)) ? Tags[int32(Skill)] : Invalid;
	}
}
