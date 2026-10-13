// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerWinArea.h"

#include "Components/BoxComponent.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"

//构造函数：初始化碰撞与组件
AmultiplayerWinArea::AmultiplayerWinArea()
{

	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false;
	WinTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("WinTrigger"));
	SetRootComponent(WinTrigger);
	WinTrigger->SetBoxExtent(FVector(150.0f));
	//仅查询
	WinTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	//只对角色开启碰撞通道检测
	WinTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	WinTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	//只关注 Pawn，具体是否玩家控制以及多碰撞体去重由 PlayerOccupancy 统一处理。
	PlayerOccupancy =CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(TEXT("PlayerOccupancy"));
}


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

	//获得配置
	RequiredPlayers = FmultiplayerGameplayConfig::Get(this).WinRequiredPlayers;
	//绑定人数变化事件
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this,
		&AmultiplayerWinArea::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(WinTrigger);
	//绑定进度变化事件
	CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
			this,
			&AmultiplayerWinArea::HandleObjectiveProgressChanged);
	}
	//初始化之后立即评估是否胜利
	EvaluateWinCondition();
}

//销毁前解绑事件
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

//人数变化，执行判定
void AmultiplayerWinArea::HandleOccupancyChanged(int32 PlayerCount)
{
	// 不缓存事件参数，统一读取 PlayerOccupancy 的当前去重结果，重复通知也能安全重算。
	EvaluateWinCondition();
}

//进度变化执行判定
void AmultiplayerWinArea::HandleObjectiveProgressChanged(
	int32 ActivatedKeys,
	int32 RequiredKeys)
{
	// 玩家可能已经站在区域中等待最后一把钥匙，所以目标进度变化同样必须触发胜利检查。
	EvaluateWinCondition();
}

//判定内容
void AmultiplayerWinArea::EvaluateWinCondition()
{
	//客户端直接返回，gamemode无效返回
	if (GetNetMode() == NM_Client || CoopGameState == nullptr)
	{
		return;
	}

	// 每次从当前状态组合人数和目标，避免维护第二份容易过期的本地胜利条件缓存。
	const int32 PlayerCount = PlayerOccupancy->GetPlayerCount();

	if (AmultiplayerGameMode* CoopGameMode =GetWorld()->GetAuthGameMode<AmultiplayerGameMode>())
	{
		//提交数据不做判定
		CoopGameMode->TryCompleteCoopGame(PlayerCount, RequiredPlayers);
	}
}
