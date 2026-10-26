#pragma once
// 编辑器离线资产构建、迁移与验证入口；不参与运行时技能执行。
// 更新命令只保存实现中列明的项目包，验证命令不保存；具体迁移范围由所选模式决定。

#include "Commandlets/Commandlet.h"
#include "CombatAssetBuildCommandlet.generated.h"

/** 离线编辑器工具：单次显式选择构建、迁移或只读验证模式。 */
UCLASS()
class UCombatAssetBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	// 将工具配置为编辑器命令程序，不启用客户端或服务器玩法。
	UCombatAssetBuildCommandlet();
	// 保留反射入口供现有编辑器 Python 调用；非编辑器拒绝执行，失败返回非零。
	UFUNCTION(BlueprintCallable, Category = "CombatAssets")
	virtual int32 Main(const FString& Params) override;
};
