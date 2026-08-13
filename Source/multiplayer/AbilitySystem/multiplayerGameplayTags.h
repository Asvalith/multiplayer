// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NativeGameplayTags.h"

namespace MultiplayerGameplayTags
{
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Ability_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Ability_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(InputTag_Ability_Immunity);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Ability_Immunity);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Immune);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Dead);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(State_Vulnerable);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Debug_ForceReject_Immunity);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Team_Player);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Team_Enemy);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Negative);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Negative_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Negative_Vulnerability);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Positive_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Positive_Immunity);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Cost_Immunity);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Debug_PredictionPending);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Heal);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Immunity);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Damage_Cast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Damage_Impact);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Heal_Cast);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Heal_Result);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_State_Immunity);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_State_Vulnerability);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Death);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(GameplayCue_Coop_Prediction_Pending);
}
