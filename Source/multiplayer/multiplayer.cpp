// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Core/multiplayerLog.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreDelegates.h"
#include "Testing/CoopNetTestDriver.h"

// 模块入口只注册统一日志，并按测试驱动自身开关启动开发期联机验证。
// 不持有玩法状态；测试订阅必须在 UObject 清理之前释放，正式规则留在各运行时模块。

// 日志分类只在一个实现文件中定义；各模块引用头文件声明，方便按 LogMultiplayer 筛选双端记录。
DEFINE_LOG_CATEGORY(LogMultiplayer);

// 对称管理开发测试驱动与引擎退出订阅，不替代 GameInstance 的连接生命周期。
class FMultiplayerModule : public FDefaultGameModuleImpl
{
public:
	// 模块就绪后安装提前退出钩子，再让测试驱动自行判断命令行是否要求运行。
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		// UObject 系统清理早于模块卸载；测试驱动必须在对象仍存活时移除 Tick、委托和 Root。
		TestShutdownHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&StopCoopNetTests);
		StartCoopNetTests();
	}
	// 正常退出或运行中卸载均解除订阅并幂等停止测试，避免遗留根引用和 Tick。
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
