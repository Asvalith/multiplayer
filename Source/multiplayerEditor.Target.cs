// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

/** 编辑器目标：将相同游戏模块加载到 UnrealEditor，供关卡编辑、蓝图配置和开发测试使用。 */
public class multiplayerEditorTarget : TargetRules
{
	public multiplayerEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		// 与游戏目标保持同一引擎版本约定，避免编辑器能编译而游戏目标依赖另一套头文件顺序。
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("multiplayer");
	}
}
