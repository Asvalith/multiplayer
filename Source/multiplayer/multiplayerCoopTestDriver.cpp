// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerCoopTestDriver.h"

#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerCoopGate.h"
#include "multiplayerCoopKey.h"
#include "multiplayerGameInstance.h"
#include "multiplayerGameMode.h"
#include "multiplayerKeySocket.h"
#include "multiplayerLog.h"
#include "multiplayerMovingPlatform.h"
#include "multiplayerPressurePlate.h"
#include "multiplayerWinArea.h"

namespace MultiplayerCoopAutomation
{
	bool bRestartIssued = false;
	FString RestartedMap;
	constexpr float SettleSeconds = 1.0f;
}

AmultiplayerCoopTestDriver::AmultiplayerCoopTestDriver()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	bReplicates = false;
}

void AmultiplayerCoopTestDriver::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		Destroy();
		return;
	}

	const FString CurrentMap = GetWorld()->GetOutermost()->GetName();
	bPostRestartRun = MultiplayerCoopAutomation::bRestartIssued
		&& MultiplayerCoopAutomation::RestartedMap == CurrentMap;
	if (MultiplayerCoopAutomation::bRestartIssued && !bPostRestartRun)
	{
		// 返回主菜单后仍可能使用同一个全局 GameMode，不能在那里再次执行玩法测试。
		Destroy();
		return;
	}

	if (bPostRestartRun)
	{
		Phase = ETestPhase::PostRestartLeave;
		PhaseStartedAt = GetWorld()->GetTimeSeconds();
		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: current-map restart completed."));
		return;
	}

	FParse::Value(
		FCommandLine::Get(),
		TEXT("CoopBandwidthProfile="),
		BandwidthProfile);
	for (TActorIterator<AmultiplayerMovingPlatform> It(GetWorld()); It; ++It)
	{
		if (BandwidthProfile.Equals(TEXT("Baseline"), ESearchCase::IgnoreCase))
		{
			// 对照组只改变移动平台快照上限；玩法、时长与触发顺序保持一致。
			It->SetNetUpdateFrequency(100.0f);
			It->SetMinNetUpdateFrequency(100.0f);
		}
		PlatformStartLocations.Add(*It, It->GetActorLocation());
		PlatformMaxDistances.Add(*It, 0.0f);
		Platforms.Add(*It);
	}
	for (TActorIterator<AmultiplayerPressurePlate> It(GetWorld()); It; ++It) Plates.Add(*It);
	for (TActorIterator<AmultiplayerCoopGate> It(GetWorld()); It; ++It) Gates.Add(*It);

	UE_LOG(
		LogMultiplayer,
		Log,
		TEXT("Coop automation: waiting for two real player controllers. BandwidthProfile=%s"),
		BandwidthProfile.IsEmpty() ? TEXT("None") : *BandwidthProfile);
}

void AmultiplayerCoopTestDriver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetWorld()->GetTimeSeconds();
	for (const TPair<TObjectPtr<AmultiplayerMovingPlatform>, FVector>& Entry : PlatformStartLocations)
	{
		if (IsValid(Entry.Key))
		{
			float& MaxDistance = PlatformMaxDistances.FindOrAdd(Entry.Key);
			MaxDistance = FMath::Max(
				MaxDistance,
				FVector::Dist(Entry.Key->GetActorLocation(), Entry.Value));
		}
	}

	if (Phase == ETestPhase::PostRestartLeave)
	{
		if (Now - PhaseStartedAt < MultiplayerCoopAutomation::SettleSeconds
			|| !RefreshPlayers())
		{
			return;
		}

		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: initiating complete session leave."));
		if (UmultiplayerGameInstance* GameInstance =
			GetGameInstance<UmultiplayerGameInstance>())
		{
			Phase = ETestPhase::Finished;
			GameInstance->LeaveGame();
		}
		else
		{
			Fail(TEXT("GameInstance was unavailable for leave verification."));
		}
		return;
	}

	switch (Phase)
	{
	case ETestPhase::WaitForPlayers:
		if (RefreshPlayers())
		{
			UNetDriver* NetDriver = GetWorld()->GetNetDriver();
			BandwidthStartBytes = NetDriver != nullptr ? NetDriver->OutTotalBytes : 0;
			BandwidthStartedAt = Now;
			Phase = ETestPhase::CompleteKeys;
		}
		break;
	case ETestPhase::CompleteKeys:
		CompleteKeyObjectives();
		break;
	case ETestPhase::WaitForPlate:
		if (Now - PhaseStartedAt >= MultiplayerCoopAutomation::SettleSeconds)
		{
			ValidateCurrentPlate();
		}
		break;
	case ETestPhase::WaitForGate:
		if (Now - PhaseStartedAt >= MultiplayerCoopAutomation::SettleSeconds)
		{
			ValidateCurrentGate();
		}
		break;
	case ETestPhase::WaitForPlatform:
		if (Now - PhaseStartedAt >= MultiplayerCoopAutomation::SettleSeconds)
		{
			ValidateCurrentPlatform();
		}
		break;
	case ETestPhase::WaitForVictory:
		if (Now - PhaseStartedAt >= MultiplayerCoopAutomation::SettleSeconds)
		{
			ValidateVictory();
		}
		break;
	case ETestPhase::Finished:
		if (FParse::Param(FCommandLine::Get(), TEXT("CoopTestRestart"))
			&& !MultiplayerCoopAutomation::bRestartIssued
			&& Now - PhaseStartedAt >= MultiplayerCoopAutomation::SettleSeconds)
		{
			MultiplayerCoopAutomation::bRestartIssued = true;
			MultiplayerCoopAutomation::RestartedMap = GetWorld()->GetOutermost()->GetName();
			if (AmultiplayerGameMode* GameMode =
				GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
			{
				GameMode->RequestRestartCurrentRound(Players[0]->GetController());
			}
		}
		break;
	default:
		break;
	}
}

bool AmultiplayerCoopTestDriver::RefreshPlayers()
{
	Players.Reset();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* Controller = It->Get();
		ACharacter* Character = Controller != nullptr
			? Cast<ACharacter>(Controller->GetPawn())
			: nullptr;
		if (Character != nullptr)
		{
			Players.Add(Character);
		}
	}
	return Players.Num() >= 2;
}

void AmultiplayerCoopTestDriver::CompleteKeyObjectives()
{
	TArray<AmultiplayerCoopKey*> Keys;
	TArray<AmultiplayerKeySocket*> Sockets;
	for (TActorIterator<AmultiplayerCoopKey> It(GetWorld()); It; ++It) Keys.Add(*It);
	for (TActorIterator<AmultiplayerKeySocket> It(GetWorld()); It; ++It) Sockets.Add(*It);
	Keys.Sort([](const AmultiplayerCoopKey& A, const AmultiplayerCoopKey& B)
	{
		return A.GetName() < B.GetName();
	});
	Sockets.Sort([](const AmultiplayerKeySocket& A, const AmultiplayerKeySocket& B)
	{
		return A.GetName() < B.GetName();
	});

	if (Sockets.IsEmpty() || Keys.Num() < Sockets.Num())
	{
		Fail(TEXT("The map does not contain a complete key/socket set."));
		return;
	}

	for (int32 Index = 0; Index < Sockets.Num(); ++Index)
	{
		if (!Sockets[Index]->StoreCollectedKey(Keys[Index]))
		{
			Fail(FString::Printf(TEXT("Key/socket activation failed at index %d."), Index));
			return;
		}
		if (Sockets[Index]->StoreCollectedKey(Keys[Index]))
		{
			Fail(TEXT("A socket accepted the same completed key twice."));
			return;
		}
	}

	const AmultiplayerCoopGameState* State =
		GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (State == nullptr
		|| State->GetObjectiveState().ActivatedKeys != Sockets.Num()
		|| !State->IsObjectiveComplete())
	{
		Fail(TEXT("Authoritative key progress did not match activated sockets."));
		return;
	}

	UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: key/socket authority and duplicate guard passed."));
	if (Plates.IsEmpty() || Gates.IsEmpty() || Platforms.IsEmpty())
	{
		Fail(TEXT("The map is missing a pressure plate, gate, or moving platform."));
		return;
	}
	ActivateNextPlate();
}

void AmultiplayerCoopTestDriver::ActivateNextPlate()
{
	if (CurrentPlateIndex >= Plates.Num())
	{
		CurrentGateIndex = 0;
		ActivateNextGate();
		return;
	}

	MovePlayerTo(Players[0], Plates[CurrentPlateIndex]->GetActivationCenter());
	Phase = ETestPhase::WaitForPlate;
	PhaseStartedAt = GetWorld()->GetTimeSeconds();
}

void AmultiplayerCoopTestDriver::ValidateCurrentPlate()
{
	if (!IsValid(Plates[CurrentPlateIndex])
		|| !Plates[CurrentPlateIndex]->IsPlateActive())
	{
		Fail(TEXT("A pressure plate did not activate through its overlap path."));
		return;
	}
	++CurrentPlateIndex;
	ActivateNextPlate();
}

void AmultiplayerCoopTestDriver::ActivateNextGate()
{
	if (CurrentGateIndex >= Gates.Num())
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: pressure plate and gate flow passed."));
		CurrentPlatformIndex = 0;
		ActivateNextPlatform();
		return;
	}

	TArray<AmultiplayerPressurePlate*> RequiredPlates;
	Gates[CurrentGateIndex]->GetRequiredPlates(RequiredPlates);
	const int32 RequiredCount = Gates[CurrentGateIndex]->GetRequiredPlateCount();
	if (RequiredCount > Players.Num() || RequiredPlates.Num() < RequiredCount)
	{
		Fail(TEXT("A gate requires more valid pressure plates than the two-player test can satisfy."));
		return;
	}
	for (int32 Index = 0; Index < RequiredCount; ++Index)
	{
		MovePlayerTo(Players[Index], RequiredPlates[Index]->GetActivationCenter());
	}
	Phase = ETestPhase::WaitForGate;
	PhaseStartedAt = GetWorld()->GetTimeSeconds();
}

void AmultiplayerCoopTestDriver::ValidateCurrentGate()
{
	if (!IsValid(Gates[CurrentGateIndex]) || !Gates[CurrentGateIndex]->IsGateOpen())
	{
		Fail(TEXT("A gate did not open after its configured pressure plates were activated."));
		return;
	}
	++CurrentGateIndex;
	ActivateNextGate();
}

void AmultiplayerCoopTestDriver::ActivateNextPlatform()
{
	if (CurrentPlatformIndex >= Platforms.Num())
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: server-driven moving platform flow passed."));
		EnterWinArea();
		return;
	}

	AmultiplayerMovingPlatform* Platform = Platforms[CurrentPlatformIndex];
	if (!IsValid(Platform))
	{
		Fail(TEXT("A moving platform was destroyed during the test."));
		return;
	}

	if (Platform->UsesPlatformOccupancy())
	{
		if (Platform->GetRequiredOccupantCount() > Players.Num())
		{
			Fail(TEXT("A platform requires more occupants than the test provides."));
			return;
		}
		for (int32 Index = 0; Index < Platform->GetRequiredOccupantCount(); ++Index)
		{
			MovePlayerTo(Players[Index], Platform->GetActivationCenter());
		}
	}
	else if (AmultiplayerPressurePlate* ActivationPlate = Platform->GetActivationPlate())
	{
		MovePlayerTo(Players[0], ActivationPlate->GetActivationCenter());
	}
	else
	{
		Fail(TEXT("An external-pressure-plate platform has no activation plate."));
		return;
	}

	Phase = ETestPhase::WaitForPlatform;
	PhaseStartedAt = GetWorld()->GetTimeSeconds();
}

void AmultiplayerCoopTestDriver::ValidateCurrentPlatform()
{
	AmultiplayerMovingPlatform* Platform = Platforms[CurrentPlatformIndex];
	const float* MaxDistance = PlatformMaxDistances.Find(Platform);
	if (!IsValid(Platform) || MaxDistance == nullptr || *MaxDistance < 5.0f)
	{
		Fail(TEXT("A moving platform did not move through its configured activation path."));
		return;
	}
	++CurrentPlatformIndex;
	ActivateNextPlatform();
}

void AmultiplayerCoopTestDriver::EnterWinArea()
{
	AmultiplayerWinArea* WinArea = nullptr;
	for (TActorIterator<AmultiplayerWinArea> It(GetWorld()); It; ++It)
	{
		WinArea = *It;
		break;
	}
	if (WinArea == nullptr)
	{
		Fail(TEXT("No win area exists in the map."));
		return;
	}

	const FVector Center = WinArea->GetActivationCenter();
	MovePlayerTo(Players[0], Center + FVector(30.0f, 0.0f, 0.0f));
	MovePlayerTo(Players[1], Center - FVector(30.0f, 0.0f, 0.0f));
	Phase = ETestPhase::WaitForVictory;
	PhaseStartedAt = GetWorld()->GetTimeSeconds();
}

void AmultiplayerCoopTestDriver::ValidateVictory()
{
	const AmultiplayerCoopGameState* State =
		GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (State == nullptr || !State->GetObjectiveState().bGameWon)
	{
		Fail(TEXT("Two players in the win area did not produce an authoritative victory."));
		return;
	}

	FinishBandwidthSample();
	UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: FULL GAMEPLAY FLOW PASS."));
	Phase = ETestPhase::Finished;
	PhaseStartedAt = GetWorld()->GetTimeSeconds();
}

void AmultiplayerCoopTestDriver::FinishBandwidthSample()
{
	if (BandwidthProfile.IsEmpty())
	{
		return;
	}

	UNetDriver* NetDriver = GetWorld()->GetNetDriver();
	const uint32 EndBytes = NetDriver != nullptr ? NetDriver->OutTotalBytes : BandwidthStartBytes;
	const uint32 DeltaBytes = EndBytes - BandwidthStartBytes;
	const float Duration = FMath::Max(0.001f, GetWorld()->GetTimeSeconds() - BandwidthStartedAt);
	UE_LOG(
		LogMultiplayer,
		Log,
		TEXT("Coop bandwidth sample: Profile=%s DurationSeconds=%.3f OutBytes=%u BytesPerSecond=%.3f"),
		*BandwidthProfile,
		Duration,
		DeltaBytes,
		DeltaBytes / Duration);
}

void AmultiplayerCoopTestDriver::Fail(const FString& Reason)
{
	Phase = ETestPhase::Failed;
	SetActorTickEnabled(false);
	UE_LOG(LogMultiplayer, Error, TEXT("Coop automation failed: %s"), *Reason);
}

void AmultiplayerCoopTestDriver::MovePlayerTo(
	ACharacter* Character,
	const FVector& Location) const
{
	if (Character != nullptr)
	{
		Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
}
