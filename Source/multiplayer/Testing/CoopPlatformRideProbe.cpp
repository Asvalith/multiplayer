#include "Testing/CoopPlatformRideProbe.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Mechanisms/multiplayerMovingPlatform.h"
#include "Mechanisms/multiplayerTransporterComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"
#include "Testing/CoopRideTestCharacter.h"

namespace
{
	const FVector RideOrigin(30000, 8000, 2000);
	const FString MotionStages[] = { TEXT("MotionStand"), TEXT("MotionWalk"), TEXT("MotionReverse"), TEXT("MotionJump") };

	// 导出完整向量，并保留旧版 X 列名，使历史报告仍能用同一绘图工具读取。
	void SetVectorFields(const TSharedRef<FJsonObject>& Object, const TCHAR* Prefix, const FVector& Value)
	{
		Object->SetNumberField(FString(Prefix) + TEXT("X"), Value.X);
		Object->SetNumberField(FString(Prefix) + TEXT("Y"), Value.Y);
		Object->SetNumberField(FString(Prefix) + TEXT("Z"), Value.Z);
	}
}

void FCoopRideMovementSettings::Apply(UCharacterMovementComponent* Movement) const
{
	Movement->MaxWalkSpeed = MaxWalkSpeed;
	Movement->JumpZVelocity = JumpSpeed;
	Movement->AirControl = AirControl;
	Movement->FallingLateralFriction = AirFriction;
	Movement->BrakingDecelerationFalling = FallingBraking;
}

void ACoopPlatformRideProbe::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACoopPlatformRideProbe, MovementSettings);
}

ACoopPlatformRideProbe::ACoopPlatformRideProbe()
{
#if !UE_BUILD_SHIPPING
	PrimaryActorTick.bCanEverTick = true;
	// 统一在角色和平台均完成本帧移动后采样，避免把两个不同 Tick 时刻的位置相减。
	PrimaryActorTick.TickGroup = TG_PostPhysics;
#endif
}

void ACoopPlatformRideProbe::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
#if !UE_BUILD_SHIPPING
	if (!Subject || !Stage.StartsWith(TEXT("Motion")) || Stage == TEXT("MotionBoard")) return;
	APlayerController* PC = HasAuthority() ? Cast<APlayerController>(GetOwner()) : GetWorld()->GetFirstPlayerController();
	auto* Character = PC ? Cast<ACoopRideTestCharacter>(PC->GetCharacter()) : nullptr;
	if (!Character) return;
	auto* Movement = CastChecked<UCoopRideTestMovement>(Character->GetCharacterMovement());
	if (Rider.Get() != Character)
	{
		Rider = Character;
		// 输入在帧末逐帧提交，下一帧由原 CMC 消费；不修改平台与角色原有 Tick 依赖。
	}
	if (SampleStage != Stage)
	{
		SampleStage = Stage;
		Elapsed = RelativeTravel = MaxRelativeStep = MaxPlatformStep = TakeoffBaseSpeed = 0;
		Samples = BasedSamples = FallingSamples = PlatformUpdates = 0;
		bSegmentFinished = bMetricReported = bJumpSent = bLandedAfterJump = false;
		PreviousRelative = Character->GetActorLocation() - Subject->GetActorLocation();
		PreviousPlatform = Subject->GetActorLocation();
		InitialPlatformX = PreviousPlatform.X;
		InitialRiderX = Character->GetActorLocation().X;
		StageStartWorldSeconds = GetWorld()->GetTimeSeconds();
		TimelineSamples.Reset();
		FrozenCorrectionEvents = FrozenClientMoves = FrozenServerMoves = DroppedTimelineSamples = 0;
		InitialRelativeZ = PreviousRelative.Z;
		MaxRise = MaxFrameSeconds = CorrectionDistance = 0;
		AirSeconds = InitialAirSpeed = LaterAirSpeed = 0;
		Corrections = ComparableCorrections = BaseChanges = 0;
		Movement->ObservedPlatform = Subject;
		Movement->ResetMetrics();
	}
	if (bSegmentFinished) return;
	Elapsed += DeltaSeconds;
	MaxFrameSeconds = FMath::Max(MaxFrameSeconds, DeltaSeconds);
	const bool bBased = Character->GetMovementBase() && Character->GetMovementBase()->GetOwner() == Subject;
	const bool bFalling = Movement->IsFalling();
	++Samples;
	BasedSamples += bBased ? 1 : 0;
	FallingSamples += bFalling ? 1 : 0;
	if (bFalling)
	{
		if (AirSeconds == 0) InitialAirSpeed = FMath::Abs(Movement->Velocity.X);
		if (AirSeconds < .25f && AirSeconds + DeltaSeconds >= .25f) LaterAirSpeed = FMath::Abs(Movement->Velocity.X);
		AirSeconds += DeltaSeconds;
	}
	if (FallingSamples > 0 && bBased && !bFalling) bLandedAfterJump = true;
	const FVector Relative = Character->GetActorLocation() - Subject->GetActorLocation();
	MaxRise = FMath::Max(MaxRise, float(Relative.Z - InitialRelativeZ));
	const FVector RelativeStep = Relative - PreviousRelative;
	RelativeTravel += FMath::Abs(RelativeStep.Y);
	MaxRelativeStep = FMath::Max(MaxRelativeStep, float(RelativeStep.Size()));
	const float PlatformStep = FVector::Distance(PreviousPlatform, Subject->GetActorLocation());
	MaxPlatformStep = FMath::Max(MaxPlatformStep, PlatformStep);
	PlatformUpdates += PlatformStep > .01f ? 1 : 0;
	PreviousRelative = Relative;
	PreviousPlatform = Subject->GetActorLocation();
	if (!HasAuthority())
	{
		if (Stage == TEXT("MotionWalk") && Elapsed < 3.f)
		{
			// 横向往返，不由服务器写玩家位置；真正经过客户端预测和 ServerMove 路径。
			const float Direction = (int32(Elapsed / .5f) % 2 == 0) ? 1.f : -1.f;
			Character->AddMovementInput(FVector::YAxisVector, Direction * .12f);
		}
		if (Stage == TEXT("MotionJump") && !bJumpSent && Elapsed >= .75f)
		{
			TakeoffBaseSpeed = Movement->GetImpartedMovementBaseVelocity().Size();
			bJumpSent = true;
			Character->Jump();
		}
		if (bJumpSent && Elapsed > .9f) Character->StopJumping();
	}
	if (Stage == TEXT("MotionJump") && !HasAuthority())
	{
		if (TimelineSamples.Num() < 300)
		{
			FCoopRideTimelineSample Sample;
			Sample.WorldSeconds = GetWorld()->GetTimeSeconds();
			Sample.ElapsedSeconds = Sample.WorldSeconds - StageStartWorldSeconds;
			Sample.PlatformX = Subject->GetActorLocation().X - InitialPlatformX;
			Sample.RiderX = Character->GetActorLocation().X - InitialRiderX;
			Sample.RelativeX = Relative.X - (InitialRiderX - InitialPlatformX);
			Sample.PlatformStepCm = PlatformStep;
			Sample.PlatformVelocityX = Subject->GetVelocity().X;
			Sample.RiderVelocityX = Movement->Velocity.X;
			Sample.bBased = bBased;
			Sample.bFalling = bFalling;
			Sample.bJumpRequested = bJumpSent;
			TimelineSamples.Add(Sample);
		}
		else ++DroppedTimelineSamples;
	}
	// 四秒包括输入停止后的稳定区间；不删除首秒、不跳过校正峰值。
	if (Elapsed >= 4.f)
	{
		bSegmentFinished = true;
		// 校正指标和位移指标使用同一个采样截止时刻，不把等待下一次驱动轮询的时间算进来。
		Corrections = Movement->CorrectionCount;
		ComparableCorrections = Movement->ComparableCorrectionCount;
		BaseChanges = Movement->BaseChangeCorrectionCount;
		CorrectionDistance = Movement->MaxCorrectionCm;
		FrozenCorrectionEvents = Movement->CorrectionEvents.Num();
		FrozenClientMoves = Movement->ClientMoveEvents.Num();
		FrozenServerMoves = Movement->ServerMoveEvents.Num();
	}
#endif
}

bool ACoopPlatformRideProbe::IsSegmentValid() const
{
	if (Samples < 10 || Elapsed <= 0 || !Rider.IsValid()) return false;
	if (MovementSettings.bStationaryPlatform ? MaxPlatformStep > .01f : PlatformUpdates == 0) return false;
	const bool bCurrentlyBased = Rider->GetMovementBase() && Rider->GetMovementBase()->GetOwner() == Subject;
	if (SampleStage == TEXT("MotionJump")) return (HasAuthority() || bJumpSent) && MaxRise >= 50
		&& FallingSamples > 0 && bLandedAfterJump && bCurrentlyBased;
	if (float(BasedSamples) / Samples < .9f || !bCurrentlyBased) return false;
	if (SampleStage == TEXT("MotionWalk")) return RelativeTravel >= 30;
	if (SampleStage == TEXT("MotionReverse")) return MovementSettings.bStationaryPlatform
		|| Subject->GetActorLocation().X < InitialPlatformX - 100;
	return true;
}

TSharedRef<FJsonObject> ACoopPlatformRideProbe::MakeMetrics() const
{
	auto Metrics = MakeShared<FJsonObject>();
	Metrics->SetStringField(TEXT("category"), TEXT("platformRide"));
	Metrics->SetStringField(TEXT("phase"), SampleStage.RightChop(6));
	Metrics->SetNumberField(TEXT("frameSamples"), Samples);
	Metrics->SetNumberField(TEXT("elapsedSeconds"), Elapsed);
	Metrics->SetNumberField(TEXT("basedSamples"), BasedSamples);
	Metrics->SetNumberField(TEXT("fallingSamples"), FallingSamples);
	Metrics->SetNumberField(TEXT("relativeTravelCm"), RelativeTravel);
	Metrics->SetNumberField(TEXT("maxRelativeStepCm"), MaxRelativeStep);
	Metrics->SetNumberField(TEXT("maxPlatformStepCm"), MaxPlatformStep);
	Metrics->SetNumberField(TEXT("platformPositionChanges"), PlatformUpdates);
	Metrics->SetNumberField(TEXT("takeoffBaseSpeedCmPerSec"), TakeoffBaseSpeed);
	Metrics->SetNumberField(TEXT("maxRiseCm"), MaxRise);
	Metrics->SetNumberField(TEXT("maxFrameSeconds"), MaxFrameSeconds);
	Metrics->SetNumberField(TEXT("horizontalSpeedAtTakeoffCmPerSec"), InitialAirSpeed);
	Metrics->SetNumberField(TEXT("horizontalSpeedAfterQuarterSecondCmPerSec"), LaterAirSpeed);
	Metrics->SetBoolField(TEXT("landedAfterJump"), bLandedAfterJump);
	Metrics->SetNumberField(TEXT("correctionCount"), Corrections);
	Metrics->SetNumberField(TEXT("maxCorrectionCm"), CorrectionDistance);
	Metrics->SetNumberField(TEXT("comparableCorrections"), ComparableCorrections);
	Metrics->SetNumberField(TEXT("baseChangeCorrections"), BaseChanges);
	if (SampleStage == TEXT("MotionJump") && !HasAuthority())
	{
		Metrics->SetStringField(TEXT("timelineClock"), TEXT("Client local World seconds relative to Jump phase start; server/client clocks are not aligned"));
		Metrics->SetNumberField(TEXT("droppedTimelineSamples"), DroppedTimelineSamples);
		TArray<TSharedPtr<FJsonValue>> SamplesJson;
		for (const FCoopRideTimelineSample& Sample : TimelineSamples)
		{
			auto Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("t"), Sample.ElapsedSeconds);
			Row->SetNumberField(TEXT("platformX"), Sample.PlatformX);
			Row->SetNumberField(TEXT("riderX"), Sample.RiderX);
			Row->SetNumberField(TEXT("relativeX"), Sample.RelativeX);
			Row->SetNumberField(TEXT("platformStepCm"), Sample.PlatformStepCm);
			Row->SetNumberField(TEXT("platformVelocityX"), Sample.PlatformVelocityX);
			Row->SetNumberField(TEXT("riderVelocityX"), Sample.RiderVelocityX);
			Row->SetBoolField(TEXT("based"), Sample.bBased);
			Row->SetBoolField(TEXT("falling"), Sample.bFalling);
			Row->SetBoolField(TEXT("jumpRequested"), Sample.bJumpRequested);
			SamplesJson.Add(MakeShared<FJsonValueObject>(Row));
		}
		Metrics->SetArrayField(TEXT("timelineSamples"), MoveTemp(SamplesJson));
		TArray<TSharedPtr<FJsonValue>> CorrectionsJson;
		if (Rider.IsValid())
		{
			const auto* TestMovement = Cast<UCoopRideTestMovement>(Rider->GetCharacterMovement());
			Metrics->SetNumberField(TEXT("droppedCorrectionEvents"), TestMovement->DroppedCorrectionEvents);
			for (int32 Index = 0; Index < FrozenCorrectionEvents; ++Index)
			{
				const FCoopRideCorrectionEvent& Event = TestMovement->CorrectionEvents[Index];
				auto Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("t"), Event.ReceiptWorldSeconds - StageStartWorldSeconds);
				Row->SetNumberField(TEXT("moveTimestamp"), Event.MoveTimeStamp);
				Row->SetBoolField(TEXT("comparable"), Event.bComparable);
				Row->SetNumberField(TEXT("errorCm"), Event.ErrorCm);
				SetVectorFields(Row, TEXT("error"), Event.Error);
				SetVectorFields(Row, TEXT("clientVelocity"), Event.ClientVelocity);
				SetVectorFields(Row, TEXT("serverVelocity"), Event.ServerVelocity);
				Row->SetBoolField(TEXT("savedOnBase"), Event.bSavedOnBase);
				Row->SetBoolField(TEXT("serverOnBase"), Event.bServerOnBase);
				Row->SetStringField(TEXT("savedBase"), Event.SavedBase);
				Row->SetStringField(TEXT("serverBase"), Event.ServerBase);
				CorrectionsJson.Add(MakeShared<FJsonValueObject>(Row));
			}
		}
		Metrics->SetArrayField(TEXT("correctionEvents"), MoveTemp(CorrectionsJson));
	}
	if (SampleStage == TEXT("MotionJump") && Rider.IsValid())
	{
		const auto* TestMovement = CastChecked<UCoopRideTestMovement>(Rider->GetCharacterMovement());
		if (HasAuthority())
		{
			Metrics->SetNumberField(TEXT("droppedServerMoves"), TestMovement->DroppedServerMoves);
			TArray<TSharedPtr<FJsonValue>> MovesJson;
			for (int32 Index = 0; Index < FrozenServerMoves; ++Index)
			{
				const FCoopRideServerMoveEvent& Event = TestMovement->ServerMoveEvents[Index];
				auto Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("moveTimestamp"), Event.MoveTimeStamp);
				Row->SetNumberField(TEXT("serverWorldSeconds"), Event.ServerWorldSeconds);
				SetVectorFields(Row, TEXT("server"), Event.Server);
				SetVectorFields(Row, TEXT("serverRelative"), Event.ServerRelative);
				SetVectorFields(Row, TEXT("serverVelocity"), Event.ServerVelocity);
				SetVectorFields(Row, TEXT("reported"), Event.Reported);
				SetVectorFields(Row, TEXT("platform"), Event.Platform);
				SetVectorFields(Row, TEXT("platformVelocity"), Event.PlatformVelocity);
				Row->SetNumberField(TEXT("deltaSeconds"), Event.DeltaSeconds);
				Row->SetBoolField(TEXT("serverBased"), Event.bServerBased);
				Row->SetBoolField(TEXT("reportedBased"), Event.bReportedBased);
				Row->SetBoolField(TEXT("serverFalling"), Event.bServerFalling);
				Row->SetBoolField(TEXT("reportedFalling"), Event.bReportedFalling);
				Row->SetStringField(TEXT("serverBase"), Event.ServerBase);
				Row->SetStringField(TEXT("reportedBase"), Event.ReportedBase);
				MovesJson.Add(MakeShared<FJsonValueObject>(Row));
			}
			Metrics->SetArrayField(TEXT("serverMoveEvents"), MoveTemp(MovesJson));
		}
		else
		{
			Metrics->SetNumberField(TEXT("droppedClientMoves"), TestMovement->DroppedClientMoves);
			TArray<TSharedPtr<FJsonValue>> MovesJson;
			for (int32 Index = 0; Index < FrozenClientMoves; ++Index)
			{
				const FCoopRideClientMoveEvent& Event = TestMovement->ClientMoveEvents[Index];
				auto Row = MakeShared<FJsonObject>();
				Row->SetNumberField(TEXT("moveTimestamp"), Event.MoveTimeStamp);
				Row->SetNumberField(TEXT("clientWorldSeconds"), Event.ClientWorldSeconds);
				SetVectorFields(Row, TEXT("start"), Event.Start);
				SetVectorFields(Row, TEXT("end"), Event.End);
				SetVectorFields(Row, TEXT("relative"), Event.Relative);
				SetVectorFields(Row, TEXT("startVelocity"), Event.StartVelocity);
				SetVectorFields(Row, TEXT("endVelocity"), Event.EndVelocity);
				SetVectorFields(Row, TEXT("platform"), Event.Platform);
				SetVectorFields(Row, TEXT("platformVelocity"), Event.PlatformVelocity);
				Row->SetNumberField(TEXT("deltaSeconds"), Event.DeltaSeconds);
				Row->SetNumberField(TEXT("startMovementMode"), Event.StartMovementMode);
				Row->SetNumberField(TEXT("endMovementMode"), Event.EndMovementMode);
				Row->SetBoolField(TEXT("startBased"), Event.bStartBased);
				Row->SetBoolField(TEXT("endBased"), Event.bEndBased);
				Row->SetBoolField(TEXT("jumpPressed"), Event.bJumpPressed);
				Row->SetStringField(TEXT("startBase"), Event.StartBase);
				Row->SetStringField(TEXT("endBase"), Event.EndBase);
				MovesJson.Add(MakeShared<FJsonValueObject>(Row));
			}
			Metrics->SetArrayField(TEXT("clientMoveEvents"), MoveTemp(MovesJson));
		}
	}
	Metrics->SetStringField(TEXT("correctionScope"), HasAuthority() ? TEXT("Not applicable: server does not receive client corrections") : TEXT("CMC accepted corrections; same historical move in comparable coordinate space; base changes counted separately"));
	int32 Hz = 30;
	FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformNetHz="), Hz);
	FString SyncMode = TEXT("PlatformInertia");
	FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformSyncMode="), SyncMode);
	Metrics->SetNumberField(TEXT("platformNetHz"), Hz);
	Metrics->SetStringField(TEXT("syncMode"), SyncMode);
	Metrics->SetNumberField(TEXT("fallingBraking"), Rider.IsValid() ? Rider->GetCharacterMovement()->BrakingDecelerationFalling : -1);
	Metrics->SetStringField(TEXT("platformMotion"), MovementSettings.bStationaryPlatform ? TEXT("Static") : TEXT("Moving"));
	return Metrics;
}

// 场景只搭建一次初始位置，运动阶段不传送玩家、不关 CMC 校正，也不抬高校正容差。
void UCoopNetTestDriver::MotionServerTick(UWorld* World)
{
	auto* MotionProbe = Cast<ACoopPlatformRideProbe>(Probe.Get());
	if (!MotionProbe) { Finish(false, TEXT("Missing motion probe")); return; }
	auto* PC = Cast<APlayerController>(MotionProbe->GetOwner());
	if (!PC) return;
	if (Phase == TEXT("ScenarioStart"))
	{
		APawn* PreviousPawn = PC->GetPawn();
		FActorSpawnParameters Parameters;
		Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		auto* Rider = World->SpawnActor<ACoopRideTestCharacter>(RideOrigin + FVector(0, 0, 115), FRotator::ZeroRotator, Parameters);
		auto* Platform = World->SpawnActor<AmultiplayerMovingPlatform>(RideOrigin, FRotator::ZeroRotator, Parameters);
		if (!Rider || !Platform) { Finish(false, TEXT("Could not construct motion fixture")); return; }
		// 读取实际关卡生成的角色配置，防止蓝图覆盖被测试专用 Pawn 的 C++ 默认值掩盖。
		if (const ACharacter* PreviousCharacter = Cast<ACharacter>(PreviousPawn))
		{
			const UCharacterMovementComponent* Original = PreviousCharacter->GetCharacterMovement();
			MotionProbe->MovementSettings.MaxWalkSpeed = Original->MaxWalkSpeed;
			MotionProbe->MovementSettings.JumpSpeed = Original->JumpZVelocity;
			MotionProbe->MovementSettings.AirControl = Original->AirControl;
			MotionProbe->MovementSettings.AirFriction = Original->FallingLateralFriction;
			MotionProbe->MovementSettings.FallingBraking = Original->BrakingDecelerationFalling;
		}
		FString SyncMode = TEXT("PlatformInertia");
		FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformSyncMode="), SyncMode);
		FString PlatformMotion = TEXT("Moving");
		FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformMotion="), PlatformMotion);
		MotionProbe->MovementSettings.bStationaryPlatform = PlatformMotion == TEXT("Static");
		if (SyncMode == TEXT("Baseline") || SyncMode == TEXT("OrderedVelocity"))
		{
			MotionProbe->MovementSettings.FallingBraking = 1500;
		}
		MotionProbe->MovementSettings.bReady = true;
		MotionProbe->MovementSettings.Apply(Rider->GetCharacterMovement());
		PC->Possess(Rider);
		if (PreviousPawn) PreviousPawn->Destroy();
		Platform->bAlwaysRelevant = true;
		Platform->FindComponentByClass<UBoxComponent>()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Platform->FindComponentByClass<UmultiplayerTransporterComponent>()->ConfigureWorldTargets(RideOrigin, RideOrigin + FVector(10000, 0, 0));
		int32 Hz = 30;
		FParse::Value(FCommandLine::Get(), TEXT("CoopPlatformNetHz="), Hz);
		Platform->SetNetUpdateFrequency(FMath::Clamp(Hz, 1, 60));
		Platform->SetMinNetUpdateFrequency(FMath::Clamp(Hz, 1, 60));
		Probe->Subject = Platform;
		SetCommand(TEXT("MotionBoard"));
	}
	if (Phase == TEXT("MotionBoard") && Receipts.Contains(Phase))
	{
		if (!MotionProbe->MovementSettings.bStationaryPlatform)
			Probe->Subject->FindComponentByClass<UmultiplayerTransporterComponent>()->SetTransportActive(true);
		SetCommand(MotionStages[0]);
	}
	if (MotionProbe->IsSegmentFinished() && !MotionProbe->bMetricReported)
	{
		MotionProbe->bMetricReported = true;
		auto Extra = MakeShared<FJsonObject>();
		Extra->SetObjectField(TEXT("metrics"), MotionProbe->MakeMetrics());
		Emit(TEXT("METRIC"), TEXT("Server observation; not a visual acceptance test"), Extra);
	}
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(MotionStages); ++Index)
	{
		if (Phase != MotionStages[Index] || !Receipts.Contains(Phase) || !MotionProbe->bMetricReported) continue;
		// 服务端也必须观察到实际行走/起跳/落地；客户端自报成功不能替代这个检查。
		Assert(TEXT("Server") + Phase, MotionProbe->IsSegmentValid(), TEXT("Authoritative movement observed without fixture teleports"));
		if (bDone) return;
		if (Index + 1 == UE_ARRAY_COUNT(MotionStages))
		{
			Assert(TEXT("MotionServerObserved"), true, TEXT("All four authoritative motion segments validated"));
			SetCommand(TEXT("Complete"));
		}
		else
		{
			const FString Next = MotionStages[Index + 1];
			if (Next == TEXT("MotionReverse")) Probe->Subject->FindComponentByClass<UmultiplayerTransporterComponent>()->SetTransportActive(false);
			if (Next == TEXT("MotionJump") && !MotionProbe->MovementSettings.bStationaryPlatform)
				Probe->Subject->FindComponentByClass<UmultiplayerTransporterComponent>()->SetTransportActive(true);
			SetCommand(Next);
		}
		break;
	}
	if (Phase == TEXT("Complete") && Receipts.Contains(TEXT("Complete"))) Finish(true);
}

void UCoopNetTestDriver::MotionClientTick(UWorld* World)
{
	auto* MotionProbe = Cast<ACoopPlatformRideProbe>(Probe.Get());
	auto* PC = World->GetFirstPlayerController();
	auto* Rider = PC ? Cast<ACoopRideTestCharacter>(PC->GetCharacter()) : nullptr;
	if (!MotionProbe || !Rider || !Probe->Subject || !MotionProbe->MovementSettings.bReady) return;
	if (Phase != Probe->Stage) SetPhase(Probe->Stage);
	if (Probe->Stage == TEXT("MotionBoard"))
	{
		MotionProbe->MovementSettings.Apply(Rider->GetCharacterMovement());
		if (Rider->GetMovementBase() && Rider->GetMovementBase()->GetOwner() == Probe->Subject)
			SendReceipt(TEXT("MotionBoard"), true, TEXT("Possessed test character resolves actual platform base"));
		return;
	}
	if (MotionProbe->IsSegmentFinished() && !MotionProbe->bMetricReported)
	{
		MotionProbe->bMetricReported = true;
		auto Extra = MakeShared<FJsonObject>();
		Extra->SetObjectField(TEXT("metrics"), MotionProbe->MakeMetrics());
		Emit(TEXT("METRIC"), TEXT("Per-frame client motion; no pixel or camera smoothness claim"), Extra);
		SendReceipt(Probe->Stage, MotionProbe->IsSegmentValid(), TEXT("Actual CMC movement, platform base and jump landing checked"));
	}
	if (Probe->Stage == TEXT("Complete") && !SentReceipts.Contains(TEXT("Complete")))
	{
		SendReceipt(TEXT("Complete"), true, TEXT("Dynamic ride scenario complete"));
		Finish(true);
	}
}
