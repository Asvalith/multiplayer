// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopPlayerController.generated.h"

class UmultiplayerVictoryPresenterComponent;
class UmultiplayerVictoryWidget;

/**
 * 承接所属客户端的本地合作 UI。
 *
 * PlayerController 在服务器和它所属的客户端存在，但其他客户端不会拥有这名玩家的
 * PlayerController，因此很适合放“只属于该玩家”的输入和 UI 桥接。本项目把共享胜利结果放在
 * GameState，再由 VictoryPresenter 仅在 IsLocalController() 的实例上转成本地蓝图事件。
 *
 * (*) PlayerController 同时存在于服务器和所属客户端，适合连接复制状态与本地界面。
 * (**) BeginPlayingState 表示控制器已真正进入可操作阶段，比 JoinSession 回调或发出 ClientTravel
 * 更适合作为“客户端连接/重连成功”的最终确认点。
 */
UCLASS()
class MULTIPLAYER_API AmultiplayerCoopPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
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

	/** 主动退出当前会话并返回项目默认主菜单；该路径会先关闭自动重连。 */
	UFUNCTION(BlueprintCallable, Category = "Coop|Flow")
	void LeaveCoopSession();

	// 默认 C++ UI 已能完成流程；该事件只用于项目后续替换动画、音效或美术样式。
	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Victory", meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

protected:
	// 本地进入 PlayingState 后同时确认连接成功，并重新绑定可能刚创建/替换的 GameState。
	virtual void BeginPlayingState() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void ClientReturnToMainMenuWithTextReason_Implementation(
		const FText& ReturnReason) override;

	UFUNCTION(Server, Reliable)
	void ServerRequestRestartCurrentRound();

private:
	void RemoveVictoryScreen();

	// 仅本地使用的表现桥梁，不需要复制，也不参与服务器规则。
	UPROPERTY(VisibleAnywhere, Category = "Coop|Victory")
	TObjectPtr<UmultiplayerVictoryPresenterComponent> VictoryPresenter;

	// 只存在于本地视口，不复制，也不参与胜利规则。
	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerVictoryWidget> VictoryWidget;
};
