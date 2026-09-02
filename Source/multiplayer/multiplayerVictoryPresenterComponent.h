// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;

/**
 * 将复制到本地的胜利状态转交给所属 PlayerController 的蓝图表现。
 *
 * GameState 负责提供网络状态，PlayerController 负责本地 UI，本组件只做二者之间的生命周期桥接。
 * 它会处理 GameState 可能尚未创建、绑定晚于状态复制、重新绑定以及重复胜利通知等情况。
 * 组件本身不会创建 Widget；当前项目只有蓝图事件入口，最终是否出现胜利界面取决于派生蓝图
 * 是否实现 ReceiveCoopGameWon，因此不能仅凭该组件声称完整胜利 UI 已经落地。
 *
 * (*) 表现组件只在本地玩家控制器上工作，不复制，也不参与胜负判定。
 * (**) Listen Server 的本地玩家也要走同一表现路径；不能只依赖远端客户端的 RepNotify。
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerVictoryPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UmultiplayerVictoryPresenterComponent();

	/**
	 * 重新确认本地所有权，并绑定当前 GameState。
	 * (**) 监听绑定可能晚于状态复制，所以绑定后必须立即读取一次当前状态，
	 * 不能只等待下一次 Delegate。
	 */
	void RefreshBinding();

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UFUNCTION()
	void HandleGameWon();

private:
	// 对称移除当前 GameState Delegate；切图或 EndPlay 时必须先解绑外部对象。
	void ClearBinding();

	// 仅缓存当前绑定目标以便解绑，不拥有 GameState，也不用于规则判定。
	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	// (**) RepNotify 或重新绑定可能重复通知；本地标记避免胜利界面弹出多次。
	bool bVictoryNotified = false;
};
