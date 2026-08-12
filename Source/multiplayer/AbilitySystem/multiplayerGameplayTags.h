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

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Negative);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Negative_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Positive_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Positive_Immunity);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Heal);

	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Damage);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Heal);
	UE_DECLARE_GAMEPLAY_TAG_EXTERN(Cooldown_Ability_Immunity);
}
