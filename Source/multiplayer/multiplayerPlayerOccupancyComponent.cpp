// Copyright Epic Games, Inc. All Rights Reserved.

#include "multiplayerPlayerOccupancyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"

UmultiplayerPlayerOccupancyComponent::UmultiplayerPlayerOccupancyComponent()
{
	// 完全由 Overlap/OnDestroyed 事件驱动，不需要 Tick，也不需要把服务器临时成员表复制给客户端。
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerPlayerOccupancyComponent::BindTrigger(
	UPrimitiveComponent* InTrigger,
	bool bInRequirePlayerControlledCharacter)
{
	// 允许组件被重新配置；先完整解绑旧对象，防止一个组件同时监听两个触发体。
	UnbindTrigger();
	BoundTrigger = InTrigger;
	bRequirePlayerControlledCharacter = bInRequirePlayerControlledCharacter;

	AActor* Owner = GetOwner();
	if (BoundTrigger == nullptr || Owner == nullptr)
	{
		return;
	}

	if (!Owner->HasAuthority())
	{
		// 客户端不参与规则统计，关闭碰撞可减少重复 Overlap 和本地误判。
		BoundTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return;
	}

	BoundTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
	BoundTrigger->OnComponentEndOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);
}

void UmultiplayerPlayerOccupancyComponent::UnbindTrigger()
{
	// 先解除触发体回调，再清角色回调；避免清理过程中又收到新的 Begin/EndOverlap。
	if (BoundTrigger != nullptr)
	{
		BoundTrigger->OnComponentBeginOverlap.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
		BoundTrigger->OnComponentEndOverlap.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);
	}

	BoundTrigger = nullptr;
	ClearOccupants();
}

int32 UmultiplayerPlayerOccupancyComponent::GetPlayerCount() const
{
	// 不直接返回 Map.Num()：弱引用失效与延迟销毁窗口中，容器可能暂时保留无效条目。
	int32 PlayerCount = 0;
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid() && Entry.Value > 0)
		{
			++PlayerCount;
		}
	}
	return PlayerCount;
}

void UmultiplayerPlayerOccupancyComponent::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	// 采用追加语义，不擅自清空调用者已有内容；当前门机关传入的是新建临时数组。
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid() && Entry.Value > 0)
		{
			OutCharacters.Add(Entry.Key.Get());
		}
	}
}

void UmultiplayerPlayerOccupancyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 可能由关卡切换或 Owner 销毁触发，复用 UnbindTrigger 保证清理路径只有一份。
	UnbindTrigger();
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex,
	bool bFromSweep,
	const FHitResult& SweepResult)
{
	// 原始 Delegate 参数很多，组件只需要 OtherActor；过滤和去重集中在 AddOccupant。
	AddOccupant(OtherActor);
}

void UmultiplayerPlayerOccupancyComponent::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	// 重复或无对应 Begin 的 EndOverlap 会由 RemoveOccupant 的查表保护安全忽略。
	RemoveOccupant(OtherActor);
}

void UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed(
	AActor* DestroyedActor)
{
	AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return;
	}

	ACharacter* Character = Cast<ACharacter>(DestroyedActor);
	if (Character == nullptr)
	{
		return;
	}

	// Pawn 销毁可能没有 EndOverlap，直接移除整个角色条目并按人数变化广播一次。
	const int32 PreviousPlayerCount = GetPlayerCount();
	OverlapCounts.Remove(Character);
	Character->OnDestroyed.RemoveDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

ACharacter* UmultiplayerPlayerOccupancyComponent::GetValidOccupant(
	AActor* OtherActor) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return nullptr;
	}

	// IsPlayerControlled 在服务器上判断实际玩家 Pawn；配置允许时也可接受 AI Character。
	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr
		|| (bRequirePlayerControlledCharacter && !Character->IsPlayerControlled()))
	{
		return nullptr;
	}

	return Character;
}

void UmultiplayerPlayerOccupancyComponent::AddOccupant(AActor* OtherActor)
{
	ACharacter* Character = GetValidOccupant(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	int32& OverlapCount = OverlapCounts.FindOrAdd(Character);
	// (**) 这里统计的是该角色进入区域的碰撞组件数，不是进入事件次数对应的玩家数。
	++OverlapCount;
	if (OverlapCount == 1)
	{
		Character->OnDestroyed.AddUniqueDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

void UmultiplayerPlayerOccupancyComponent::RemoveOccupant(AActor* OtherActor)
{
	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	int32* OverlapCount = OverlapCounts.Find(Character);
	if (OverlapCount == nullptr)
	{
		// 碰撞状态重建时可能收到没有配对 Begin 的 End，直接忽略比创建负计数更安全。
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	--(*OverlapCount);
	if (*OverlapCount <= 0)
	{
		// 只有最后一个重叠组件离开，角色才真正离开区域。
		OverlapCounts.Remove(Character);
		Character->OnDestroyed.RemoveDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

void UmultiplayerPlayerOccupancyComponent::BroadcastIfPlayerCountChanged(
	int32 PreviousPlayerCount)
{
	const int32 NewPlayerCount = GetPlayerCount();
	if (NewPlayerCount != PreviousPlayerCount)
	{
		// 组件重叠数变化但玩家数未变化时不广播，避免机关被同一角色的多个碰撞体反复触发。
		OnOccupancyChanged.Broadcast(NewPlayerCount);
	}
}

void UmultiplayerPlayerOccupancyComponent::ClearOccupants()
{
	// Map 使用弱引用不会阻止销毁，但动态 Delegate 仍保存在角色对象中，必须对有效对象逐一解绑。
	for (const TPair<TWeakObjectPtr<ACharacter>, int32>& Entry : OverlapCounts)
	{
		if (Entry.Key.IsValid())
		{
			Entry.Key->OnDestroyed.RemoveDynamic(
				this,
				&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
		}
	}
	OverlapCounts.Reset();
}
