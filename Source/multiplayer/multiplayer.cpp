// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Modules/ModuleManager.h"
#include "Testing/CoopNetTestDriver.h"

class FMultiplayerModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		StartCoopNetTests();
	}
	virtual void ShutdownModule() override
	{
		StopCoopNetTests();
		FDefaultGameModuleImpl::ShutdownModule();
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMultiplayerModule, multiplayer, "multiplayer");
