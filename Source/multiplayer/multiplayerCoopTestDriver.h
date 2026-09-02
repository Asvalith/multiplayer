// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "multiplayerCoopTestDriver.generated.h"

class ACharacter;
class AmultiplayerCoopGate;
class AmultiplayerMovingPlatform;
class AmultiplayerPressurePlate;

/**
 * Development/Debug 构建的双实例端到端驱动。
 *
 * 它不伪造 GameState，而是让钥匙进入插槽、把真实玩家移动到压力板/平台/胜利触发体，
 * 再读取服务器权威结果。GameMode 只在显式命令行参数存在时生成该对象，Shipping 不会启动它。
 */
UCLASS(NotBlueprintable, NotPlaceable, Transient)
class MULTIPLAYER_API AmultiplayerCoopTestDriver : public AActor
{
	GENERATED_BODY()

public:
	AmultiplayerCoopTestDriver();
	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

private:
	enum class ETestPhase : uint8
	{
		WaitForPlayers,
		CompleteKeys,
		WaitForPlate,
		WaitForGate,
		WaitForPlatform,
		WaitForVictory,
		Finished,
		PostRestartLeave,
		Failed
	};

	bool RefreshPlayers();
	void CompleteKeyObjectives();
	void ActivateNextPlate();
	void ValidateCurrentPlate();
	void ActivateNextGate();
	void ValidateCurrentGate();
	void ActivateNextPlatform();
	void ValidateCurrentPlatform();
	void EnterWinArea();
	void ValidateVictory();
	void FinishBandwidthSample();
	void Fail(const FString& Reason);
	void MovePlayerTo(ACharacter* Character, const FVector& Location) const;

	ETestPhase Phase = ETestPhase::WaitForPlayers;
	TArray<TObjectPtr<ACharacter>> Players;
	TArray<TObjectPtr<AmultiplayerPressurePlate>> Plates;
	TArray<TObjectPtr<AmultiplayerCoopGate>> Gates;
	TArray<TObjectPtr<AmultiplayerMovingPlatform>> Platforms;
	TMap<TObjectPtr<AmultiplayerMovingPlatform>, FVector> PlatformStartLocations;
	TMap<TObjectPtr<AmultiplayerMovingPlatform>, float> PlatformMaxDistances;
	int32 CurrentPlateIndex = 0;
	int32 CurrentGateIndex = 0;
	int32 CurrentPlatformIndex = 0;
	float PhaseStartedAt = 0.0f;
	float BandwidthStartedAt = 0.0f;
	uint32 BandwidthStartBytes = 0;
	FString BandwidthProfile;
	bool bPostRestartRun = false;
};
