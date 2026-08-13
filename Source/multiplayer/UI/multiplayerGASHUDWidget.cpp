// Copyright Epic Games, Inc. All Rights Reserved.

#include "UI/multiplayerGASHUDWidget.h"

#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Player/multiplayerGASPlayerState.h"

void UmultiplayerGASHUDWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildNativeFallbackLayout();
}

void UmultiplayerGASHUDWidget::InitializeWithPlayerState(
	AmultiplayerGASPlayerState* NewPlayerState)
{
	if (BoundPlayerState == NewPlayerState && BoundPlayerState != nullptr)
	{
		BroadcastCurrentValues();
		return;
	}

	ClearPlayerStateBinding();
	BoundPlayerState = NewPlayerState;
	if (BoundPlayerState == nullptr)
	{
		return;
	}

	BoundPlayerState->OnHealthChanged.AddDynamic(
		this,
		&UmultiplayerGASHUDWidget::HandleHealthChanged);
	BoundPlayerState->OnEnergyChanged.AddDynamic(
		this,
		&UmultiplayerGASHUDWidget::HandleEnergyChanged);

	RegisterObservedTag(MultiplayerGameplayTags::Cooldown_Ability_Damage);
	RegisterObservedTag(MultiplayerGameplayTags::Cooldown_Ability_Heal);
	RegisterObservedTag(MultiplayerGameplayTags::Cooldown_Ability_Immunity);
	RegisterObservedTag(MultiplayerGameplayTags::State_Immune);
	RegisterObservedTag(MultiplayerGameplayTags::State_Dead);
	RegisterObservedTag(MultiplayerGameplayTags::State_Vulnerable);

	BroadcastCurrentValues();
	BP_OnGASHUDInitialized();
}

void UmultiplayerGASHUDWidget::ClearPlayerStateBinding()
{
	if (BoundPlayerState != nullptr)
	{
		BoundPlayerState->OnHealthChanged.RemoveDynamic(
			this,
			&UmultiplayerGASHUDWidget::HandleHealthChanged);
		BoundPlayerState->OnEnergyChanged.RemoveDynamic(
			this,
			&UmultiplayerGASHUDWidget::HandleEnergyChanged);

		if (UmultiplayerAbilitySystemComponent* ASC =
			BoundPlayerState->GetMultiplayerAbilitySystemComponent())
		{
			for (const TPair<FGameplayTag, FDelegateHandle>& Entry : ObservedTagHandles)
			{
				ASC->RegisterGameplayTagEvent(
					Entry.Key,
					EGameplayTagEventType::NewOrRemoved).Remove(Entry.Value);
			}
		}
	}

	ObservedTagHandles.Reset();
	ActiveObservedTags.Reset();
	BoundPlayerState = nullptr;
}

void UmultiplayerGASHUDWidget::NativeDestruct()
{
	ClearPlayerStateBinding();
	Super::NativeDestruct();
}

void UmultiplayerGASHUDWidget::HandleHealthChanged(float OldValue, float NewValue)
{
	CurrentHealth = NewValue;
	MaxHealth = BoundPlayerState != nullptr ? BoundPlayerState->GetMaxHealth() : 0.0f;
	UpdateAttributePresentation();
	OnHealthDisplayChanged.Broadcast(CurrentHealth, MaxHealth);
}

void UmultiplayerGASHUDWidget::HandleEnergyChanged(float OldValue, float NewValue)
{
	CurrentEnergy = NewValue;
	MaxEnergy = BoundPlayerState != nullptr ? BoundPlayerState->GetMaxEnergy() : 0.0f;
	UpdateAttributePresentation();
	OnEnergyDisplayChanged.Broadcast(CurrentEnergy, MaxEnergy);
}

void UmultiplayerGASHUDWidget::HandleObservedTagChanged(
	const FGameplayTag Tag,
	int32 NewCount)
{
	if (NewCount > 0)
	{
		ActiveObservedTags.Add(Tag);
	}
	else
	{
		ActiveObservedTags.Remove(Tag);
	}
	UpdateTagPresentation();
	OnTagStateDisplayChanged.Broadcast(Tag, NewCount > 0);
}

void UmultiplayerGASHUDWidget::BroadcastCurrentValues()
{
	if (BoundPlayerState == nullptr)
	{
		return;
	}

	CurrentHealth = BoundPlayerState->GetHealth();
	MaxHealth = BoundPlayerState->GetMaxHealth();
	CurrentEnergy = BoundPlayerState->GetEnergy();
	MaxEnergy = BoundPlayerState->GetMaxEnergy();
	UpdateAttributePresentation();
	OnHealthDisplayChanged.Broadcast(CurrentHealth, MaxHealth);
	OnEnergyDisplayChanged.Broadcast(CurrentEnergy, MaxEnergy);

	if (const UmultiplayerAbilitySystemComponent* ASC =
		BoundPlayerState->GetMultiplayerAbilitySystemComponent())
	{
		for (const TPair<FGameplayTag, FDelegateHandle>& Entry : ObservedTagHandles)
		{
			const bool bIsActive = ASC->GetGameplayTagCount(Entry.Key) > 0;
			if (bIsActive)
			{
				ActiveObservedTags.Add(Entry.Key);
			}
			else
			{
				ActiveObservedTags.Remove(Entry.Key);
			}
			OnTagStateDisplayChanged.Broadcast(
				Entry.Key,
				bIsActive);
		}
	}
	UpdateTagPresentation();
}

void UmultiplayerGASHUDWidget::RegisterObservedTag(const FGameplayTag& Tag)
{
	UmultiplayerAbilitySystemComponent* ASC = BoundPlayerState != nullptr
		? BoundPlayerState->GetMultiplayerAbilitySystemComponent()
		: nullptr;
	if (ASC == nullptr || !Tag.IsValid() || ObservedTagHandles.Contains(Tag))
	{
		return;
	}

	const FDelegateHandle Handle = ASC->RegisterGameplayTagEvent(
		Tag,
		EGameplayTagEventType::NewOrRemoved).AddUObject(
		this,
		&UmultiplayerGASHUDWidget::HandleObservedTagChanged);
	ObservedTagHandles.Add(Tag, Handle);
}

void UmultiplayerGASHUDWidget::BuildNativeFallbackLayout()
{
	if (WidgetTree == nullptr || WidgetTree->RootWidget != nullptr)
	{
		return;
	}

	UCanvasPanel* RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>(
		UCanvasPanel::StaticClass(),
		TEXT("GASHUDRoot"));
	WidgetTree->RootWidget = RootCanvas;

	UVerticalBox* Panel = WidgetTree->ConstructWidget<UVerticalBox>(
		UVerticalBox::StaticClass(),
		TEXT("GASHUDPanel"));
	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(Panel);
	PanelSlot->SetPosition(FVector2D(24.0f, 24.0f));
	PanelSlot->SetSize(FVector2D(330.0f, 135.0f));

	HealthText = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		TEXT("HealthText"));
	HealthBar = WidgetTree->ConstructWidget<UProgressBar>(
		UProgressBar::StaticClass(),
		TEXT("HealthBar"));
	EnergyText = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		TEXT("EnergyText"));
	EnergyBar = WidgetTree->ConstructWidget<UProgressBar>(
		UProgressBar::StaticClass(),
		TEXT("EnergyBar"));
	StateText = WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		TEXT("StateText"));

	HealthBar->SetFillColorAndOpacity(FLinearColor(0.85f, 0.08f, 0.08f, 1.0f));
	EnergyBar->SetFillColorAndOpacity(FLinearColor(0.05f, 0.35f, 0.95f, 1.0f));
	StateText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.1f, 1.0f)));

	Panel->AddChildToVerticalBox(HealthText)->SetPadding(FMargin(0.0f, 2.0f));
	Panel->AddChildToVerticalBox(HealthBar)->SetPadding(FMargin(0.0f, 2.0f));
	Panel->AddChildToVerticalBox(EnergyText)->SetPadding(FMargin(0.0f, 2.0f));
	Panel->AddChildToVerticalBox(EnergyBar)->SetPadding(FMargin(0.0f, 2.0f));
	Panel->AddChildToVerticalBox(StateText)->SetPadding(FMargin(0.0f, 4.0f));

	UpdateAttributePresentation();
	UpdateTagPresentation();
}

void UmultiplayerGASHUDWidget::UpdateAttributePresentation()
{
	if (HealthText != nullptr)
	{
		HealthText->SetText(FText::FromString(FString::Printf(
			TEXT("Health  %.0f / %.0f"),
			CurrentHealth,
			MaxHealth)));
	}
	if (HealthBar != nullptr)
	{
		HealthBar->SetPercent(MaxHealth > 0.0f ? CurrentHealth / MaxHealth : 0.0f);
	}
	if (EnergyText != nullptr)
	{
		EnergyText->SetText(FText::FromString(FString::Printf(
			TEXT("Energy  %.0f / %.0f"),
			CurrentEnergy,
			MaxEnergy)));
	}
	if (EnergyBar != nullptr)
	{
		EnergyBar->SetPercent(MaxEnergy > 0.0f ? CurrentEnergy / MaxEnergy : 0.0f);
	}
}

void UmultiplayerGASHUDWidget::UpdateTagPresentation()
{
	if (StateText == nullptr)
	{
		return;
	}

	TArray<FString> States;
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::State_Immune))
	{
		States.Add(TEXT("IMMUNE"));
	}
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::State_Dead))
	{
		States.Add(TEXT("DEAD - respawning"));
	}
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::State_Vulnerable))
	{
		States.Add(TEXT("VULNERABLE"));
	}
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::Cooldown_Ability_Damage))
	{
		States.Add(TEXT("Damage CD"));
	}
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::Cooldown_Ability_Heal))
	{
		States.Add(TEXT("Heal CD"));
	}
	if (ActiveObservedTags.Contains(MultiplayerGameplayTags::Cooldown_Ability_Immunity))
	{
		States.Add(TEXT("Immunity CD"));
	}

	StateText->SetText(FText::FromString(
		States.IsEmpty() ? TEXT("Abilities ready") : FString::Join(States, TEXT(" | "))));
}
