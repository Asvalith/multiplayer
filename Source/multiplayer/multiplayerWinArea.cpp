// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerWinArea.h"

#include "Components/BoxComponent.h"
#include "multiplayerCoopGameState.h"
#include "multiplayerGameMode.h"
#include "multiplayerLog.h"
#include "multiplayerPlayerOccupancyComponent.h"

AmultiplayerWinArea::AmultiplayerWinArea()
{
	PrimaryActorTick.bCanEverTick = false;
	// 区域只在服务器做规则检测，客户端需要的是 GameState 中的胜利结果，而不是触发体本身。
	bReplicates = false;

	WinTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("WinTrigger"));
	SetRootComponent(WinTrigger);
	WinTrigger->SetBoxExtent(FVector(150.0f));
	WinTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	WinTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	// 只关注 Pawn，具体是否玩家控制以及多碰撞体去重由 PlayerOccupancy 统一处理。
	WinTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy =
		CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(
			TEXT("PlayerOccupancy"));
}

void AmultiplayerWinArea::BeginPlay()
{
	Super::BeginPlay();

	// 人数和目标完成顺序不确定，因此同时监听两种变化并在任一变化后重新读取当前状态。
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(WinTrigger);

	if (!HasAuthority())
	{
		return;
	}

	// WinArea 只读 GameState 目标，不直接写 bGameWon；最终写权限仍在服务器 GameMode。
	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}
	EvaluateWinCondition();
}

void AmultiplayerWinArea::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// 先解绑自身组件与外部 GameState，再调用父类 EndPlay，避免销毁过程中收到迟到通知。
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}

	Super::EndPlay(EndPlayReason);
}

void AmultiplayerWinArea::HandleOccupancyChanged(int32 PlayerCount)
{
	// 不缓存事件参数，统一读取 PlayerOccupancy 的当前去重结果，重复通知也能安全重算。
	EvaluateWinCondition();
}

void AmultiplayerWinArea::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	// 玩家可能已经站在区域中等待最后一把钥匙，所以目标进度变化同样必须触发胜利检查。
	EvaluateWinCondition();
}

void AmultiplayerWinArea::EvaluateWinCondition()
{
	if (!HasAuthority() || CoopGameState == nullptr)
	{
		return;
	}

	// 每次从当前状态组合人数和目标，避免维护第二份容易过期的本地胜利条件缓存。
	const int32 PlayerCount = PlayerOccupancy->GetPlayerCount();
	UE_LOG(
		LogMultiplayer,
		Verbose,
		TEXT("WinArea[%s] Players=%d Required=%d ObjectiveComplete=%s"),
		*GetName(),
		PlayerCount,
		RequiredPlayers,
		CoopGameState->IsObjectiveComplete() ? TEXT("true") : TEXT("false"));

	if (AmultiplayerGameMode* CoopGameMode =
		GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
	{
		// GameMode 再次校验目标和人数，让事件来源无法直接写入胜利状态。
		CoopGameMode->TryCompleteCoopGame(PlayerCount, RequiredPlayers);
	}
}
