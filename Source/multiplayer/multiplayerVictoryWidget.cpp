// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryWidget.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "multiplayerCoopPlayerController.h"

/**
 * 构建最小可用界面：Overlay 居中、Border 提供背景、VerticalBox 排列操作。
 * 使用离散点击事件即可完成流程，无需为血量等不存在的展示数据增加 Tick 或属性查询绑定。
 */
TSharedRef<SWidget> UmultiplayerVictoryWidget::RebuildWidget()
{
	return SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			SNew(SBorder)
			.Padding(FMargin(40.0f, 30.0f))
			.BorderBackgroundColor(FLinearColor(0.02f, 0.03f, 0.05f, 0.92f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.HAlign(HAlign_Center)
				.Padding(0.0f, 0.0f, 0.0f, 20.0f)
				[
					SNew(STextBlock)
					.Text(NSLOCTEXT("Multiplayer", "CoopVictoryTitle", "合作完成"))
					.ColorAndOpacity(FLinearColor::White)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f)
				[
					SAssignNew(RestartButton, SButton)
					.HAlign(HAlign_Center)
					.Text(NSLOCTEXT("Multiplayer", "RestartCurrentRound", "重新开始当前关卡"))
					.OnClicked_UObject(this, &UmultiplayerVictoryWidget::HandleRestartClicked)
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.Text(NSLOCTEXT("Multiplayer", "LeaveCoopSession", "退出房间"))
					.OnClicked_UObject(this, &UmultiplayerVictoryWidget::HandleLeaveClicked)
				]
			]
		];
}

/** SButton 支持键盘焦点；调用者须处理控件树尚未构建或已经释放时的空指针。 */
TSharedPtr<SWidget> UmultiplayerVictoryWidget::GetInitialFocusWidget() const
{
	return RestartButton;
}

/** 与 SAssignNew 保存引用的动作配对，避免控件树移除后仍由成员保持旧按钮存活。 */
void UmultiplayerVictoryWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	RestartButton.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

/** 先占用本地提交标记再进入控制器，避免同步回调或快速点击再次进入操作入口。 */
FReply UmultiplayerVictoryWidget::HandleRestartClicked()
{
	if (!bActionSubmitted)
	{
		bActionSubmitted = true;
		if (AmultiplayerCoopPlayerController* PlayerController =
			Cast<AmultiplayerCoopPlayerController>(GetOwningPlayer()))
		{
			PlayerController->RequestRestartCurrentRound();
		}
	}
	return FReply::Handled();
}

/** 只转发用户意图；会话销毁与网络清理由 GameInstance 完成，Widget 不直接操作连接。 */
FReply UmultiplayerVictoryWidget::HandleLeaveClicked()
{
	if (!bActionSubmitted)
	{
		bActionSubmitted = true;
		if (AmultiplayerCoopPlayerController* PlayerController =
			Cast<AmultiplayerCoopPlayerController>(GetOwningPlayer()))
		{
			PlayerController->LeaveCoopSession();
		}
	}
	return FReply::Handled();
}
