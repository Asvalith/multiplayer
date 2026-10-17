// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Core/multiplayerLog.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Testing/CoopNetTestDriver.h"
// 日志分类只在一个实现文件中定义；各模块引用头文件声明，方便按 LogMultiplayer 筛选双端记录。
DEFINE_LOG_CATEGORY(LogMultiplayer);

//测试安全性
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
