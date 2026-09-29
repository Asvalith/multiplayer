// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "multiplayerVictoryPresenterComponent.generated.h"

class AmultiplayerCoopGameState;
class AGameStateBase;
class UWorld;

/**
 * 将复制到本地的胜利状态转交给所属 PlayerController 的本地表现。
 *
 * GameState 负责提供网络状态，PlayerController 负责本地 UI，本组件只做二者之间的生命周期桥接。
 * 它会处理 GameState 可能尚未创建、绑定晚于状态复制、重新绑定以及重复胜利通知等情况。
 * 组件本身不创建 Widget，而是调用控制器的统一展示入口；控制器提供可直接使用的 C++ 默认界面，
 * 蓝图事件只作为可选表现扩展。
 *
 * (*) 表现组件只在本地玩家控制器上工作，不复制，也不参与胜负判定。
 * (**) 只为所属客户端展示；绑定后补读当前快照，不能只等下一次 RepNotify。
 */
UCLASS(ClassGroup = (Coop))
class MULTIPLAYER_API UmultiplayerVictoryPresenterComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	/** 关闭 Tick 与组件复制，表现更新完全由本地事件驱动。 */
	UmultiplayerVictoryPresenterComponent();

	/**
	 * 重新确认本地所有权，并绑定当前 GameState。
	 * (**) 监听绑定可能晚于状态复制，所以绑定后必须立即读取一次当前状态，
	 * 不能只等待下一次 Delegate。
	 */
	void RefreshBinding();

protected:
	/** 退出组件时解除 GameState 和 World 两类外部订阅，重置本地通知标记。 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 收到胜利通知或补读到已胜利状态时调用；仅向所属本地控制器转发一次。 */
	UFUNCTION()
	void HandleGameWon();

private:
	// GameState 尚未设置时订阅当前 World 的一次就绪事件；不启用 Tick 或轮询。
	void BindGameStateSetEvent(UWorld* World);
	// UWorld 是外部生命周期对象，成功绑定 GameState 或 EndPlay 时必须对称解绑。
	void ClearGameStateSetEvent();
	/** World 报告 GameState 设置完成后校验类型，再复用 RefreshBinding 完成绑定与状态补读。 */
	void HandleGameStateSet(AGameStateBase* GameState);

	// 对称移除当前 GameState Delegate；切图或 EndPlay 时必须先解绑外部对象。
	void ClearBinding();

	// UPROPERTY/TObjectPtr 是 GC 可追踪引用，用于保存解绑目标；GameState 的生成和销毁由 World 管理。
	UPROPERTY()
	TObjectPtr<AmultiplayerCoopGameState> CoopGameState;

	// 弱引用避免表现组件反向延长 World 生命周期；DelegateHandle 用于精确解绑本次等待。
	TWeakObjectPtr<UWorld> GameStateEventWorld;
	FDelegateHandle GameStateSetEventHandle;

	// (**) RepNotify 或重新绑定可能重复通知；本地标记避免胜利界面弹出多次。
	bool bVictoryNotified = false;
};
