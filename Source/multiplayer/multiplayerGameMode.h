// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "multiplayerGameMode.generated.h"

/**
 * 合作玩法的服务器规则入口。
 *
 * GameMode 只存在于服务器，因此适合保存“谁有权修改结果”的规则，而不适合直接驱动客户端 UI。
 * 钥匙插槽和胜利区域只上报已经观察到的事实，最终是否增加进度、是否允许胜利仍由这里复核；
 * 通过校验后的结果再写入可复制的 multiplayerCoopGameState，客户端只消费权威快照。
 *
 * (*) GameMode 与 GameState 的分工：前者负责规则和写入权限，后者负责向所有连接复制共享状态。
 * (**) 服务器权威不等于“调用者一定可信”。即使调用来自服务器 Actor，也要再次检查当前进度和
 * 胜利状态，防止重复 Overlap、重复 Delegate 或同一帧的多个事件造成重复结算。
 */
UCLASS(minimalapi)
class AmultiplayerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AmultiplayerGameMode();

	/**
	 * 登记一个已经通过插槽校验的钥匙目标。
	 *
	 * @return 本次是否真正增加了进度。非权威端、状态缺失、目标已完成或游戏已胜利时返回 false。
	 * 返回值使调用者可以区分“事件到达”和“状态确实发生变化”，避免把重复调用当成成功。
	 */
	bool RegisterActivatedKey();

	/**
	 * 尝试完成合作游戏。
	 *
	 * 胜利区域负责统计当前人数，但 GameMode 会同时复核人数、钥匙目标和既有胜利状态，
	 * 不允许触发区域直接写 bGameWon。
	 *
	 * @param CurrentPlayers 胜利区域当前统计到的不同玩家数量。
	 * @param RequiredPlayers 该区域要求的玩家数量；运行时至少按 1 处理。
	 * @return 仅首次把权威状态推进到胜利时返回 true。
	 */
	bool TryCompleteCoopGame(int32 CurrentPlayers, int32 RequiredPlayers);

protected:
	virtual void BeginPlay() override;

private:
	/**
	 * 以关卡实际摆放的插槽数量作为目标数量。
	 * 这样增加或删除插槽后无需同步修改另一份配置；没有插槽的测试地图才使用回退值。
	 */
	int32 ResolveRequiredKeys() const;

	// 仅供没有摆放插槽的测试地图初始化；正式关卡优先使用 ResolveRequiredKeys 的统计结果。
	int32 RequiredKeys = 4;
};
