#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Network/multiplayerGameInstance.h"
#include "Player/multiplayerCoopPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopConnectionStateTest, "Coop.Connection.State",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopConnectionStateTest::RunTest(const FString&)
{
	FString Address;
	TestTrue(TEXT("IPv4 endpoint"), UmultiplayerGameInstance::NormalizeServerAddress(TEXT(" 127.0.0.1:7777 "), Address));
	TestEqual(TEXT("Normalized endpoint"), Address, FString(TEXT("127.0.0.1:7777")));
	for (const FString Bad : {TEXT("127.0.0.1:0"), TEXT("host:65536"), TEXT("host:7777?listen"), TEXT("/Game/Map"), TEXT("host:abc"), TEXT("host..name:7777")})
		TestFalse(*Bad, UmultiplayerGameInstance::NormalizeServerAddress(Bad, Address));
	auto* GI = NewObject<UmultiplayerGameInstance>(GEngine);
	GI->AddToRoot();
	GI->InitializeStandalone(FName(TEXT("CoopDSStateTest")));
	// 拒绝重叠请求不覆盖旧地址；取消通知中的同步重入不能把旧连接重新发出去。
	GI->ConnectionState = EMultiplayerConnectionState::ReconnectWaiting;
	GI->LastServerAddress = TEXT("localhost:7777");
	TestFalse(TEXT("Busy request refused"), GI->ConnectToServer(TEXT("other:7778")));
	TestEqual(TEXT("Address retained"), GI->LastServerAddress, FString(TEXT("localhost:7777")));
	GI->ConnectionState = EMultiplayerConnectionState::Idle;
	const auto Handle = GI->OnConnectionChanged.AddLambda([GI]()
	{
		if (GI->ConnectionState == EMultiplayerConnectionState::Connecting)
		{
			GI->SetConnectionState(EMultiplayerConnectionState::Leaving, FText::GetEmpty());
		}
	});
	GI->StartTravel(false);
	TestFalse(TEXT("Canceled before travel deadline armed"), GI->GetTimerManager().IsTimerActive(GI->ConnectTimeoutHandle));
	TestTrue(TEXT("Cancel state retained"), GI->ConnectionState == EMultiplayerConnectionState::Leaving);
	GI->OnConnectionChanged.Remove(Handle);
	// 连接成功会使之前的超时失效，旧回调不能清掉新连接的地址或状态。
	const uint64 StaleRevision = GI->OperationRevision;
	GI->SetConnectionState(EMultiplayerConnectionState::Connected, FText::GetEmpty());
	GI->HandleConnectTimeout(StaleRevision);
	TestTrue(TEXT("Stale deadline preserves connected state"), GI->ConnectionState == EMultiplayerConnectionState::Connected);
	TestEqual(TEXT("Stale deadline preserves address"), GI->LastServerAddress, FString(TEXT("localhost:7777")));
	GI->ConnectionState = EMultiplayerConnectionState::Reconnecting;
	GI->ReconnectAttempt = GI->GameplayConfig.ReconnectDelaysSeconds.Num();
	GI->ScheduleReconnect();
	TestTrue(TEXT("Bounded reconnect returns idle"), GI->ConnectionState == EMultiplayerConnectionState::Idle && GI->LastServerAddress.IsEmpty());
	TestFalse(TEXT("Other world failure ignored"), GI->OwnsFailure(nullptr, nullptr));
	// 强制退出也必须先占用 Leaving；直接调用项目故障处理，不广播引擎全局错误。
	// Dummy World 尚未进入玩法，必须初始化 Actor 才会执行 RPC 和完整销毁生命周期。
	GI->GetWorld()->InitializeActorsForPlay(FURL());
	auto* Controller = GI->GetWorld()->SpawnActor<AmultiplayerCoopPlayerController>();
	if (TestNotNull(TEXT("Request controller"), Controller))
	{
		Controller->DispatchBeginPlay();
		bool bInjectedLeaveFailure = false;
		const auto FailLeaveHandle = GI->OnConnectionChanged.AddLambda([this, GI, Controller, &bInjectedLeaveFailure]()
		{
			if (GI->ConnectionState != EMultiplayerConnectionState::Leaving) return;
			bInjectedLeaveFailure = true;
			TestTrue(TEXT("Forced leave updates request state first"), Controller->GetVictoryAction() == ECoopVictoryAction::Leaving);
			GI->HandleTravelFailure(GI->GetWorld(), ETravelFailure::ClientTravelFailure, TEXT("Synthetic return-to-menu failure"));
		});
		Controller->ClientReturnToMainMenuWithTextReason(FText::FromString(TEXT("Test server return request")));
		TestTrue(TEXT("Forced leave failure reached"), bInjectedLeaveFailure);
		TestTrue(TEXT("Forced leave failure restores Idle"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);
		GI->OnConnectionChanged.Remove(FailLeaveHandle);
		Controller->Destroy();
		TestFalse(TEXT("Controller EndPlay removes leave subscription"), GI->OnLeaveFailed.IsBound());
	}
	GI->Shutdown();
	if (UWorld* World = GI->GetWorld()) { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
	GI->RemoveFromRoot();
	return true;
}
#endif
