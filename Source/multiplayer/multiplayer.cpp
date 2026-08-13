// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "AbilitySystem/multiplayerAbilitySystemGlobals.h"
#include "GameplayAbilitiesDeveloperSettings.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogMultiplayerGAS);

class FmultiplayerGameModule final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();

		UGameplayAbilitiesDeveloperSettings* AbilitySettings =
			GetMutableDefault<UGameplayAbilitiesDeveloperSettings>();
		AbilitySettings->AbilitySystemGlobalsClassName =
			FSoftClassPath(UmultiplayerAbilitySystemGlobals::StaticClass());
		AbilitySettings->GameplayCueNotifyPaths.AddUnique(TEXT("/Game/GAS/GameplayCues"));
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FmultiplayerGameModule, multiplayer, "multiplayer");
