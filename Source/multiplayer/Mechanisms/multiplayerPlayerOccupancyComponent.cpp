// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapInfo.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"

/*
 * 事件流：触发体报告进入/离开 -> 按 Character 累计重叠项 -> 对外发布不同角色数量。
 * 控制器变更更新玩家资格，OnDestroyed 补足销毁清理；重绑定从已有重叠表恢复区域候选。
 * 调用方应确保组件用于服务器规则；非复制的本地 Actor 还须像 WinArea 一样先排除客户端世界。
 */

/** 关闭 Tick 和组件复制，人数表作为服务器按事件维护的临时规则数据。 */
UmultiplayerPlayerOccupancyComponent::UmultiplayerPlayerOccupancyComponent()
{
	// 重叠、控制器变化和销毁均由事件驱动，不需要 Tick，也不复制服务器临时成员表。
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

/**
 * 替换绑定触发体，并从新触发体已有重叠项恢复计数；客户端复制 Actor 只关闭本地检测。
 * (**) 重绑定不先通知清零再通知恢复，否则机关会短暂误判无人；比较最终集合后最多通知一次。
 */
void UmultiplayerPlayerOccupancyComponent::BindTrigger(
	UPrimitiveComponent* InTrigger,
	bool bInRequirePlayerControlledCharacter)
{
	// 保存旧成员、静默清理，最后只广播最终结果；这是事件层面的合并，并非线程同步原子操作。
	TSet<TWeakObjectPtr<ACharacter>> PreviousOccupants;
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.OverlapCount > 0 && Entry.Value.bCountsAsPlayer)
		{
			PreviousOccupants.Add(Entry.Key);
		}
	}

	const auto BroadcastRebindIfChanged =
		[this, &PreviousOccupants]()
		{
			const int32 NewPlayerCount = GetPlayerCount();
			bool bMembershipChanged = NewPlayerCount != PreviousOccupants.Num();
			if (!bMembershipChanged)
			{
				for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
				{
					if (Entry.Key.IsValid()
						&& Entry.Value.OverlapCount > 0 && Entry.Value.bCountsAsPlayer
						&& !PreviousOccupants.Contains(Entry.Key))
					{
						bMembershipChanged = true;
						break;
					}
				}
			}

			if (bMembershipChanged)
			{
				// 人数相同但成员从 A 换成 B 时，依赖玩家身份集合的机关仍必须重新求值。
				OnOccupancyChanged.Broadcast(NewPlayerCount);
			}
		};

	UnbindTriggerInternal();
	BoundTrigger = InTrigger;
	bRequirePlayerControlledCharacter = bInRequirePlayerControlledCharacter;

	AActor* Owner = GetOwner();
	if (BoundTrigger == nullptr || Owner == nullptr)
	{
		BroadcastRebindIfChanged();
		return;
	}

	if (!Owner->HasAuthority())
	{
		// 客户端不参与规则统计，关闭碰撞可减少重复 Overlap 和本地误判。
		BoundTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BroadcastRebindIfChanged();
		return;
	}

	BoundTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
	BoundTrigger->OnComponentEndOverlap.AddUniqueDynamic(
		this,
		&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);

	// Delegate 绑定前角色可能已经在区域内；按真实重叠组件重建，不能只按 Actor 记一次。
	RebuildOccupantsFromCurrentOverlaps();
	BroadcastRebindIfChanged();
}

/** 对外解除绑定并清空区域成员；若原先有人，最后统一广播人数归零。 */
void UmultiplayerPlayerOccupancyComponent::UnbindTrigger()
{
	const int32 PreviousPlayerCount = GetPlayerCount();
	UnbindTriggerInternal();
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

/** 静默清除触发体和角色回调，不自行广播，供普通解绑与重绑定复用。 */
void UmultiplayerPlayerOccupancyComponent::UnbindTriggerInternal()
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

/** 只统计有效且仍有重叠项的角色，不修改内部记录，也不把多组件重叠重复算成人数。 */
int32 UmultiplayerPlayerOccupancyComponent::GetPlayerCount() const
{
	// 不直接返回 Map.Num()：弱引用失效与延迟销毁窗口中，容器可能暂时保留无效条目。
	int32 PlayerCount = 0;
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.OverlapCount > 0 && Entry.Value.bCountsAsPlayer)
		{
			++PlayerCount;
		}
	}
	return PlayerCount;
}

/** 追加当前有效角色到调用方数组；调用方可再用 TSet 合并多个区域中的同一角色。 */
void UmultiplayerPlayerOccupancyComponent::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	// 采用追加语义，不擅自清空调用者已有内容；当前门机关传入的是新建临时数组。
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.OverlapCount > 0 && Entry.Value.bCountsAsPlayer)
		{
			OutCharacters.Add(Entry.Key.Get());
		}
	}
}

/** Owner 或关卡结束时复用完整解绑路径，避免弱引用表和事件关系留到后续运行阶段。 */
void UmultiplayerPlayerOccupancyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	// EndPlay 可能由关卡切换或 Owner 销毁触发，复用 UnbindTrigger 保证清理路径只有一份。
	UnbindTrigger();
	Super::EndPlay(EndPlayReason);
}

/** 接收引擎的一条开始重叠事件，按角色累计重叠项，具体过滤由 AddOccupant 统一处理。 */
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

/** 接收一条结束重叠事件；只有该角色最后一个重叠项离开才减少人数。 */
void UmultiplayerPlayerOccupancyComponent::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	// 表中不存在该角色时会忽略 End；已有条目按事件减少一次，依赖引擎的重叠开始/结束配对。
	RemoveOccupant(OtherActor);
}

/** 角色直接销毁时清除整条记录，无需等待它的每个碰撞体分别发出离开事件。 */
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

	FOccupantRecord RemovedRecord;
	if (Occupants.RemoveAndCopyValue(Character, RemovedRecord))
	{
		UnbindOccupant(Character);
		// 销毁中的弱引用可能已失效，不能仅用“调用前 GetPlayerCount”推测是否需要通知。
		if (RemovedRecord.bCountsAsPlayer)
		{
			OnOccupancyChanged.Broadcast(GetPlayerCount());
		}
	}
}

/** 保留区域内所有 Character 候选；玩家资格由独立字段和控制器事件维护。 */
ACharacter* UmultiplayerPlayerOccupancyComponent::GetOverlapCandidate(
	AActor* OtherActor) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr || !Owner->HasAuthority())
	{
		return nullptr;
	}

	ACharacter* Character = Cast<ACharacter>(OtherActor);
	return IsValid(Character) && !Character->IsActorBeingDestroyed() ? Character : nullptr;
}

/** 物理重叠不等于玩家占用；资格会随 Possess/UnPossess 改变，而不一定产生新的 Overlap。 */
bool UmultiplayerPlayerOccupancyComponent::CanCountAsPlayer(const ACharacter* Character) const
{
	return IsValid(Character) && !Character->IsActorBeingDestroyed()
		&& (!bRequirePlayerControlledCharacter || Character->IsPlayerControlled());
}

/** 用记录中的旧资格比较变化；直接查询当前 Controller 会丢失变更前的人数。 */
void UmultiplayerPlayerOccupancyComponent::HandleControllerChanged(
	APawn* Pawn, AController* OldController, AController* NewController)
{
	ACharacter* Character = Cast<ACharacter>(Pawn);
	FOccupantRecord* Record = Occupants.Find(Character);
	if (Record == nullptr)
	{
		return;
	}
	const int32 PreviousPlayerCount = GetPlayerCount();
	Record->bCountsAsPlayer = CanCountAsPlayer(Character);
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

/** 一个候选角色的外部订阅成组释放，与首次重叠的绑定严格配对。 */
void UmultiplayerPlayerOccupancyComponent::UnbindOccupant(ACharacter* Character)
{
	Character->OnDestroyed.RemoveDynamic(this, &UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
	Character->ReceiveControllerChangedDelegate.RemoveDynamic(
		this, &UmultiplayerPlayerOccupancyComponent::HandleControllerChanged);
}

/**
 * 为候选角色增加重叠项并监听生命周期；只有有效玩家人数改变才广播。
 * (*) 这里去重的是玩家计数，不会按 OtherComponent 对重复的同一 Begin 事件再次去重。
 */
void UmultiplayerPlayerOccupancyComponent::AddOccupant(AActor* OtherActor)
{
	ACharacter* Character = GetOverlapCandidate(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	RecordOccupantOverlap(Character);
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

/** 首次重叠绑定销毁和控制器回调；批量重建由调用方最后统一通知，不发布中间人数。 */
void UmultiplayerPlayerOccupancyComponent::RecordOccupantOverlap(ACharacter* Character)
{
	FOccupantRecord& Record = Occupants.FindOrAdd(Character);
	// 一个 Begin 贡献一个重叠项；角色可能有多个组件或刚体，但对外人数始终只算一人。
	++Record.OverlapCount;
	if (Record.OverlapCount == 1)
	{
		Record.bCountsAsPlayer = CanCountAsPlayer(Character);
		Character->OnDestroyed.AddUniqueDynamic(
			this,
			&UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed);
		Character->ReceiveControllerChangedDelegate.AddUniqueDynamic(
			this, &UmultiplayerPlayerOccupancyComponent::HandleControllerChanged);
	}
}

/** 减少已有重叠记录；移除最后一项时解除全部角色监听，未知角色直接忽略。 */
void UmultiplayerPlayerOccupancyComponent::RemoveOccupant(AActor* OtherActor)
{
	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	FOccupantRecord* Record = Occupants.Find(Character);
	if (Record == nullptr)
	{
		// 碰撞状态重建时可能收到没有配对 Begin 的 End，直接忽略比创建负计数更安全。
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	--Record->OverlapCount;
	if (Record->OverlapCount <= 0)
	{
		// 只有最后一个重叠组件离开，角色才真正离开区域。
		Occupants.Remove(Character);
		UnbindOccupant(Character);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

/** 比较调用前后人数，过滤同一角色组件进出的噪声；重绑定的成员变化由专用路径处理。 */
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

/**
 * 从触发体当前保存的完整重叠表重建计数，并为每个首次出现的角色绑定销毁监听。
 * 调用前须清空旧表；函数只读取现成记录，不强制刷新碰撞，也不逐个广播中间人数。
 */
void UmultiplayerPlayerOccupancyComponent::RebuildOccupantsFromCurrentOverlaps()
{
	if (BoundTrigger == nullptr)
	{
		return;
	}

	// 完整 OverlapInfo 同时保留组件和 BodyIndex，重建结果才能与 Begin/EndOverlap 的计数语义一致。
	// GetOverlappingComponents 会按组件去重，在启用多刚体重叠时可能少算。
	for (const FOverlapInfo& Overlap : BoundTrigger->GetOverlapInfos())
	{
		UPrimitiveComponent* OtherComponent = Overlap.OverlapInfo.Component.Get();
		if (OtherComponent == nullptr)
		{
			continue;
		}

		ACharacter* Character = GetOverlapCandidate(OtherComponent->GetOwner());
		if (Character == nullptr)
		{
			continue;
		}

		RecordOccupantOverlap(Character);
	}
}

/** 先解除有效角色上的销毁监听再清表；弱引用不会保活角色，但也不会自动清掉整张表。 */
void UmultiplayerPlayerOccupancyComponent::ClearOccupants()
{
	// Map 使用弱引用不会阻止销毁，但动态 Delegate 仍保存在角色对象中，必须对有效对象逐一解绑。
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid())
		{
			UnbindOccupant(Entry.Key.Get());
		}
	}
	Occupants.Reset();
}
