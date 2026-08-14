// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerVictoryPresenterComponent.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "multiplayer.h"
#include "multiplayerCharacter.h"
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

	UWorld* World = GetWorld();
	CoopGameState = World != nullptr
		? World->GetGameState<AmultiplayerCoopGameState>()
		: nullptr;
	if (CoopGameState == nullptr)
	{
		return;
	}

	CoopGameState->OnGameWon.AddUniqueDynamic(
		this,
		&UmultiplayerVictoryPresenterComponent::HandleGameWon);

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_VICTORY_UI Phase=Bound Actor=%s GameState=%s AlreadyWon=%s"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(CoopGameState),
		CoopGameState->GetObjectiveState().bGameWon ? TEXT("true") : TEXT("false"));

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
	const bool bHadBinding = IsValid(CoopGameState);
	const bool bHadWidget = IsValid(VictoryWidget);
	const bool bHadPresentationState =
		bAppliedGameAndUIInput || bAppliedMouseCursorOverride;

	if (IsValid(CoopGameState))
	{
		CoopGameState->OnGameWon.RemoveDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleGameWon);
	}
	CoopGameState = nullptr;

	if (IsValid(VictoryWidget))
	{
		if (IsValid(RestartButton))
		{
			RestartButton->OnClicked.RemoveDynamic(
				this,
				&UmultiplayerVictoryPresenterComponent::HandleRestartClicked);
		}
		VictoryWidget->RemoveFromParent();
	}
	RestartButton = nullptr;
	VictoryWidget = nullptr;

	bool bRestoredInput = false;
	bool bRestoredMouseCursor = false;
	if (APlayerController* PlayerController = PresentationPlayerController.Get();
		PlayerController != nullptr && PlayerController->IsLocalController())
	{
		if (bAppliedGameAndUIInput)
		{
			PlayerController->SetInputMode(FInputModeGameOnly());
			bRestoredInput = true;
		}

		// Do not overwrite a cursor state another system changed after the victory
		// screen was shown. Restore only while our applied value is still active.
		if (bAppliedMouseCursorOverride
			&& PlayerController->bShowMouseCursor == bAppliedMouseCursorState)
		{
			PlayerController->bShowMouseCursor = bPreviousMouseCursorState;
			bRestoredMouseCursor = true;
		}
	}

	PresentationPlayerController.Reset();
	bAppliedGameAndUIInput = false;
	bAppliedMouseCursorOverride = false;
	bPreviousMouseCursorState = false;
	bAppliedMouseCursorState = false;
	bRestartRequested = false;

	if (bHadBinding || bHadWidget || bHadPresentationState)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("COOP_VICTORY_UI Phase=Cleared Actor=%s HadBinding=%s RemovedWidget=%s RestoredInput=%s RestoredMouse=%s"),
			*GetNameSafe(GetOwner()),
			bHadBinding ? TEXT("true") : TEXT("false"),
			bHadWidget ? TEXT("true") : TEXT("false"),
			bRestoredInput ? TEXT("true") : TEXT("false"),
			bRestoredMouseCursor ? TEXT("true") : TEXT("false"));
	}
}

void UmultiplayerVictoryPresenterComponent::HandleGameWon()
{
	if (IsValid(VictoryWidget))
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("COOP_VICTORY_UI Phase=AlreadyVisible Actor=%s Widget=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(VictoryWidget));
		return;
	}
	VictoryWidget = nullptr;

	if (VictoryWidgetClass == nullptr)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("COOP_VICTORY_UI Phase=MissingWidgetClass Actor=%s"),
			*GetNameSafe(GetOwner()));
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
		UE_LOG(
			LogMultiplayerGAS,
			Error,
			TEXT("COOP_VICTORY_UI Phase=CreateFailed Actor=%s Controller=%s WidgetClass=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(PlayerController),
			*GetNameSafe(VictoryWidgetClass));
		return;
	}

	VictoryWidget->AddToViewport(100);
	RestartButton = Cast<UButton>(VictoryWidget->GetWidgetFromName(RestartButtonName));
	if (IsValid(RestartButton))
	{
		RestartButton->OnClicked.AddUniqueDynamic(
			this,
			&UmultiplayerVictoryPresenterComponent::HandleRestartClicked);
	}
	else
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("COOP_VICTORY_UI Phase=RestartButtonMissing Actor=%s Widget=%s ButtonName=%s"),
			*GetNameSafe(GetOwner()),
			*GetNameSafe(VictoryWidget),
			*RestartButtonName.ToString());
	}
	PresentationPlayerController = PlayerController;
	bPreviousMouseCursorState = PlayerController->bShowMouseCursor;
	bAppliedMouseCursorState = bShowMouseCursor;
	bAppliedMouseCursorOverride = bPreviousMouseCursorState != bAppliedMouseCursorState;
	if (bAppliedMouseCursorOverride)
	{
		PlayerController->bShowMouseCursor = bAppliedMouseCursorState;
	}

	if (bSwitchToGameAndUIInput)
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(VictoryWidget->TakeWidget());
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
		bAppliedGameAndUIInput = true;
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_VICTORY_UI Phase=Created Actor=%s Controller=%s Widget=%s GameAndUI=%s Cursor=%s"),
		*GetNameSafe(GetOwner()),
		*GetNameSafe(PlayerController),
		*GetNameSafe(VictoryWidget),
		bAppliedGameAndUIInput ? TEXT("true") : TEXT("false"),
		PlayerController->bShowMouseCursor ? TEXT("true") : TEXT("false"));
}

void UmultiplayerVictoryPresenterComponent::HandleRestartClicked()
{
	if (bRestartRequested)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Verbose,
			TEXT("COOP_VICTORY_UI Phase=RestartIgnored Actor=%s Reason=AlreadyRequested"),
			*GetNameSafe(GetOwner()));
		return;
	}

	AmultiplayerCharacter* Character = Cast<AmultiplayerCharacter>(GetOwner());
	if (Character == nullptr || !Character->IsLocallyControlled())
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("COOP_VICTORY_UI Phase=RestartIgnored Actor=%s Reason=InvalidLocalOwner"),
			*GetNameSafe(GetOwner()));
		return;
	}

	bRestartRequested = true;
	if (IsValid(RestartButton))
	{
		RestartButton->SetIsEnabled(false);
	}

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("COOP_VICTORY_UI Phase=RestartRequested Actor=%s Role=%s"),
		*GetNameSafe(Character),
		*UEnum::GetValueAsString(Character->GetLocalRole()));
	Character->RequestRestartCoopGame();
}
