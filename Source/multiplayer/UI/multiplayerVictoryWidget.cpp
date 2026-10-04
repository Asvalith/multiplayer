// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/multiplayerVictoryWidget.h"

#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerLog.h"
#include "Engine/World.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Player/multiplayerCoopPlayerController.h"

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
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 12.0f, 0.0f, 0.0f)
				[
					SAssignNew(ActionMessage, STextBlock)
					.ColorAndOpacity(FLinearColor::White)
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
	ActionMessage.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}

/** 只在状态变化时刷新 Slate；空指针检查覆盖控件树尚未构建或已释放的阶段。 */
void UmultiplayerVictoryWidget::SetActionFeedback(bool bActionsEnabled, const FText& Message)
{
	SetIsEnabled(bActionsEnabled);
	if (ActionMessage.IsValid())
	{
		ActionMessage->SetText(Message);
	}
}

/** 控制器先校验本地条件，再锁定本次请求；失败结果沿同一入口恢复界面。 */
FReply UmultiplayerVictoryWidget::HandleRestartClicked()
{
	if (AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwningPlayer()))
	{
		PlayerController->RequestRestartCurrentRound();
	}
	return FReply::Handled();
}

/** 只转发用户意图；断开连接与返回菜单由 GameInstance 完成。 */
FReply UmultiplayerVictoryWidget::HandleLeaveClicked()
{
	if (AmultiplayerCoopPlayerController* PlayerController =
		Cast<AmultiplayerCoopPlayerController>(GetOwningPlayer()))
	{
		PlayerController->LeaveCoopSession();
	}
	return FReply::Handled();
}

void AmultiplayerCoopHUD::BeginPlay()
{
	Super::BeginPlay();
	if (PlayerOwner == nullptr || !PlayerOwner->IsLocalController())
	{
		return;
	}

	//菜单的 UIOnly 修改的是共享视口；换控制器后显式恢复，不能依赖旧菜单析构。
	// HUD 初始化时恢复一次，Controller 再次进入 PlayingState 不会覆盖已有胜利界面的焦点。
	PlayerOwner->bShowMouseCursor = false;
	PlayerOwner->SetInputMode(FInputModeGameOnly());
	UWorld* World = GetWorld();
	RefreshVictoryBinding(World != nullptr ? World->GetGameState() : nullptr);
}

void AmultiplayerCoopHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearVictoryBinding();
	if (VictoryWidget != nullptr)
	{
		if (AmultiplayerCoopPlayerController* Controller = Cast<AmultiplayerCoopPlayerController>(PlayerOwner))
		{
			Controller->OnVictoryActionChanged.RemoveAll(VictoryWidget.Get());
		}
		VictoryWidget->RemoveFromParent();
		VictoryWidget = nullptr;
	}
	if (PlayerOwner != nullptr && PlayerOwner->IsLocalController())
	{
		PlayerOwner->bShowMouseCursor = false;
		PlayerOwner->SetInputMode(FInputModeGameOnly());
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerCoopHUD::RefreshVictoryBinding(AGameStateBase* GameState)
{
	ClearVictoryBinding();
	CoopGameState = Cast<AmultiplayerCoopGameState>(GameState);
	if (CoopGameState == nullptr)
	{
		// PlayingState 不保证 GameState 已到达，等待明确事件而非 Tick 轮询。
		if (UWorld* World = GetWorld())
		{
			GameStateEventWorld = World;
			GameStateSetEventHandle = World->GameStateSetEvent.AddUObject(
				this, &AmultiplayerCoopHUD::RefreshVictoryBinding);
		}
		return;
	}

	CoopGameState->OnGameWon.AddUniqueDynamic(this, &AmultiplayerCoopHUD::PresentCoopVictory);
	// 晚加入或晚绑定时，胜利通知可能已经发生，必须补读快照。
	if (CoopGameState->GetObjectiveState().bGameWon)
	{
		PresentCoopVictory();
	}
}

void AmultiplayerCoopHUD::ClearVictoryBinding()
{
	if (UWorld* BoundWorld = GameStateEventWorld.Get())
	{
		BoundWorld->GameStateSetEvent.Remove(GameStateSetEventHandle);
	}
	GameStateSetEventHandle.Reset();
	GameStateEventWorld.Reset();
	if (IsValid(CoopGameState))
	{
		CoopGameState->OnGameWon.RemoveDynamic(this, &AmultiplayerCoopHUD::PresentCoopVictory);
	}
	CoopGameState = nullptr;
}

/** 创建一次本地胜利界面，并把输入焦点交给可交互按钮；重复通知复用已有展示结果。 */
void AmultiplayerCoopHUD::PresentCoopVictory()
{
	AmultiplayerCoopPlayerController* Controller = Cast<AmultiplayerCoopPlayerController>(PlayerOwner);
	if (Controller == nullptr || !Controller->IsLocalController() || VictoryWidget != nullptr)
	{
		return;
	}

	// 先保存实例再展示/广播，防止同步重入；不再另存一份“已经通知”标记。
	VictoryWidget = CreateWidget<UmultiplayerVictoryWidget>(Controller, UmultiplayerVictoryWidget::StaticClass());
	if (VictoryWidget == nullptr)
	{
		UE_LOG(LogMultiplayer, Error, TEXT("Victory UI could not be created."));
		return;
	}
	VictoryWidget->AddToViewport(100);
	Controller->OnVictoryActionChanged.AddUObject(VictoryWidget.Get(), &UmultiplayerVictoryWidget::SetActionFeedback);
	VictoryWidget->SetActionFeedback(Controller->GetVictoryAction() == ECoopVictoryAction::Idle, FText::GetEmpty());
	// 胜利界面接管输入，避免点击按钮的同时继续操纵角色；EndPlay 时对称恢复。
	Controller->bShowMouseCursor = true;
	FInputModeUIOnly InputMode;
	if (const TSharedPtr<SWidget> InitialFocus = VictoryWidget->GetInitialFocusWidget(); InitialFocus.IsValid())
	{
		InputMode.SetWidgetToFocus(InitialFocus);
	}
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	Controller->SetInputMode(InputMode);
	UE_LOG(LogMultiplayer, Log, TEXT("Victory UI displayed with restart and leave actions."));

	// 蓝图事件是可选的表现扩展，不再承担“是否存在胜利界面”的基础职责。
	Controller->ReceiveCoopGameWon();
}
