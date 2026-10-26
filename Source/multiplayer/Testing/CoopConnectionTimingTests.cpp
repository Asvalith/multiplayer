// Copyright Epic Games, Inc. All Rights Reserved.
// 业务 RTO 与真实 GI Timer 的边界；不伪造 NetDriver，也不把本机计时测试称为 UDP 验收。

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Network/multiplayerGameInstance.h"
#include "TimerManager.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopConnectionTimingTest, "Multiplayer.Network.ConnectionTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 手算的两个测量点验证先更新变化量、后更新平滑值；等待预算与重连间隔分别有界。
bool FCoopConnectionTimingTest::RunTest(const FString& Parameters)
{
	FmultiplayerConnectionTiming Timing;
	TestEqual(TEXT("No RTT uses the documented one-second estimate"), Timing.GetRTOSeconds(), 1.0);
	TestEqual(TEXT("Startup keeps the twenty-second business budget plus estimate"), Timing.GetConnectTimeoutSeconds(0), 21.0);
	TestEqual(TEXT("Configured backoff is not multiplied by the exponent again"), Timing.GetReconnectDelaySeconds(4.0, 2), 4.0);
	Timing.ObserveRTT(.4);
	TestTrue(TEXT("First sample initializes SRTT and half-sample variation"),
		FMath::IsNearlyEqual(Timing.SmoothedRTT, .4) && FMath::IsNearlyEqual(Timing.RTTVariation, .2));
	TestTrue(TEXT("First sample produces the hand-calculated 1.2-second estimate"), FMath::IsNearlyEqual(Timing.GetRTOSeconds(), 1.2));
	Timing.ObserveRTT(.8);
	TestTrue(TEXT("Second sample uses alpha 1/8 and beta 1/4 with the previous SRTT"),
		FMath::IsNearlyEqual(Timing.SmoothedRTT, .45) && FMath::IsNearlyEqual(Timing.RTTVariation, .25)
		&& FMath::IsNearlyEqual(Timing.GetRTOSeconds(), 1.45));
	for (double Invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
		Timing.ObserveRTT(Invalid);
	TestTrue(TEXT("Invalid measurements do not update the estimator or sample count"),
		Timing.SampleCount == 2 && FMath::IsNearlyEqual(Timing.GetRTOSeconds(), 1.45));
	TestTrue(TEXT("Retry uses measured RTO backoff when larger than its configured interval"),
		FMath::IsNearlyEqual(Timing.GetReconnectDelaySeconds(1.0, 2), 5.8));
	TestEqual(TEXT("Waiting to retry is capped at sixty seconds"), Timing.GetReconnectDelaySeconds(1.0, 8), 60.0);
	TestEqual(TEXT("Connection deadline is capped at sixty seconds including the original budget"), Timing.GetConnectTimeoutSeconds(8), 60.0);
	FmultiplayerConnectionTiming Fast, Slow;
	Fast.ObserveRTT(.001); Slow.ObserveRTT(10.0);
	TestEqual(TEXT("Small measurements cannot remove the minimum RTO margin"), Fast.GetRTOSeconds(), .25);
	TestEqual(TEXT("Large measurements cannot create an unbounded RTO"), Slow.GetRTOSeconds(), 5.0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopConnectionStateTest, "Multiplayer.Network.ConnectionStateTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 直接执行生产 GI 的状态与 Timer；缺少本地 PC 是真实失败分支，借此验预算而不启动 Travel。
// 此处不模拟服务器回执；成功后的真实 ACK 采样和状态恢复由 CombatReconnect 多进程门禁覆盖。
bool FCoopConnectionStateTest::RunTest(const FString& Parameters)
{
	TGuardValue<uint64> RestoreFrame(GFrameCounter, GFrameCounter);
	TStrongObjectPtr<UmultiplayerGameInstance> Instance(NewObject<UmultiplayerGameInstance>());
	auto* GI = Instance.Get();
	auto& Timers = GI->GetTimerManager();
	const auto Tick = [&](float Delta) { ++GFrameCounter; Timers.Tick(Delta); };
	Tick(0.f); // 激活 TimerManager；后续等待从 SetTimer 的真实注册时刻计算。
	GI->GameplayConfig.ReconnectDelaysSeconds = {.25f, .25f};
	GI->ConnectionTiming.ObserveRTT(.4);
	GI->LastServerAddress = TEXT("127.0.0.1:7777");
	const uint64 OldRevision = GI->SetConnectionState(EMultiplayerConnectionState::Connected, FText::GetEmpty());
	GI->ScheduleReconnect();
	const uint64 WaitingRevision = GI->OperationRevision;
	const FTimerHandle WaitingHandle = GI->ReconnectTimerHandle;
	const float FirstDelay = Timers.GetTimerRemaining(WaitingHandle);
	TestTrue(TEXT("Scheduling waits without consuming an attempt and uses the actual adaptive timer"),
		GI->GetConnectionState() == EMultiplayerConnectionState::ReconnectWaiting && GI->ReconnectAttempt == 0
		&& FMath::IsNearlyEqual(FirstDelay, 1.2f, .0001f));
	GI->ScheduleReconnect();
	GI->HandleConnectTimeout(OldRevision);
	TestTrue(TEXT("Repeated failure and expired timeout cannot replace or advance the current wait"),
		GI->OperationRevision == WaitingRevision && GI->ReconnectTimerHandle == WaitingHandle
		&& Timers.GetTimerRemaining(WaitingHandle) == FirstDelay && GI->ReconnectAttempt == 0);
	Tick(FirstDelay + .01f);
	TestTrue(TEXT("A real timer starts exactly one attempt; missing PC schedules the next bounded wait"),
		GI->ReconnectAttempt == 1 && GI->GetConnectionState() == EMultiplayerConnectionState::ReconnectWaiting
		&& GI->ConnectionTiming.SampleCount == 1
		&& FMath::IsNearlyEqual(Timers.GetTimerRemaining(GI->ReconnectTimerHandle), 2.4f, .0001f));
	AddExpectedMessagePlain(TEXT("DS connection failed: Reconnect attempts exhausted; connect manually to retry"),
		ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	Tick(2.41f);
	TestTrue(TEXT("Exhausting two configured attempts ends retrying and discards the obsolete endpoint estimate"),
		GI->GetConnectionState() == EMultiplayerConnectionState::Idle && GI->LastServerAddress.IsEmpty()
		&& !Timers.IsTimerActive(GI->ReconnectTimerHandle) && GI->ConnectionTiming.SampleCount == 0);
	GI->GameplayConfig.ReconnectDelaysSeconds.Reset();
	GI->LastServerAddress = TEXT("127.0.0.1:7777");
	GI->SetConnectionState(EMultiplayerConnectionState::Connected, FText::GetEmpty());
	GI->ScheduleReconnect();
	TestTrue(TEXT("Empty configured budget disables automatic reconnect without scheduling a timer"),
		GI->GetConnectionState() == EMultiplayerConnectionState::Idle && !Timers.IsTimerActive(GI->ReconnectTimerHandle));
	GI->GameplayConfig.ReconnectDelaysSeconds = {1.f};
	GI->LastServerAddress = TEXT("127.0.0.1:7777");
	GI->SetConnectionState(EMultiplayerConnectionState::Connected, FText::GetEmpty());
	GI->ScheduleReconnect();
	const uint64 BeforeShutdown = GI->OperationRevision;
	TestTrue(TEXT("Shutdown starts with an actual pending reconnect timer"), Timers.IsTimerActive(GI->ReconnectTimerHandle));
	GI->Shutdown();
	Tick(10.f);
	TestTrue(TEXT("Shutdown clears pending waits and invalidates old operations without starting another attempt"),
		GI->OperationRevision > BeforeShutdown && GI->ReconnectAttempt == 0
		&& !Timers.IsTimerActive(GI->ReconnectTimerHandle) && !Timers.IsTimerActive(GI->ConnectTimeoutHandle)
		&& !Timers.IsTimerActive(GI->ConnectionTimingTimer));
	return !HasAnyErrors();
}

#endif
