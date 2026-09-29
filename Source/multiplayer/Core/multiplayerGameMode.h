// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameMode.h"
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
 *
 * 所有公开函数只提交服务器规则结果，不直接创建客户端界面；客户端可见状态统一写入
 * CoopGameState，再由属性复制分发。
 */
UCLASS(minimalapi)
class AmultiplayerGameMode : public AGameMode
{
	GENERATED_BODY()

public:
	AmultiplayerGameMode();

	// 在接收玩家前设置实际登录容量；地图直接启动或重开时同样使用 JSON 人数上限。
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;

	/**
	 * 校验进度后，同步执行插槽提供的钥匙操作；操作成功才增加进度并通知外部。
	 *
	 * @return 本次是否真正增加了进度。非权威端、状态缺失、目标已完成或游戏已胜利时返回 false。
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

	/**
	 * 以关卡实际摆放的插槽数量作为目标数量。
	 * 这样增加或删除插槽后无需同步修改另一份配置；没有插槽时使用回退值。
	 */
	int32 ResolveRequiredKeys() const;

	// 无插槽时的回退数量；有插槽的关卡优先使用 ResolveRequiredKeys 的统计结果。
	int32 RequiredKeys = 4;

	// 多个客户端可能同时点击重开；服务器只接受本局第一个有效请求。
	FPendingRestart PendingRestart;
};
