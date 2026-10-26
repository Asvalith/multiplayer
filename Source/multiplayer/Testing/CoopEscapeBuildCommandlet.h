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
	// 保留编辑器 Python 反射入口；审计与保存模式互斥，非编辑器和失败操作返回非零。
	UFUNCTION(BlueprintCallable, Category = "CombatAssets")
	virtual int32 Main(const FString& Params) override;
};
