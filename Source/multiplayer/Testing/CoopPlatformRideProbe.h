#pragma once

#include "CoreMinimal.h"
#include "Testing/CoopNetTestDriver.h"
#include "CoopPlatformRideProbe.generated.h"

class ACoopRideTestCharacter;
class UCharacterMovementComponent;

/** 把实际蓝图角色的关键移动参数带给测试两端，避免只在服务器复制配置造成伪校正。 */
USTRUCT()
struct FCoopRideMovementSettings
{
	GENERATED_BODY()
	UPROPERTY() float MaxWalkSpeed = 500;
	UPROPERTY() float JumpSpeed = 700;
	UPROPERTY() float AirControl = .35f;
	UPROPERTY() float AirFriction = 0;
	UPROPERTY() float FallingBraking = 0;
	UPROPERTY() bool bReady = false;
	void Apply(UCharacterMovementComponent* Movement) const;
};

/** 动态载人实验的逐帧采样器。只有显式 RideMotion 测试会生成，不进入普通关卡流程。 */
UCLASS(NotBlueprintable, Transient)
class ACoopPlatformRideProbe : public ACoopNetTestProbe
{
	GENERATED_BODY()
public:
	ACoopPlatformRideProbe();
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated) FCoopRideMovementSettings MovementSettings;
	bool IsSegmentFinished() const { return bSegmentFinished && SampleStage == Stage; }
	bool IsSegmentValid() const;
	TSharedRef<FJsonObject> MakeMetrics() const;
	bool bMetricReported = false;

private:
	TWeakObjectPtr<ACoopRideTestCharacter> Rider;
	FString SampleStage;
	FVector PreviousRelative = FVector::ZeroVector;
	FVector PreviousPlatform = FVector::ZeroVector;
	float Elapsed = 0, RelativeTravel = 0, MaxRelativeStep = 0, MaxPlatformStep = 0;
	float TakeoffBaseSpeed = 0, InitialPlatformX = 0;
	float InitialRelativeZ = 0, MaxRise = 0, MaxFrameSeconds = 0, CorrectionDistance = 0;
	float AirSeconds = 0, InitialAirSpeed = 0, LaterAirSpeed = 0;
	int32 Samples = 0, BasedSamples = 0, FallingSamples = 0, PlatformUpdates = 0;
	int32 Corrections = 0, ComparableCorrections = 0, BaseChanges = 0;
	bool bSegmentFinished = false, bJumpSent = false, bLandedAfterJump = false;
};
