// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayer.h"
#include "Core/multiplayerLog.h"
#include "Modules/ModuleManager.h"
#if !UE_BUILD_SHIPPING
#include "Misc/CoreDelegates.h"
#include "Testing/CoopNetTestDriver.h"
#endif

// 模块入口只注册统一日志，并按测试驱动自身开关启动开发期联机验证。
// 不持有玩法状态；测试订阅必须在 UObject 清理之前释放，正式规则留在各运行时模块。

// 日志分类只在一个实现文件中定义；各模块引用头文件声明，方便按 LogMultiplayer 筛选双端记录。
DEFINE_LOG_CATEGORY(LogMultiplayer);

// 对称管理开发测试驱动与引擎退出订阅，不替代 GameInstance 的连接生命周期。
class FMultiplayerModule : public FDefaultGameModuleImpl
{
public:
	// 仅开发构建安装实验入口；未传实验参数时不创建测试对象，Shipping 不注册测试退出钩子。
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
#if !UE_BUILD_SHIPPING
		// UObject 系统清理早于模块卸载；测试驱动必须在对象仍存活时移除 Tick、委托和 Root。
		TestShutdownHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&StopCoopNetTests);
		StartCoopNetTests();
#endif
	}
	// 正常退出或运行中卸载均解除订阅并幂等停止测试，避免遗留根引用和 Tick。
	virtual void ShutdownModule() override
	{
#if !UE_BUILD_SHIPPING
		FCoreDelegates::OnEnginePreExit.Remove(TestShutdownHandle);
		// 同时覆盖运行中卸载模块；正常退出时 Stop 已执行，这里可安全重复调用。
		StopCoopNetTests();
#endif
		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
#if !UE_BUILD_SHIPPING
	FDelegateHandle TestShutdownHandle;
#endif
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMultiplayerModule, multiplayer, "multiplayer");
