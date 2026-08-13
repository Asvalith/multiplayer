// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/multiplayerGASHUDPresenterComponent.h"

#include "Blueprint/UserWidget.h"
#include "Player/multiplayerGASPlayerState.h"
#include "UI/multiplayerGASHUDWidget.h"
#include "multiplayerCharacter.h"

UmultiplayerGASHUDPresenterComponent::UmultiplayerGASHUDPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
	HUDWidgetClass = UmultiplayerGASHUDWidget::StaticClass();
}

void UmultiplayerGASHUDPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	RefreshBinding();
}

void UmultiplayerGASHUDPresenterComponent::RefreshBinding()
{
	AmultiplayerCharacter* Character = Cast<AmultiplayerCharacter>(GetOwner());
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		ReleaseWidget();
		return;
	}

	APlayerController* PlayerController = Cast<APlayerController>(Character->GetController());
	AmultiplayerGASPlayerState* PlayerState =
		Character->GetPlayerState<AmultiplayerGASPlayerState>();
	if (PlayerController == nullptr || PlayerState == nullptr || HUDWidgetClass == nullptr)
	{
		return;
	}

	if (HUDWidget != nullptr && !HUDWidget->IsA(HUDWidgetClass))
	{
		ReleaseWidget();
	}

	if (HUDWidget == nullptr)
	{
		HUDWidget = CreateWidget<UmultiplayerGASHUDWidget>(
			PlayerController,
			HUDWidgetClass);
		if (HUDWidget != nullptr)
		{
			HUDWidget->AddToViewport();
		}
	}

	if (HUDWidget != nullptr)
	{
		HUDWidget->InitializeWithPlayerState(PlayerState);
	}
}

void UmultiplayerGASHUDPresenterComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	ReleaseWidget();
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerGASHUDPresenterComponent::ReleaseWidget()
{
	if (HUDWidget != nullptr)
	{
		HUDWidget->ClearPlayerStateBinding();
		HUDWidget->RemoveFromParent();
		HUDWidget = nullptr;
	}
}
