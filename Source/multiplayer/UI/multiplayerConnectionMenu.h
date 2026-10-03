#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerConnectionMenu.generated.h"

class SEditableTextBox;
class SButton;
class STextBlock;

/** DS 菜单只提交地址与取消意图，连接状态始终由 GameInstance 持有。 */
UCLASS()
class MULTIPLAYER_API UmultiplayerConnectionWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	// 控件树生命周期与连接状态订阅。
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;

private:
	// 按 GameInstance 的当前状态刷新控件。
	void Refresh();

	// 用户操作回调。
	FReply ConnectClicked();
	FReply CancelClicked();

	// 在 ReleaseSlateResources 中释放的 Slate 引用。
	TSharedPtr<SEditableTextBox> Address;
	TSharedPtr<SButton> ConnectButton;
	TSharedPtr<SButton> CancelButton;
	TSharedPtr<STextBlock> Status;
};

/** 独立菜单控制器，不生成游戏 Pawn，不在服务器创建界面。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerMenuController : public APlayerController
{
	GENERATED_BODY()

protected:
	// 本地菜单生命周期。
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UmultiplayerConnectionWidget> Menu;
};

UCLASS()
class MULTIPLAYER_API AmultiplayerMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AmultiplayerMenuGameMode();
};
