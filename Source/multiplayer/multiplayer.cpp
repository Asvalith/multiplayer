// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Testing/CoopNetTestDriver.h"

class FMultiplayerModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		// UObject 系统清理早于模块卸载；测试驱动必须在对象仍存活时移除 Tick、委托和 Root。
		TestShutdownHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&StopCoopNetTests);
		StartCoopNetTests();
	}
	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnEnginePreExit.Remove(TestShutdownHandle);
		// 同时覆盖运行中卸载模块；正常退出时 Stop 已执行，这里可安全重复调用。
		StopCoopNetTests();
		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	FDelegateHandle TestShutdownHandle;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMultiplayerModule, multiplayer, "multiplayer");
