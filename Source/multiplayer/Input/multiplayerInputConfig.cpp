// Copyright Epic Games, Inc. All Rights Reserved.

#include "Input/multiplayerInputConfig.h"

#include "InputAction.h"
#include "multiplayer.h"

const UInputAction* UmultiplayerInputConfig::FindAbilityInputActionForTag(
	FGameplayTag InputTag,
	bool bLogNotFound) const
{
	for (const FmultiplayerTaggedInputAction& Entry : AbilityInputActions)
	{
		if (Entry.InputAction != nullptr && Entry.InputTag.MatchesTagExact(InputTag))
		{
			return Entry.InputAction;
		}
	}

	if (bLogNotFound)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("InputConfig %s has no InputAction for tag %s"),
			*GetNameSafe(this),
			*InputTag.ToString());
	}

	return nullptr;
}
