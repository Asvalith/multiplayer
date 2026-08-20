// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "Blueprint/UserWidget.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "multiplayerCoopGameState.h"

UmultiplayerVictoryPresenterComponent::UmultiplayerVictoryPresenterComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UmultiplayerVictoryPresenterComponent::BeginPlay()
{
	Super::BeginPlay();
	RefreshBinding();
}

void UmultiplayerVictoryPresenterComponent::RefreshBinding()
{
	ClearBinding();

	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (OwnerPawn == nullptr || !OwnerPawn->IsLocallyControlled())
	{
		return;
	}

	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState == nullptr)
	{
		return;
	}

	CoopGameState->OnGameWon.AddUniqueDynamic(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameWon);

	if (CoopGameState->GetObjectiveState().bGameWon)
	{
		HandleGameWon();
	}
}

void UmultiplayerVictoryPresenterComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearBinding();
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerVictoryPresenterComponent::ClearBinding()
{
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnGameWon.RemoveDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleGameWon);
	}
	CoopGameState = nullptr;
}

void UmultiplayerVictoryPresenterComponent::HandleGameWon()
{
	if (VictoryWidget != nullptr || VictoryWidgetClass == nullptr)
	{
		return;
	}

	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	APlayerController* PlayerController = OwnerPawn != nullptr
		? Cast<APlayerController>(OwnerPawn->GetController())
		: nullptr;
	if (PlayerController == nullptr || !PlayerController->IsLocalController())
	{
		return;
	}

	VictoryWidget = CreateWidget<UUserWidget>(PlayerController, VictoryWidgetClass);
	if (VictoryWidget == nullptr)
	{
		return;
	}

	VictoryWidget->AddToViewport(100);
	PlayerController->bShowMouseCursor = bShowMouseCursor;
	if (bSwitchToGameAndUIInput)
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(VictoryWidget->TakeWidget());
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}
}
