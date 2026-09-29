using UnrealBuildTool;

/** 独立专用服务器目标；需使用支持 Server Target 的源码构建引擎。 */
public class multiplayerServerTarget : TargetRules
{
	public multiplayerServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_5;
		ExtraModuleNames.Add("multiplayer");
	}
}
