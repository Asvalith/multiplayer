#include "UI/multiplayerConnectionMenu.h"
#include "Network/multiplayerGameInstance.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<SWidget> UmultiplayerConnectionWidget::RebuildWidget()
{
	return SNew(SOverlay) + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
	[
		SNew(SBorder).Padding(32)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SNew(STextBlock).Text(NSLOCTEXT("Multiplayer", "DSMenu", "连接 Co-op 专用服务器"))]
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SAssignNew(Address, SEditableTextBox).MinDesiredWidth(380).Text(FText::FromString(TEXT("127.0.0.1:7777")))]
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SAssignNew(ConnectButton, SButton).Text(NSLOCTEXT("Multiplayer", "Connect", "连接"))
				.OnClicked_UObject(this, &ThisClass::ConnectClicked)]
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SAssignNew(CancelButton, SButton).Text(NSLOCTEXT("Multiplayer", "Cancel", "取消连接 / 重连"))
				.OnClicked_UObject(this, &ThisClass::CancelClicked)]
			+ SVerticalBox::Slot().AutoHeight().Padding(6)
			[SAssignNew(Status, STextBlock).WrapTextAt(480)]
		]
	];
}

void UmultiplayerConnectionWidget::NativeConstruct()
{
	Super::NativeConstruct();
	if (auto* GI = GetGameInstance<UmultiplayerGameInstance>()) GI->OnConnectionChanged.AddUObject(this, &ThisClass::Refresh);
	Refresh();
}
void UmultiplayerConnectionWidget::NativeDestruct()
{
	if (auto* GI = GetGameInstance<UmultiplayerGameInstance>()) GI->OnConnectionChanged.RemoveAll(this);
	Super::NativeDestruct();
}
void UmultiplayerConnectionWidget::ReleaseSlateResources(bool bReleaseChildren)
{
	Address.Reset(); ConnectButton.Reset(); CancelButton.Reset(); Status.Reset();
	Super::ReleaseSlateResources(bReleaseChildren);
}
void UmultiplayerConnectionWidget::Refresh()
{
	const auto* GI = GetGameInstance<UmultiplayerGameInstance>();
	if (!GI || !ConnectButton || !CancelButton || !Address || !Status) return;
	const auto State = GI->GetConnectionState();
	const bool bIdle = State == EMultiplayerConnectionState::Idle;
	ConnectButton->SetEnabled(bIdle);
	Address->SetEnabled(bIdle);
	CancelButton->SetEnabled(!bIdle && State != EMultiplayerConnectionState::Leaving);
	Status->SetText(GI->GetConnectionMessage());
}
FReply UmultiplayerConnectionWidget::ConnectClicked()
{
	if (auto* GI = GetGameInstance<UmultiplayerGameInstance>(); GI && Address) GI->ConnectToServer(Address->GetText().ToString());
	return FReply::Handled();
}
FReply UmultiplayerConnectionWidget::CancelClicked()
{
	if (auto* GI = GetGameInstance<UmultiplayerGameInstance>()) GI->LeaveGame();
	return FReply::Handled();
}

AmultiplayerMenuGameMode::AmultiplayerMenuGameMode()
{
	PlayerControllerClass = AmultiplayerMenuController::StaticClass();
	DefaultPawnClass = nullptr;
}
void AmultiplayerMenuController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController() || GetNetMode() != NM_Standalone) return;
	Menu = CreateWidget<UmultiplayerConnectionWidget>(this, UmultiplayerConnectionWidget::StaticClass());
	if (Menu) Menu->AddToViewport();
	bShowMouseCursor = true;
	SetInputMode(FInputModeUIOnly());
}
void AmultiplayerMenuController::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Menu) { Menu->RemoveFromParent(); Menu = nullptr; }
	Super::EndPlay(Reason);
}
