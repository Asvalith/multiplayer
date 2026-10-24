#pragma once

#include "CoreMinimal.h"
#include "Testing/CoopNetTestDriver.h"
#include "CoopPlatformRideProbe.generated.h"

class ACoopRideTestCharacter;
class UCharacterMovementComponent;

/** Jump 阶段的客户端本地观察点；位置均相对本阶段首帧，避免世界坐标噪声。 */
struct FCoopRideTimelineSample
{
	float WorldSeconds = 0;
	float ElapsedSeconds = 0;
	float PlatformX = 0;
	float RiderX = 0;
	float RelativeX = 0;
	float PlatformStepCm = 0;
	float PlatformVelocityX = 0;
	float RiderVelocityX = 0;
	bool bBased = false;
	bool bFalling = false;
	bool bJumpRequested = false;
};

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
	// 静止对照只关闭平台运动，其余角色参数和输入不变。
	UPROPERTY() bool bStationaryPlatform = false;
	UPROPERTY() bool bReady = false;
	// 将原蓝图参数用于本端实验 CMC，服务器与所属客户端必须一致。
	void Apply(UCharacterMovementComponent* Movement) const;
};

/** 动态载人实验的逐帧采样器。只有显式 RideMotion 测试会生成，不进入普通关卡流程。 */
UCLASS(NotBlueprintable, Transient)
class ACoopPlatformRideProbe : public ACoopNetTestProbe
{
	GENERATED_BODY()
public:
	// 开发期在 PostPhysics 逐帧采样，不在普通关卡自动生成。
	ACoopPlatformRideProbe();
	// 驱动当前实验输入并采样实际运动，阶段结束后冻结统计。
	virtual void Tick(float DeltaSeconds) override;
	// 同步实验参数，采样历史仍由每端独立记录。
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	UPROPERTY(Replicated) FCoopRideMovementSettings MovementSettings;
	// 只认可当前复制阶段对应的采样已结束，旧阶段结果不能推进新阶段。
	bool IsSegmentFinished() const { return bSegmentFinished && SampleStage == Stage; }
	// 检查当前阶段真实移动/起跳/落地是否满足实验条件。
	bool IsSegmentValid() const;
	// 输出本端同一采样窗口的指标及有界历史，不宣称视觉流畅度通过。
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
	float StageStartWorldSeconds = 0, InitialRiderX = 0;
	TArray<FCoopRideTimelineSample> TimelineSamples;
	int32 FrozenCorrectionEvents = 0, FrozenClientMoves = 0, FrozenServerMoves = 0, DroppedTimelineSamples = 0;
	int32 Samples = 0, BasedSamples = 0, FallingSamples = 0, PlatformUpdates = 0;
	int32 Corrections = 0, ComparableCorrections = 0, BaseChanges = 0;
	bool bSegmentFinished = false, bJumpSent = false, bLandedAfterJump = false;
};
