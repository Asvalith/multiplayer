// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "multiplayerVictoryWidget.generated.h"

class SButton;
class SWidget;

/**
 * 无需蓝图即可工作的胜利界面。
 *
 * 共享胜利仍由服务器 GameState 决定；本类只提供本地展示和两个明确操作：重开当前关卡、
 * 主动退出会话。使用 C++ 默认实现后，即使没有制作 UMG 蓝图也能完成最小玩法闭环，
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

protected:
	/** 创建标题与两个按钮的 Slate 树，保留重开按钮以提供可靠的初始焦点。 */
	virtual TSharedRef<SWidget> RebuildWidget() override;
	/** 释放本类额外保存的 Slate 引用，再让父类清理底层控件树。 */
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	/** 本地只提交一次重开意图；返回 Handled 表示输入已处理，不表示服务器已同意。 */
	FReply HandleRestartClicked();
	/** 与重开共用提交标记，防止连续点击发起两个互相冲突的操作。 */
	FReply HandleLeaveClicked();

	// 防止连续点击重复发请求；当前界面等待重开/退出销毁，没有失败后解锁按钮的恢复流程。
	bool bActionSubmitted = false;

	// 保存默认操作按钮的 Slate 引用，仅用于设置初始键盘/手柄焦点。
	TSharedPtr<SButton> RestartButton;
};
