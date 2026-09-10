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
	// 进程内保存重开标记，使旧 World 销毁后生成的新驱动能继续退出检查。
	// 当前脚本每个实例使用独立进程；这些静态字段没有为同进程多 PIE World 做隔离。
	bool bRestartIssued = false;
	FString RestartedMap;
	constexpr float SettleSeconds = 1.0f;
	constexpr float PlatformArrivalTimeoutSeconds = 15.0f;
}

AmultiplayerCoopTestDriver::AmultiplayerCoopTestDriver()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	bReplicates = false;
}

/**
 * 测试启动只做一次关卡对象收集。重开之后直接进入退出阶段，避免重新跑一遍玩法或在菜单中误测。
 * 本对象的服务器生成限制在 GameMode；非复制 Actor 的 HasAuthority 本身不能替代该生成约束。
 */
void AmultiplayerCoopTestDriver::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority())
	{
		Destroy();
		return;
	}

	const FString CurrentMap = GetWorld()->GetOutermost()->GetName();
	const bool bPostRestartRun = MultiplayerCoopAutomation::bRestartIssued
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
			// 对照组只改变平台快照上限，使用相同关卡与测试步骤；实际耗时允许波动，采样按时长归一。
			It->SetNetUpdateFrequency(100.0f);
		}
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

/**
 * 将“触发操作”和“检查结果”放在不同阶段，给 Overlap、状态复制和重开流程留出处理时间。
 * 0.1 秒轮询仅用于开发测试；Finished 还需等待可选重开，Failed 则由 Fail 关闭 Tick。
 */
void AmultiplayerCoopTestDriver::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Now = GetWorld()->GetTimeSeconds();
	if (Phase == ETestPhase::WaitForPlatform
		&& Platforms.IsValidIndex(CurrentPlatformIndex))
	{
		const AmultiplayerMovingPlatform* Platform = Platforms[CurrentPlatformIndex];
		if (IsValid(Platform))
		{
			CurrentPlatformMaxDistance = FMath::Max(
				CurrentPlatformMaxDistance,
				FVector::Dist(Platform->GetActorLocation(), CurrentPlatformStartLocation));
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
			// 从两名 Pawn 就绪开始取样，排除进程启动和初次地图连接这段不同长度的准备时间。
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
			// 先留下跨 World 标记再请求重开，否则新 World 的测试驱动无法分辨这是第二阶段。
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

/** 控制器存在不等于角色已经生成；以成功取得 Character 作为本测试可移动玩家的前提。 */
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

/**
 * 直接测试服务器登记接口：首次调用应成功，同一对象再次提交必须失败，最后核对共享进度。
 * 按 Actor 名称排序只保证当前测试配对可重复，没有读取钥匙预绑定插槽来重放完整玩家操作。
 */
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
	// 先测平台，避免后续遍历全部压力板时提前触发同一外部板，让平台阶段产生假阳性。
	CurrentPlatformIndex = 0;
	ActivateNextPlatform();
}

/** 触发板后先记录等待阶段；不在同一调用栈立即断言，以免把事件处理先后误当作逻辑故障。 */
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

/** 当前只验证板被激活，未覆盖多人同时进出及激活保持时占用成员变化的全部组合。 */
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

/** 通过门的只读依赖接口布置触发条件，使测试随关卡配置变化，而不是写死某两个板名。 */
void AmultiplayerCoopTestDriver::ActivateNextGate()
{
	if (CurrentGateIndex >= Gates.Num())
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: pressure plate and gate flow passed."));
		EnterWinArea();
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

/** 只有权威开门条件成立才进入下一扇门；视觉插值完成与否不作为此阶段的通过条件。 */
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

/** 每次仅激活一个平台；配置缺失或人数无法满足时直接报错，不通过修改规则使测试通过。 */
void AmultiplayerCoopTestDriver::ActivateNextPlatform()
{
	if (CurrentPlatformIndex >= Platforms.Num())
	{
		UE_LOG(LogMultiplayer, Log, TEXT("Coop automation: server-driven moving platform flow passed."));
		CurrentPlateIndex = 0;
		ActivateNextPlate();
		return;
	}

	AmultiplayerMovingPlatform* Platform = Platforms[CurrentPlatformIndex];
	if (!IsValid(Platform))
	{
		Fail(TEXT("A moving platform was destroyed during the test."));
		return;
	}

	// 只跟踪当前平台在本阶段的位移，避免每个测试 Tick 遍历关卡中的全部平台。
	CurrentPlatformStartLocation = Platform->GetActorLocation();
	CurrentPlatformMaxDistance = 0.0f;

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

/**
 * “移动过”与“到终点”同时满足才通过，防止小幅移动或初始恰在终点造成假通过。
 * 这里只检查服务器平台位置，不证明乘客承载、沿路碰撞和客户端运动表现全部正确。
 */
void AmultiplayerCoopTestDriver::ValidateCurrentPlatform()
{
	AmultiplayerMovingPlatform* Platform = Platforms[CurrentPlatformIndex];
	if (!IsValid(Platform))
	{
		Fail(TEXT("A moving platform was unavailable while validating its configured path."));
		return;
	}

	if (CurrentPlatformMaxDistance >= 5.0f && Platform->HasReachedActiveTarget())
	{
		++CurrentPlatformIndex;
		ActivateNextPlatform();
		return;
	}

	if (GetWorld()->GetTimeSeconds() - PhaseStartedAt
		< MultiplayerCoopAutomation::PlatformArrivalTimeoutSeconds)
	{
		return;
	}

	Fail(TEXT("A moving platform did not reach its configured active endpoint."));
}

/** 将两名角色放到同一胜利区的不同位置，由真实区域统计向 GameMode 请求胜利判定。 */
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

/** 该通过标记仅确认本驱动检查的服务器流程，外部脚本继续检查客户端 UI、重开及菜单到达。 */
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

/**
 * 用计数器差值除以实际经过的时间，得到同一玩法时段的平均发送速率。
 * 对照结果需由脚本匹配 Baseline/Optimized；该值不是单个 Actor 带宽，也不代表公网固定收益。
 */
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

/** 失败后停止推进，避免前一断言失败后又打印后续成功日志；保留错误原因供外部脚本立即终止。 */
void AmultiplayerCoopTestDriver::Fail(const FString& Reason)
{
	Phase = ETestPhase::Failed;
	SetActorTickEnabled(false);
	UE_LOG(LogMultiplayer, Error, TEXT("Coop automation failed: %s"), *Reason);
}

/** 直接传送到测试位置且不做路径 Sweep，用来构造触发条件；不把这次位移作为正常行走的证明。 */
void AmultiplayerCoopTestDriver::MovePlayerTo(
	ACharacter* Character,
	const FVector& Location) const
{
	if (Character != nullptr)
	{
		Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	}
}
