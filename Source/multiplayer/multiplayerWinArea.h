// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerWinArea.generated.h"

class AmultiplayerCoopGameState;
class UBoxComponent;
class UmultiplayerPlayerOccupancyComponent;

/**
 * 服务器端的胜利区域：目标完成且区域内人数足够时，请求 GameMode 结束比赛。
 *
 * WinArea 只是事件汇合点：区域人数变化和 GameState 目标变化都会触发 EvaluateWinCondition，
 * 但它没有写 GameState 的权限，最终仍把当前人数交给 GameMode 复核。这样无论玩家先进入区域
 * 还是先完成钥匙目标，最终都能从当前状态重新计算，而不依赖事件历史或固定发生顺序。
 *
 * (*) 触发区域本身无需复制，客户端只需要 GameState 中的最终胜利状态。
 * (**) 玩家可能先进入区域，也可能先完成钥匙目标，因此两种事件都要重新检查；
 * 最终规则仍由 GameMode 复核，避免事件顺序或重复触发导致错误结算。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerWinArea : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerWinArea();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// 参数只表示触发原因；判定时重新读取完整当前状态，避免使用可能已经过期的事件参数。
	UFUNCTION()
	void HandleOccupancyChanged(int32 PlayerCount);

	UFUNCTION()
	void HandleObjectiveProgressChanged(int32 ActivatedKeys, int32 RequiredKeys);

private:
	// 仅服务器执行，并把人数交给 GameMode；重复调用不会绕过 GameMode 的胜利状态检查。
	void EvaluateWinCondition();

	UPROPERTY(VisibleAnywhere, Category = "Coop|Win")
	TObjectPtr<UBoxComponent> WinTrigger;

	UPROPERTY(VisibleAnywhere, Category = "Coop|Win")
	TObjectPtr<UmultiplayerPlayerOccupancyComponent> PlayerOccupancy;

	// 当前关卡默认要求两名不同玩家同时在区域内；由 Occupancy 负责去重。
	UPROPERTY(EditAnywhere, Category = "Coop|Win", meta = (ClampMin = "1"))
	int32 RequiredPlayers = 2;

	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;
};
