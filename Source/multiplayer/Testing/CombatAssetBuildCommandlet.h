#pragma once

#include "Commandlets/Commandlet.h"
#include "CombatAssetBuildCommandlet.generated.h"

/** 离线编辑器工具：生成独立战斗表现资产，不修改地图、原角色或原骨架。 */
UCLASS()
class UCombatAssetBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UCombatAssetBuildCommandlet();
	UFUNCTION(BlueprintCallable, Category = "CombatAssets")
	virtual int32 Main(const FString& Params) override;
};
