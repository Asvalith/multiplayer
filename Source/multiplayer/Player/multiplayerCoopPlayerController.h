// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopPlayerController.generated.h"

class UmultiplayerVictoryPresenterComponent;
class UmultiplayerVictoryWidget;

/** 互斥的本地界面操作；不复制，服务器规则仍由 GameMode 决定。 */
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
 * 承接所属客户端的本地合作 UI。
 *
 * PlayerController 在服务器和它所属的客户端存在，但其他客户端不会拥有这名玩家的
 * PlayerController，因此很适合放“只属于该玩家”的输入和 UI 桥接。本项目把共享胜利结果放在
 * GameState，再由 VictoryPresenter 仅在 IsLocalController() 的实例上调用界面入口。
 *
 * (**) 本项目在 BeginPlayingState 确认客户端进入游戏；这并不保证所有复制 Actor 都已就绪，
 * 所以胜利状态的监听仍需由 Presenter 单独处理 GameState 的到达时序。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	/** 创建本地表现桥接组件；是否执行界面逻辑由组件在运行时检查控制器归属。 */
	AmultiplayerCoopPlayerController();

	/** 显示项目自带的胜利界面，再通知可选蓝图表现扩展。只允许本地控制器调用。 */
	void PresentCoopVictory();

	/**
	 * 请求服务器重新加载当前合作关卡。
	 * (*) 客户端没有改写比赛的权限，因此本地入口只负责发送 Server RPC，GameMode 会再次检查
	 * 当前是否已经胜利以及是否已有重开请求。
	 */
	UFUNCTION(BlueprintCallable, Category = "Coop|Flow")
	void RequestRestartCurrentRound();

	/** 主动退出当前连接并返回菜单；保留蓝图入口名，先关闭自动重连。 */
	UFUNCTION(BlueprintCallable, Category = "Coop|Flow")
	void LeaveCoopSession();

	/** 只读观察当前操作，不允许外部跳过请求和失败恢复路径修改状态。 */
	ECoopVictoryAction GetVictoryAction() const { return VictoryAction; }

	/** GameMode 在旧 World 内恢复失败时，通知实际提交请求的所属客户端。 */
	void NotifyRestartFailed(const FText& Reason);

	// 默认 C++ UI 已能完成流程；该事件只用于项目后续替换动画、音效或美术样式。
	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Victory", meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

protected:
	// 本地进入 PlayingState 后同时确认连接成功，并重新绑定可能刚创建/替换的 GameState。
	virtual void BeginPlayingState() override;
	/** 控制器退出当前 World 时移除视口界面、释放引用并还原输入模式。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** 接收服务器的返回菜单通知，转入项目自己的退出入口。 */
	virtual void ClientReturnToMainMenuWithTextReason_Implementation(
		const FText& ReturnReason) override;

	/** 低频的重开请求通过可靠 RPC 发给服务器；是否允许重开仍由 GameMode 检查。 */
	UFUNCTION(Server, Reliable)
	void ServerRequestRestartCurrentRound();

	/** 只回复所属客户端；收到失败前不接受新的重开，成功由新 World 接管。 */
	UFUNCTION(Client, Reliable)
	void ClientRestartFailed(const FText& Reason);

private:
	/** 清除本地界面与焦点状态；可重复调用，不修改共享胜利结果。 */
	void RemoveVictoryScreen();
	void SetVictoryAction(ECoopVictoryAction NewAction, const FText& Message);
	void HandleLeaveFailed(const FText& Reason);

	ECoopVictoryAction VictoryAction = ECoopVictoryAction::Idle;
	// 单请求在途，失败回执只发一次；当前没有超时取消，不需要另维护请求编号。
	bool bServerRestartPending = false;

	// 仅本地使用的表现桥梁，不需要复制，也不参与服务器规则。
	UPROPERTY(VisibleAnywhere, Category = "Coop|Victory")
	TObjectPtr<UmultiplayerVictoryPresenterComponent> VictoryPresenter;

	// 只存在于本地视口，不复制，也不参与胜利规则。
	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerVictoryWidget> VictoryWidget;
};
