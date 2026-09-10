// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/*
 * 本模块的编译依赖。公开依赖为模块接口及调用方提供类型/链接信息，私有依赖只服务于内部实现。
 * 构建配置描述“能引用哪些模块”，不负责生成玩法对象或启动联网。
 */
public class multiplayer : ModuleRules
{
	public multiplayer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// 反射与引擎对象、增强输入、会话接口和本地 UI 是当前玩法实际使用的模块。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EnhancedInput",
			"OnlineSubsystem",
			"UMG",
			"Slate",
			"SlateCore"
		});

		// 仅 GameInstance 的实现读取默认菜单地图，公开头文件无需暴露 EngineSettings。
		PrivateDependencyModuleNames.Add("EngineSettings");

		// 面向局域网的具体子系统按配置加载；业务类通过 OnlineSubsystem 接口访问会话。
		DynamicallyLoadedModuleNames.Add("OnlineSubsystemNull");
	}
}
