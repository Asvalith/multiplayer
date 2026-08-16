// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerGameplayTags.h"

namespace MultiplayerGameplayTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(InputTag_Ability_Damage, "InputTag.Ability.Damage", "Activates the damage demo ability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(InputTag_Ability_Heal, "InputTag.Ability.Heal", "Activates the heal demo ability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(InputTag_Ability_Immunity, "InputTag.Ability.Immunity", "Activates the immunity demo ability.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Damage, "Ability.Damage", "Damage ability identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Heal, "Ability.Heal", "Heal ability identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Immunity, "Ability.Immunity", "Immunity ability identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Ability_Guardian_Melee, "Ability.Guardian.Melee", "Server-only Guardian melee ability identity.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Immune, "State.Immune", "The actor is immune to negative demo effects.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Dead", "The actor has no health remaining.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Vulnerable, "State.Vulnerable", "Stacking incoming-damage vulnerability.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Guardian_Shielded, "State.Guardian.Shielded", "Guardian shield is currently blocking negative effects.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Guardian_Channeling, "State.Guardian.Channeling", "At least one valid player is channeling the Guardian objective.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Guardian_AI_Windup, "State.Guardian.AI.Windup", "Guardian is in its server-timed attack telegraph window.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Guardian_AI_Attacking, "State.Guardian.AI.Attacking", "Guardian is committing its server-only GAS attack.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Debug_ForceReject_Immunity, "Debug.Prediction.ForceReject.Immunity", "Non-replicated server-only tag used by the non-Shipping prediction rejection lab.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Team_Player, "Team.Player", "Shared team identity for cooperative players.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Team_Enemy, "Team.Enemy", "Hostile target identity for cooperative combat.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Negative, "Effect.Negative", "Root tag for negative effects.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Negative_Damage, "Effect.Negative.Damage", "Damage effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Negative_Vulnerability, "Effect.Negative.Vulnerability", "Stacking vulnerability effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Positive_Heal, "Effect.Positive.Heal", "Healing effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Positive_Immunity, "Effect.Positive.Immunity", "Immunity effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Positive_GuardianShield, "Effect.Positive.GuardianShield", "Persistent Guardian shield effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Cost_Immunity, "Effect.Cost.Immunity", "Immunity energy cost identity used by the prediction rollback evidence lab.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Debug_PredictionPending, "Effect.Debug.PredictionPending", "Reversible predicted effect identity used only by the M6 evidence lab.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Damage", "SetByCaller damage magnitude.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Heal, "Data.Heal", "SetByCaller healing magnitude.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Damage, "Cooldown.Ability.Damage", "Damage ability cooldown.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Heal, "Cooldown.Ability.Heal", "Heal ability cooldown.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Immunity, "Cooldown.Ability.Immunity", "Immunity ability cooldown.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Damage_Cast, "GameplayCue.Coop.Damage.Cast", "Predicted damage cast presentation on the source avatar.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Damage_Impact, "GameplayCue.Coop.Damage.Impact", "Server-confirmed damage impact presentation on the target avatar.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Heal_Cast, "GameplayCue.Coop.Heal.Cast", "Predicted heal cast presentation on the source avatar.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Heal_Result, "GameplayCue.Coop.Heal.Result", "Server-confirmed healing presentation on the target avatar.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_State_Immunity, "GameplayCue.Coop.State.Immunity", "Predicted and reconciled immunity lifecycle presentation.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_State_Vulnerability, "GameplayCue.Coop.State.Vulnerability", "Replicated vulnerability lifecycle presentation.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Death, "GameplayCue.Coop.Death", "Server-confirmed death presentation.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Prediction_Pending, "GameplayCue.Coop.Prediction.Pending", "Persistent prediction-lab presentation removed by acceptance timeout or PredictionKey rejection.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Guardian_State_Shield, "GameplayCue.Coop.Guardian.State.Shield", "Guardian shield lifecycle presentation owned by its persistent authority GE.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(GameplayCue_Coop_Guardian_Telegraph, "GameplayCue.Coop.Guardian.Telegraph", "Server-emitted Guardian windup presentation; never drives damage timing.");
}
