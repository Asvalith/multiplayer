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

		// 按功能目录组织头文件和实现，模块内 include 统一从模块根目录开始查找。
		PrivateIncludePaths.Add(ModuleDirectory);

		// 反射与引擎对象、增强输入和本地 UI 是当前玩法实际使用的模块。
		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore"
		});

		// 默认菜单、JSON 解析及测试用玩家连接标识只在实现文件中使用。
		PrivateDependencyModuleNames.AddRange(new string[] { "EngineSettings", "Json", "CoreOnline" });
	}
}
