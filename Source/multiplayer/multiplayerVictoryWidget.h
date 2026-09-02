// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "multiplayerVictoryWidget.generated.h"

/**
 * 无需蓝图即可工作的胜利界面。
 *
 * 共享胜利仍由服务器 GameState 决定；本类只提供本地展示和两个明确操作：重开当前关卡、
 * 主动退出会话。使用 C++ 默认实现后，即使没有制作 UMG 蓝图也能完成最小玩法闭环，
 * 蓝图事件仍可用于替换美术表现。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerVictoryWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	FReply HandleRestartClicked();
	FReply HandleLeaveClicked();

	// 防止连续点击在网络往返期间重复发送重开或退出请求。
	bool bActionSubmitted = false;
};
