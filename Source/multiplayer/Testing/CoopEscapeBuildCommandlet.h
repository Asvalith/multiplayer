#pragma once
#include "Commandlets/Commandlet.h"
#include "CoopEscapeBuildCommandlet.generated.h"

// 显式离线构建可玩关卡副本；绝不保存原 Egypt 地图或重建已有 HUD。
UCLASS()
class UCoopEscapeBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()
public:
	UCoopEscapeBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
