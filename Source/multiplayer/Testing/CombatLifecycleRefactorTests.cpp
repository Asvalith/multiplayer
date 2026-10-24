// Copyright Epic Games, Inc. All Rights Reserved.
// 请求回执与个人恢复的窄回归：走正式 Controller/GameMode/ASC，不冒充实际弱网传输。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatProgression.h"
#include "Components/CapsuleComponent.h"
#include "Core/multiplayerGameMode.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Player/multiplayerCoopPlayerController.h"

namespace
{
	// 独立服务器规则世界，不初始化项目 GI，不订阅全局失败，不创建窗口或触发真实 Travel。
	struct FCoopFlowWorld
	{
		UWorld* World = nullptr;
		AmultiplayerGameMode* Mode = nullptr;
		FCoopFlowWorld()
		{
			UWorld::InitializationValues Values;
			Values.CreatePhysicsScene(true).ShouldSimulatePhysics(false).EnableTraceCollision(true)
				.CreateNavigation(false).CreateAISystem(false);
			World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true,
				ERHIFeatureLevel::Num, &Values);
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			auto* Instance = NewObject<UGameInstance>(GEngine);
			World->SetGameInstance(Instance);
			Context.OwningGameInstance = Instance;
			World->GetWorldSettings()->DefaultGameMode = AmultiplayerGameMode::StaticClass();
			World->SetGameMode(FURL());
			World->InitializeActorsForPlay(FURL());
			World->SetBegunPlay(true);
			Mode = World->GetAuthGameMode<AmultiplayerGameMode>();
		}
		~FCoopFlowWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopFlowRequestClosureTest,
	"Multiplayer.Combat.Flow.RestartAdmissionAndPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopFlowRequestClosureTest::RunTest(const FString& Parameters)
{
	FCoopFlowWorld Fixture;
	auto* Controller = Fixture.World->SpawnActor<AmultiplayerCoopPlayerController>();
	if (!TestNotNull(TEXT("Real rule world exists"), Fixture.Mode)
		|| !TestNotNull(TEXT("Real local controller exists"), Controller)) return false;
	TestTrue(TEXT("Standalone controller executes the real local request entrance"), Controller->IsLocalController());
	TestFalse(TEXT("Fixture has not admitted this connection"), Controller->HasAcceptedCombatSession());
	Controller->RequestRestartCurrentRound();
	TestTrue(TEXT("A local request before admission does not occupy the interface"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);
	TestFalse(TEXT("A rejected local request does not occupy the server request"), Controller->bServerRestartPending);

	// 模拟旧客户端已发出请求，直接经过服务端接收分支；不是网络包丢失或乱序模拟。
	Controller->VictoryAction = ECoopVictoryAction::RestartPending;
	Controller->ServerRequestRestartCurrentRound_Implementation();
	TestTrue(TEXT("Server refusal completes the already-pending local request"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);
	TestFalse(TEXT("Refusal does not create a pending server request"), Controller->bServerRestartPending);

	// 一个真实请求正在等待 Travel 时，重复包不能发失败回执把它解锁。
	Controller->bCombatSessionAccepted = true;
	Controller->bServerRestartPending = true;
	Controller->VictoryAction = ECoopVictoryAction::RestartPending;
	Controller->ServerRequestRestartCurrentRound_Implementation();
	TestTrue(TEXT("Duplicate in-flight request retains the server owner"), Controller->bServerRestartPending);
	TestTrue(TEXT("Duplicate in-flight request does not unlock the actual client wait"), Controller->GetVictoryAction() == ECoopVictoryAction::RestartPending);
	Controller->NotifyRestartFailed(FText::FromString(TEXT("fixture travel refused")));
	TestFalse(TEXT("The owning failure releases server pending once"), Controller->bServerRestartPending);
	TestTrue(TEXT("The owning failure releases the local interface"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);
	Controller->NotifyRestartFailed(FText::FromString(TEXT("duplicate failure")));
	TestTrue(TEXT("Repeated failure does not reoccupy the interface"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);

	// 接纳后仍须由真实 GameMode 拒绝未终局重开，不能只靠客户端资格通过。
	Controller->RequestRestartCurrentRound();
	TestTrue(TEXT("A live round refusal returns the client to Idle"), Controller->GetVictoryAction() == ECoopVictoryAction::Idle);
	TestFalse(TEXT("A live round refusal clears the server request"), Controller->bServerRestartPending);
	TestTrue(TEXT("No rejected request schedules map travel"), Fixture.World->NextURL.IsEmpty());
	Controller->VictoryAction = ECoopVictoryAction::Leaving;
	Controller->ClientRestartFailed_Implementation(FText::FromString(TEXT("late restart failure")));
	TestTrue(TEXT("A late restart result cannot unlock a newer leave operation"), Controller->GetVictoryAction() == ECoopVictoryAction::Leaving);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopResumeAdmissionResultTest,
	"Multiplayer.Combat.Recovery.RejectedRestoreKeepsTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCoopResumeAdmissionResultTest::RunTest(const FString& Parameters)
{
	FCoopFlowWorld Fixture;
	if (!TestNotNull(TEXT("Recovery uses the production GameMode"), Fixture.Mode)) return false;
	// 仅延迟自动握手以布置待接纳 Pawn；所有授予与恢复结果仍来自正式 ClaimCombatSession。
	const auto SpawnPending = [&](const FVector& Location)
	{
		auto* Controller = Fixture.World->SpawnActor<AmultiplayerCoopPlayerController>();
		auto* Character = Fixture.World->SpawnActorDeferred<AmultiplayerCombatCharacter>(
			AmultiplayerCombatCharacter::StaticClass(), FTransform(Location), nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Controller || !Character) return static_cast<AmultiplayerCoopPlayerController*>(nullptr);
		Character->FinishSpawning(FTransform(Location));
		Character->SetCombatSessionReady(false);
		Controller->bCombatHandshakeSubmitted = true;
		Controller->Possess(Character);
		return Controller;
	};
	FGuid Ticket;
	FText Error;
	bool bRetryable = false;
	auto* Original = SpawnPending(FVector(1000, 0, 100));
	if (!TestNotNull(TEXT("Original pending controller"), Original)
		|| !TestTrue(TEXT("Fresh session is admitted by the real rule"), Fixture.Mode->ClaimCombatSession(Original, FGuid(), Ticket, Error, bRetryable))) return false;
	auto* OriginalPawn = CastChecked<AmultiplayerCombatCharacter>(Original->GetPawn());
	auto* ASC = OriginalPawn->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetHealthAttribute(), 73.f);
	ASC->SetNumericAttributeBase(UmultiplayerCombatAttributes::GetEnergyAttribute(), 37.f);
	Fixture.Mode->SaveCombatSession(Original);
	const FmultiplayerCombatResumeState SavedState = Fixture.Mode->CombatSessions.FindChecked(Ticket).State;
	TestTrue(TEXT("Capture has a genuine initialized progression profile"), SavedState.Progression.ProfileId.IsValid());
	Original->UnPossess();
	OriginalPawn->Destroy();
	Original->Destroy();

	// 注入真实成长校验会拒绝的版本，检查失败向外传播；不是让坏数据静默套默认成长。
	Fixture.Mode->CombatSessions.FindChecked(Ticket).State.Progression.Version = 0;
	auto* BadLoadout = SpawnPending(FVector(2000, 0, 100));
	if (!TestNotNull(TEXT("Loadout-refusal pending controller"), BadLoadout)) return false;
	FGuid Issued;
	TestFalse(TEXT("Rejected progression cannot be announced as restored"), Fixture.Mode->ClaimCombatSession(BadLoadout, Ticket, Issued, Error, bRetryable));
	TestFalse(TEXT("Rejected restore has no usable issued ticket"), Issued.IsValid());
	TestTrue(TEXT("Rejected restore provides an explicit reason"), !Error.IsEmpty());
	TestTrue(TEXT("Rejected restore retains the original ticket for retry"), Fixture.Mode->CombatSessions.Contains(Ticket));
	TestFalse(TEXT("Rejected restore never opens the controller admission gate"), BadLoadout->HasAcceptedCombatSession());
	TestFalse(TEXT("Rejected restore never opens the pawn action gate"), CastChecked<AmultiplayerCombatCharacter>(BadLoadout->GetPawn())->IsCombatReady());
	TestFalse(TEXT("Failed provisional server ticket is removed"), BadLoadout->ServerCombatTicket.IsValid());
	TestEqual(TEXT("Failure does not leak provisional session entries"), Fixture.Mode->CombatSessions.Num(), 1);

	Fixture.Mode->CombatSessions.FindChecked(Ticket).State = SavedState;
	auto* BadLocation = SpawnPending(FVector(3000, 0, 100));
	if (!TestNotNull(TEXT("Teleport-refusal pending controller"), BadLocation)) return false;
	auto* LocationPawn = CastChecked<AmultiplayerCombatCharacter>(BadLocation->GetPawn());
	// 使用引擎明确禁止的非可移动根，实际执行 TeleportTo 失败，不改写传送函数或伪造返回值。
	LocationPawn->GetCapsuleComponent()->SetMobility(EComponentMobility::Static);
	TestFalse(TEXT("Native teleport failure rejects the whole admission"), Fixture.Mode->ClaimCombatSession(BadLocation, Ticket, Issued, Error, bRetryable));
	TestTrue(TEXT("Teleport failure retains the old ticket and concrete reason"), Fixture.Mode->CombatSessions.Contains(Ticket) && !Error.IsEmpty());
	TestFalse(TEXT("Teleport failure does not admit the temporary pawn"), BadLocation->HasAcceptedCombatSession() || LocationPawn->IsCombatReady());
	TestTrue(TEXT("Teleport failure does not silently use the spawn point as success"), LocationPawn->GetActorLocation().Equals(FVector(3000, 0, 100)));
	TestEqual(TEXT("Teleport failure also releases the provisional session"), Fixture.Mode->CombatSessions.Num(), 1);

	auto* ValidRetry = SpawnPending(FVector(4000, 0, 100));
	if (!TestNotNull(TEXT("Valid retry pending controller"), ValidRetry)) return false;
	TestTrue(TEXT("An intact old ticket can be claimed after failed attempts"), Fixture.Mode->ClaimCombatSession(ValidRetry, Ticket, Issued, Error, bRetryable));
	auto* Restored = CastChecked<AmultiplayerCombatCharacter>(ValidRetry->GetPawn());
	TestTrue(TEXT("Successful restore rotates the ticket and clears the error"), Issued.IsValid() && Issued != Ticket && Error.IsEmpty());
	TestTrue(TEXT("Only successful restore opens both admission gates"), ValidRetry->HasAcceptedCombatSession() && Restored->IsCombatReady());
	TestEqual(TEXT("Recovery does not refill health"), Restored->GetHealth(), 73.f);
	TestEqual(TEXT("Recovery does not refill energy"), Restored->GetEnergy(), 37.f);
	TestTrue(TEXT("Successful recovery reaches the recorded position"), Restored->GetActorLocation().Equals(SavedState.Location, .1f));
	TestEqual(TEXT("Retry retains exactly one authoritative session"), Fixture.Mode->CombatSessions.Num(), 1);
	return !HasAnyErrors();
}

#endif
