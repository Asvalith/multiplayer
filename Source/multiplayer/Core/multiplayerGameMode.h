// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Network/multiplayerCombatResumeState.h"
#include "GameFramework/GameMode.h"
#include "multiplayerGameMode.generated.h"

class AmultiplayerCoopPlayerController;

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
	// GameSession 和 GameState 创建后，统一应用内存中的配置并初始化目标快照。
	virtual void InitGameState() override;
	// 登录后为尚未接纳的控制器安排握手期限。
	virtual void PostLogin(APlayerController* NewPlayer) override;
	// 只为未接纳玩家走引擎出生流程，不提供已接纳角色的免费复活。
	virtual void RestartPlayer(AController* NewPlayer) override;
	// 退出时补存个人快照，再交还引擎清理连接。
	virtual void Logout(AController* Exiting) override;

	// 接纳只校验服务器签发的票据；个人状态始终来自本局服务器快照，不收客户端数值。
	bool ClaimCombatSession(AmultiplayerCoopPlayerController* Controller, const FGuid& Ticket,
		FGuid& OutTicket, FText& OutError, bool& bRetryable);
	// Controller 在销毁 Pawn 前调用；重复的销毁/Logout 通知不会覆盖第一次快照。
	void SaveCombatSession(AmultiplayerCoopPlayerController* Controller);
	// 终局死亡后复核整队；仍有活跃玩家时保留救援机会，不把单人死亡当整局结束。
	void NotifyCombatPlayerDeath();
	// 服务器接纳完成后的通知；场景出生器据此启动，GameMode 不接管刷怪数量、落点或计时。
	FSimpleMulticastDelegate OnCombatSessionAccepted;

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
	 * 仅在胜利或失败终局接受一次重开请求，并使用 AGameMode::RestartGame 重新加载当前 URL。
	 * 这不会选择或自动切换到另一张玩法地图。
	 */
	bool RequestRestartCurrentRound(AController* RequestingController);

	// Travel 失败且本 GameMode/World 仍可用时解除本次重开锁；成功切图由新 World 初始化。
	bool RecoverFailedRestart(const FString& FailureReason);

protected:
	// 跨越引擎 Possess/Restart 过程保持未接纳 Pawn 禁行，接纳后才由握手开放。
	virtual void FinishRestartPlayer(AController* NewPlayer, const FRotator& StartRotation) override;
	// 向客户端发送 Travel 前清旧回合票据，失败恢复时再由原 GameMode 下发。
	virtual APlayerController* ProcessClientTravel(FString& URL, bool bSeamless, bool bAbsolute) override;

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopResumeAdmissionResultTest;
#endif
	// 单个服务器会话：在线弱引用、票据别名和个人快照共享同一接纳锁。
	struct FCombatSession
	{
		TWeakObjectPtr<AmultiplayerCoopPlayerController> Controller;
		FGuid PreviousTicket;
		FmultiplayerCombatResumeState State;
		double SavedAt = 0;
		double ExpiresAt = 0;
	};
	// 仅存本局内存；断线快照保留120秒，当前/备用票据共享一个在线锁，轮换回执丢失也能恢复。
	TMap<FGuid, FCombatSession> CombatSessions;

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
