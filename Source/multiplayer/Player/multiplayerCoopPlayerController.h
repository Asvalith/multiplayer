// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopPlayerController.generated.h"

/** 本地操作反馈，不经过网络；HUD/Widget 订阅后更新按钮和提示。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FCoopVictoryActionChanged, bool, bActionsEnabled, const FText&, Message);

/** 互斥的本地界面操作；不复制，服务器规则仍由 GameMode 决定。 */
UENUM(BlueprintType)
enum class ECoopVictoryAction : uint8
{
	// 可接受新操作，包括上一次失败后重试。
	Idle,
	// 已发送请求，等待失败回执或新 World 替换；不是“已经重开成功”。
	RestartPending,
	// 正在断开连接并返回菜单，失败时恢复 Idle。
	Leaving
};

/**
 * [职责] 所属连接的握手、重开与退出入口；服务器规则仍由 GameMode 决定。
 * 重开经 RPC 交给 GameMode，断开连接交给 GameInstance，操作反馈通过本地委托交给 UI。
 * 不订阅胜利结果、不创建界面、不切换鼠标焦点；这些由本地 HUD 负责。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	// 只表示本控制器已经过服务器接纳，不把进入 PlayingState 当作恢复完成。
	bool HasAcceptedCombatSession() const { return bCombatSessionAccepted; }
	// [边界] 所属客户端只提交票据和本机操作编号；服务器校验与恢复，不接受客户端资源或位置。
	UFUNCTION(Server, Reliable)
	void ServerEstablishCombatSession(FGuid Ticket, uint64 Operation);

	// 对外操作与状态查询。
	/** 请求服务器重开；GameMode 再次检查胜利状态与重复请求。 */
	UFUNCTION(BlueprintCallable, Category = "Coop|Flow")
	void RequestRestartCurrentRound();

	/** 主动退出当前连接并返回菜单；保留蓝图入口名，先关闭自动重连。 */
	UFUNCTION(BlueprintCallable, Category = "Coop|Flow")
	void LeaveCoopSession();

	/** 只读观察当前操作，不允许外部跳过请求和失败恢复路径修改状态。 */
	UFUNCTION(BlueprintPure, Category = "Coop|Flow")
	ECoopVictoryAction GetVictoryAction() const { return VictoryAction; }

	// 所属客户端的按钮与提示反馈；这是本地委托，不是网络广播。
	UPROPERTY(BlueprintAssignable, Category = "Coop|Flow")
	FCoopVictoryActionChanged OnVictoryActionChanged;

	/** GameMode 在旧 World 内恢复失败时，通知实际提交请求的所属客户端。 */
	void NotifyRestartFailed(const FText& Reason);

	// 蓝图表现扩展。
	// HUD 已展示 UMG 胜利界面；该事件仅用于附加动画、音效，不再创建第二份 Widget。
	UFUNCTION(BlueprintImplementableEvent, BlueprintCallable, Category = "Coop|Victory", meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

protected:
	// 保留死亡后 Enter 重开入口，具体终局资格仍由服务器复核。
	virtual void SetupInputComponent() override;
	// 控制器生命周期与引擎通知。
	/** 实例进入 World 时订阅退出失败回执，与 EndPlay 对称。 */
	virtual void BeginPlay() override;
	/** 控制器退出当前 World 时解除请求回执订阅；界面清理由 HUD 负责。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** 进入 Playing 时发起所属连接握手，服务器接纳回执才确认就绪。 */
	virtual void BeginPlayingState() override;
	// 基类销毁/解绑 Pawn 之前保存个人状态，防止 Logout 时已无 Pawn 可读。
	virtual void Destroyed() override;
	// 覆盖常规玩家离开时的 Pawn 销毁入口，复用同一幂等保存流程。
	virtual void PawnLeavingGame() override;
	/** 接收服务器的返回菜单通知，转入项目自己的退出入口。 */
	virtual void ClientReturnToMainMenuWithTextReason_Implementation(
		const FText& ReturnReason) override;

	// 所属客户端与服务器之间的操作请求及回执。
	/** 低频的重开请求通过可靠 RPC 发给服务器；是否允许重开仍由 GameMode 检查。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestRestartCurrentRound();

	/** 只回复所属客户端；收到失败前不接受新的重开，成功由新 World 接管。 */
	UFUNCTION(Client, Reliable)
	void ClientRestartFailed(const FText& Reason);

private:
	friend class AmultiplayerGameMode;
#if WITH_DEV_AUTOMATION_TESTS
	friend class FCoopFlowRequestClosureTest;
	friend class FCoopResumeAdmissionResultTest;
#endif
	// 可靠回执只发所属连接；客户端按 Controller、World 与操作编号过滤旧结果。
	UFUNCTION(Client, Reliable)
	void ClientCombatSessionResult(FGuid Ticket, uint64 Operation, const FText& Error, bool bRetryable);
	// 正常服务器切图清旧回合票据；失败恢复时服务器可重新下发仍然有效的本局票据。
	UFUNCTION(Client, Reliable)
	void ClientSetReconnectTicket(FGuid Ticket);
	FGuid ServerCombatTicket;
	uint64 ClientHandshakeOperation = 0;
	FTimerHandle CombatHandshakeTimer;
	bool bCombatSessionAccepted = false;
	bool bCombatHandshakeSubmitted = false;

	// 本地操作状态统一从这里更新，界面只接收反馈，不维护第二份请求锁。
	void SetVictoryAction(ECoopVictoryAction NewAction, const FText& Message);

	// 外部状态变化回调。
	// 由 GameInstance 的本地 OnLeaveFailed 委托触发，只解除退出操作，不恢复旧连接。
	void HandleLeaveFailed(const FText& Reason);

	// 所属客户端：本地操作状态不复制，不决定服务器规则。
	ECoopVictoryAction VictoryAction = ECoopVictoryAction::Idle;

	// 服务端：仅管理该玩家的在途重开请求；GameMode 另管整局是否已安排重开。
	// 单请求在途，失败回执只发一次；当前没有超时取消，不需要另维护请求编号。
	bool bServerRestartPending = false;
};
