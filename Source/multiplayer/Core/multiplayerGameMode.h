// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
#include "multiplayerGameMode.generated.h"

/**
 * 合作玩法的服务器规则入口。
 *
 * GameMode 只存在于服务器，复核插槽和胜利区域上报的进度、人数与胜利状态，防止重复结算。
 * 校验后的结果写入可复制的 CoopGameState，客户端 UI 消费其权威快照。
 */
UCLASS(minimalapi)
class AmultiplayerGameMode : public AGameMode
{
	GENERATED_BODY()

public:
	// 构造与引擎初始化。
	AmultiplayerGameMode();

	// 在接收玩家前设置实际登录容量；地图直接启动或重开时同样使用 JSON 人数上限。
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

	// 合作目标的权威提交入口。
	/**
	 * 校验进度后，同步执行插槽提供的钥匙操作；操作成功才增加进度并通知外部。
	 *
	 * @return 本次是否真正增加了进度。重入、状态缺失、目标已完成或游戏已胜利时返回 false。
	 * CommitKey 不会被保存或异步执行；返回 false 必须保留钥匙，不能先消耗再报告失败。
	 */
	bool RegisterActivatedKey(TFunctionRef<bool()> CommitKey);

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

	// 当前回合重开与失败恢复。
	/**
	 * 仅在比赛已经胜利时接受一次重开请求，并使用 AGameMode::RestartGame 重新加载当前 URL。
	 * 这不会选择或自动切换到另一张玩法地图。
	 */
	bool RequestRestartCurrentRound(AController* RequestingController);

	// Travel 失败且本 GameMode/World 仍可用时解除本次重开锁；成功切图由新 World 初始化。
	bool RecoverFailedRestart(const FString& FailureReason);

protected:
	virtual void BeginPlay() override;

private:
	// 钥匙附着、销毁和进度通知可能同步触发其他事件，拒绝提交期间的嵌套登记。
	bool bRegisteringKey = false;

	/** 同一次重开的回滚资料成组保存和清空，不允许遗漏某一个恢复字段。 */
	struct FPendingRestart
	{
		bool bRequested = false;
		FName PreviousMatchState = NAME_None;
		float PreviousSwitchCountdown = 0.0f;
		FString TravelURL;
		TWeakObjectPtr<AController> Requester;
	};

	// 多个客户端可能同时点击重开；服务器只接受本局第一个有效请求。
	FPendingRestart PendingRestart;
};
