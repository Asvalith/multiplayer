#pragma once
// 编辑器离线资产构建入口；生成独立战斗表现，不参与运行时技能执行。
// 所有保存由显式命令触发，原地图、原骨架和第三方源资产保持不变。

#include "Commandlets/Commandlet.h"
#include "CombatAssetBuildCommandlet.generated.h"

/** 离线编辑器工具：生成独立战斗表现资产，不修改地图、原角色或原骨架。 */
UCLASS()
class UCombatAssetBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	// 将工具配置为编辑器命令程序，不启用客户端或服务器玩法。
	UCombatAssetBuildCommandlet();
	// 验证输入资产、生成独立表现资产并保存；失败返回非零供脚本中止。
	UFUNCTION(BlueprintCallable, Category = "CombatAssets")
	virtual int32 Main(const FString& Params) override;
};
