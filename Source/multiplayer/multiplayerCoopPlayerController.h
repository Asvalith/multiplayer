// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopPlayerController.generated.h"

class UmultiplayerVictoryPresenterComponent;

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

	// 本地收到胜利状态后只触发一次；这是表现扩展点，若蓝图未实现就不会自动生成胜利界面。
	UFUNCTION(BlueprintImplementableEvent, Category = "Coop|Victory", meta = (DisplayName = "On Coop Game Won"))
	void ReceiveCoopGameWon();

protected:
	// 本地进入 PlayingState 后同时确认连接成功，并重新绑定可能刚创建/替换的 GameState。
	virtual void BeginPlayingState() override;

private:
	// 仅本地使用的表现桥梁，不需要复制，也不参与服务器规则。
	UPROPERTY(VisibleAnywhere, Category = "Coop|Victory")
	TObjectPtr<UmultiplayerVictoryPresenterComponent> VictoryPresenter;
};
