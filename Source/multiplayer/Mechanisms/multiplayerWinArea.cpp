// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerWinArea.h"

#include "Components/BoxComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Core/multiplayerLog.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"

/*
 * 终点区域只把区域人数与目标变化汇合起来，最终是否胜利仍由服务器 GameMode 判断。
 * 终点 Actor 不复制；参与玩家通过 GameState 收到胜利结果，界面不直接依赖本地重叠事件。
 */

/** 创建只检测 Pawn 的查询区域和人数组件，规则由事件触发，无需 Actor Tick。 */
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

/**
 * 排除客户端世界后，绑定人数和目标变化并补算当前状态，覆盖绑定前已满足条件的情况。
 * (**) 非复制 Actor 的 HasAuthority 不能单独证明当前是服务器，此处显式检查 NetMode。
 */
void AmultiplayerWinArea::BeginPlay()
{
	Super::BeginPlay();

	// WinArea 不复制，客户端加载出的本地关卡 Actor 也可能让 HasAuthority() 返回 true。
	// 用 NetMode 明确排除客户端，并关闭无用触发，避免客户端重复维护一份伪规则状态。
	if (GetNetMode() == NM_Client)
	{
		WinTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	// 人数和目标完成顺序不确定，因此同时监听两种变化并在任一变化后重新读取当前状态。
	RequiredPlayers = FmultiplayerGameplayConfig::Get(this).WinRequiredPlayers;
	UE_LOG(LogMultiplayer, Verbose, TEXT("WinArea %s: RequiredPlayers=%d"), *GetName(), RequiredPlayers);
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(WinTrigger);

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

/** 先停止监听再清区域人数，避免解绑过程中产生的归零事件重新进入本 Actor 的判定。 */
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

/** 人数变化后读取最新区域状态进行胜利检查，不另存一份人数缓存。 */
void AmultiplayerWinArea::HandleOccupancyChanged(int32 PlayerCount)
{
	// 不缓存事件参数，统一读取 PlayerOccupancy 的当前去重结果，重复通知也能安全重算。
	EvaluateWinCondition();
}

/** 目标进度变化也触发检查，支持最后一个目标完成时玩家已经全部处于终点区域。 */
void AmultiplayerWinArea::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	// 玩家可能已经站在区域中等待最后一把钥匙，所以目标进度变化同样必须触发胜利检查。
	EvaluateWinCondition();
}

/**
 * 在服务器取得当前不同玩家数并交给 GameMode；本类负责触发检查，不直接登记胜利。
 * 多个事件可重复走到这里，是否满足人数、目标以及是否已经胜利，由 GameMode 最后复核。
 */
void AmultiplayerWinArea::EvaluateWinCondition()
{
	if (GetNetMode() == NM_Client || CoopGameState == nullptr)
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
