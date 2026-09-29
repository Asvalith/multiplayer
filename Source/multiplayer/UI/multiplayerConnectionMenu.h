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
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void ReleaseSlateResources(bool bReleaseChildren) override;
private:
	void Refresh();
	FReply ConnectClicked();
	FReply CancelClicked();
	TSharedPtr<SEditableTextBox> Address;
	TSharedPtr<SButton> ConnectButton, CancelButton;
	TSharedPtr<STextBlock> Status;
};

/** 独立菜单控制器，不生成游戏 Pawn，不在服务器创建界面。 */
UCLASS()
class MULTIPLAYER_API AmultiplayerMenuController : public APlayerController
{
	GENERATED_BODY()
protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
	UPROPERTY(Transient) TObjectPtr<UmultiplayerConnectionWidget> Menu;
};

UCLASS()
class MULTIPLAYER_API AmultiplayerMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	AmultiplayerMenuGameMode();
};
