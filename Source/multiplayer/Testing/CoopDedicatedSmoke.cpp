#include "Testing/CoopNetTestDriver.h"

#include "Core/multiplayerCoopGameState.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"

// 只验证 DS 拓扑和双端复制入口，不冒充机关、弱网或视觉平滑验收。
void UCoopNetTestDriver::DedicatedTick(UWorld* World)
{
	AmultiplayerCoopGameState* State = World->GetGameState<AmultiplayerCoopGameState>();
	if (bServer)
	{
		if (Phase == TEXT("Boot"))
		{
			Assert(TEXT("DedicatedAuthority"), World->GetNetMode() == NM_DedicatedServer
				&& World->GetAuthGameMode() && State && State->HasAuthority()
				&& GameInstance->GetLocalPlayers().IsEmpty(),
				TEXT("Dedicated server, authoritative GameMode/GameState, zero local players"));
			if (bDone) return;
			SetPhase(TEXT("WaitingForTwoClients"));
			Emit(TEXT("READY"));
		}
		if (Phase == TEXT("WaitingForTwoClients"))
		{
			TArray<APlayerController*> Players;
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				APlayerController* PC = It->Get();
				if (PC && !PC->IsLocalController() && PC->GetCharacter()) Players.Add(PC);
			}
			if (Players.Num() < 2) return;
			Assert(TEXT("TwoRemotePlayers"), Players.Num() == 2 && State->PlayerArray.Num() == 2);
			if (bDone) return;
			for (APlayerController* PC : Players)
			{
				FActorSpawnParameters Params;
				Params.Owner = PC;
				ACoopNetTestProbe* NewProbe = World->SpawnActor<ACoopNetTestProbe>(Params);
				if (!NewProbe) { Finish(false, TEXT("Cannot spawn client receipt probe")); return; }
				NewProbe->Stage = TEXT("Ready");
				DedicatedProbes.Add(NewProbe);
			}
			SetPhase(TEXT("WaitingForReceipts"));
		}
		const auto BothReceived = [this](const FString& Name)
		{
			if (DedicatedProbes.Num() != 2) return false;
			for (const auto& Entry : DedicatedProbes)
				if (!Entry.IsValid() || !Receipts.Contains(Name + TEXT(":") + Entry->GetName())) return false;
			return true;
		};
		if (Phase == TEXT("WaitingForReceipts") && BothReceived(TEXT("DedicatedClientReady")))
		{
			Assert(TEXT("TwoClientReceipts"), true, TEXT("Both owned RPC paths returned independently"));
			for (const auto& Entry : DedicatedProbes)
			{
				Entry->Stage = TEXT("Complete");
				Entry->ForceNetUpdate();
			}
			SetPhase(TEXT("WaitingForCompletion"));
		}
		if (Phase == TEXT("WaitingForCompletion") && BothReceived(TEXT("DedicatedClientComplete"))) Finish(true);
		return;
	}

	// 直连尚未完成时可以暂处于 Standalone 过渡 World，不能在此调用 LAN 搜房。
	if (World->GetNetMode() != NM_Client) return;
	APlayerController* Local = World->GetFirstPlayerController();
	if (!Local || !Local->IsLocalController() || !Local->GetCharacter() || !State) return;
	if (!Probe.IsValid())
	{
		for (TActorIterator<ACoopNetTestProbe> It(World); It; ++It)
			if (It->GetOwner() == Local) { Probe = *It; break; }
	}
	if (!Probe.IsValid()) return;
	if (Probe->Stage == TEXT("Ready") && !SentReceipts.Contains(TEXT("DedicatedClientReady")))
	{
		bool bSawRemoteCharacter = false;
		for (TActorIterator<ACharacter> It(World); It; ++It)
			if (*It != Local->GetCharacter() && It->GetLocalRole() == ROLE_SimulatedProxy) bSawRemoteCharacter = true;
		// 等复制对象实际就绪，不用命令行中的人数或“连接成功”日志代替客户端证据。
		if (State->PlayerArray.Num() != 2 || !bSawRemoteCharacter || State->GetObjectiveState().RequiredKeys <= 0) return;
		Assert(TEXT("DedicatedClientState"), !World->GetAuthGameMode() && !State->HasAuthority()
			&& GameInstance->GetLocalPlayers().Num() == 1
			&& Local->GetCharacter()->GetLocalRole() == ROLE_AutonomousProxy,
			TEXT("Own autonomous pawn, remote simulated pawn, two PlayerStates and nonzero replicated objective"));
		if (bDone) return;
		SentReceipts.Add(TEXT("DedicatedClientReady"));
		Probe->ServerReceipt(TEXT("DedicatedClientReady"), true, TEXT("Replicated state observed on this connection"));
	}
	if (Probe->Stage == TEXT("Complete"))
	{
		Probe->ServerReceipt(TEXT("DedicatedClientComplete"), true, TEXT("Server completion barrier observed"));
		Finish(true);
	}
}
