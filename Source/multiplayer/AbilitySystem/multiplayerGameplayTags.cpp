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

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Immune, "State.Immune", "The actor is immune to negative demo effects.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(State_Dead, "State.Dead", "The actor has no health remaining.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Negative, "Effect.Negative", "Root tag for negative effects.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Negative_Damage, "Effect.Negative.Damage", "Damage effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Positive_Heal, "Effect.Positive.Heal", "Healing effect identity.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Effect_Positive_Immunity, "Effect.Positive.Immunity", "Immunity effect identity.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Damage, "Data.Damage", "SetByCaller damage magnitude.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Data_Heal, "Data.Heal", "SetByCaller healing magnitude.");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Damage, "Cooldown.Ability.Damage", "Damage ability cooldown.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Heal, "Cooldown.Ability.Heal", "Heal ability cooldown.");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Cooldown_Ability_Immunity, "Cooldown.Ability.Immunity", "Immunity ability cooldown.");
}
