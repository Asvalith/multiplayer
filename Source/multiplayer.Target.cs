// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/** 独立游戏目标：供 Development/Shipping 等游戏构建使用；具体配置由构建命令选择。 */
public class multiplayerTarget : TargetRules
{
	public multiplayerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		// 固定 UE5.5 的构建与头文件顺序约定，使本地编译和打包使用一致规则。
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("multiplayer");
	}
}
