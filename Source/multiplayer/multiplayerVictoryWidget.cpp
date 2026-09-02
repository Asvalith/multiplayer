// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryWidget.h"

#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "multiplayerCoopPlayerController.h"

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
					SNew(SButton)
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
