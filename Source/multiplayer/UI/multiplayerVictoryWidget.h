// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "multiplayerVictoryWidget.generated.h"

class SButton;
class STextBlock;
class SWidget;

/**
 * 无需蓝图即可工作的胜利界面。
 *
 * 共享胜利仍由服务器 GameState 决定；本类只提供本地展示和两个明确操作：重开当前关卡、
 * 主动断开连接。使用 C++ 默认实现后，即使没有制作 UMG 蓝图也能完成最小玩法闭环，
 * 控制器上的蓝图事件仍可扩展美术表现。
 *
 * (*) UUserWidget 参与 UObject 生命周期，内部 SWidget 使用 Slate 的共享指针管理；
 * 因此移除界面与释放保存的 Slate 引用是两个需要配对处理的步骤。
 */
UCLASS()
class MULTIPLAYER_API UmultiplayerVictoryWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	// UIOnly 输入模式必须聚焦真正支持键盘焦点的控件，不能把根 Overlay 当作焦点目标。
	TSharedPtr<SWidget> GetInitialFocusWidget() const;

	/** 控制器是操作状态的唯一来源；界面只按事件更新按钮和提示，不维护第二份提交标记。 */
	void SetActionFeedback(bool bActionsEnabled, const FText& Message);

protected:
	/** 创建标题与两个按钮的 Slate 树，保留重开按钮以提供可靠的初始焦点。 */
	virtual TSharedRef<SWidget> RebuildWidget() override;
	/** 释放本类额外保存的 Slate 引用，再让父类清理底层控件树。 */
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	// 测试驱动调用真实按钮处理函数，不新增可供玩法绕过权限的测试入口。
	friend class UCoopNetTestDriver;
	/** 返回 Handled 仅表示输入已处理；权限、重复提交和失败恢复统一交给控制器。 */
	FReply HandleRestartClicked();
	/** 转交退出意图，不直接操作网络连接。 */
	FReply HandleLeaveClicked();

	// Slate 引用用于事件驱动刷新；在 ReleaseSlateResources 中与控件树一起释放。
	TSharedPtr<SButton> RestartButton;
	TSharedPtr<STextBlock> ActionMessage;
};
