// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class multiplayer : ModuleRules
{
	public multiplayer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"GameplayAbilities",
			"GameplayTags",
			"GameplayTasks",
			"UMG",
			"OnlineSubsystem",
			"OnlineSubsystemUtils"
		});

		DynamicallyLoadedModuleNames.Add("OnlineSubsystemNull");
	}
}
