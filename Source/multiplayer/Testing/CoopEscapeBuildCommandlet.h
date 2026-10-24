#pragma once
// 离线生成合作闯关地图副本，复用现有 GAS、敌人和机关类型。
// 工具显式保存副本，不在运行时重排用户地图，也不重建 HUD。
#include "Commandlets/Commandlet.h"
#include "CoopEscapeBuildCommandlet.generated.h"

// 显式离线构建可玩关卡副本；绝不保存原 Egypt 地图或重建已有 HUD。
UCLASS()
class UCoopEscapeBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	// 注册编辑器构建工具环境，不启动客户端或服务器游戏。
	UCoopEscapeBuildCommandlet();
	// 验证地图与资产依赖后配置可玩副本，构建失败通过退出码向脚本报告。
	UFUNCTION(BlueprintCallable, Category = "CombatAssets")
	virtual int32 Main(const FString& Params) override;
};
