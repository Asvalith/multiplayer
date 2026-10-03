// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapInfo.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"

/*
 * 进入/离开维护重叠次数，控制器变化维护玩家资格，销毁直接移除角色。
 * 非复制 Actor 的调用方须像 WinArea 一样排除客户端世界，不能只依赖 HasAuthority。
 */

UmultiplayerPlayerOccupancyComponent::UmultiplayerPlayerOccupancyComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false);
}

void UmultiplayerPlayerOccupancyComponent::BindTrigger(
	UPrimitiveComponent* InTrigger,
	bool bInRequirePlayerControlledCharacter)
{
	// 先保存成员再静默重建，避免中途通知清零让机关短暂误判无人。
	TSet<TWeakObjectPtr<ACharacter>> PreviousOccupants;
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.bCountsAsPlayer)
		{
			PreviousOccupants.Add(Entry.Key);
		}
	}

	UnbindTriggerInternal();
	BoundTrigger = InTrigger;
	bRequirePlayerControlledCharacter = bInRequirePlayerControlledCharacter;

	AActor* Owner = GetOwner();
	if (BoundTrigger != nullptr && Owner != nullptr)
	{
		if (Owner->HasAuthority())
		{
			BoundTrigger->OnComponentBeginOverlap.AddUniqueDynamic(
				this,
				&UmultiplayerPlayerOccupancyComponent::HandleBeginOverlap);
			BoundTrigger->OnComponentEndOverlap.AddUniqueDynamic(
				this,
				&UmultiplayerPlayerOccupancyComponent::HandleEndOverlap);

			// 保留组件及 BodyIndex，保证重建计数与 Begin/EndOverlap 的语义一致。
			for (const FOverlapInfo& Overlap : BoundTrigger->GetOverlapInfos())
			{
				UPrimitiveComponent* OtherComponent = Overlap.OverlapInfo.Component.Get();
				if (OtherComponent == nullptr)
				{
					continue;
				}
				if (ACharacter* Character = GetOverlapCandidate(OtherComponent->GetOwner()))
				{
					RecordOccupantOverlap(Character);
				}
			}
		}
		else
		{
			BoundTrigger->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}

	// 一次遍历同时统计人数和比较成员；A 换成 B 即使人数不变，也必须通知门。
	int32 NewPlayerCount = 0;
	bool bMembershipChanged = false;
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.bCountsAsPlayer)
		{
			++NewPlayerCount;
			bMembershipChanged = bMembershipChanged || !PreviousOccupants.Contains(Entry.Key);
		}
	}

	if (bMembershipChanged || NewPlayerCount != PreviousOccupants.Num())
	{
		OnOccupancyChanged.Broadcast(NewPlayerCount);
	}
}

void UmultiplayerPlayerOccupancyComponent::UnbindTrigger()
{
	const int32 PreviousPlayerCount = GetPlayerCount();
	UnbindTriggerInternal();
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

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
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (ACharacter* Character = Entry.Key.Get())
		{
			UnbindOccupant(Character);
		}
	}
	Occupants.Reset();
}

int32 UmultiplayerPlayerOccupancyComponent::GetPlayerCount() const
{
	// 不直接返回 Map.Num()：弱引用失效与延迟销毁窗口中，容器可能暂时保留无效条目。
	int32 PlayerCount = 0;
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.bCountsAsPlayer)
		{
			++PlayerCount;
		}
	}
	return PlayerCount;
}

void UmultiplayerPlayerOccupancyComponent::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	for (const TPair<TWeakObjectPtr<ACharacter>, FOccupantRecord>& Entry : Occupants)
	{
		if (Entry.Key.IsValid() && Entry.Value.bCountsAsPlayer)
		{
			OutCharacters.Add(Entry.Key.Get());
		}
	}
}

void UmultiplayerPlayerOccupancyComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
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
	ACharacter* Character = GetOverlapCandidate(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	RecordOccupantOverlap(Character);
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

void UmultiplayerPlayerOccupancyComponent::HandleEndOverlap(
	UPrimitiveComponent* OverlappedComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComponent,
	int32 OtherBodyIndex)
{
	ACharacter* Character = Cast<ACharacter>(OtherActor);
	if (Character == nullptr)
	{
		return;
	}

	FOccupantRecord* Record = Occupants.Find(Character);
	if (Record == nullptr)
	{
		return;
	}

	const int32 PreviousPlayerCount = GetPlayerCount();
	if (--Record->OverlapCount <= 0)
	{
		// 只有最后一个重叠项离开，才移除角色并解除监听。
		Occupants.Remove(Character);
		UnbindOccupant(Character);
	}
	BroadcastIfPlayerCountChanged(PreviousPlayerCount);
}

/** 角色直接销毁时清除整条记录，无需等待它的每个碰撞体分别发出离开事件。 */
void UmultiplayerPlayerOccupancyComponent::HandleOccupantDestroyed(
	AActor* DestroyedActor)
{
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

/** 比较调用前后人数，过滤同一角色组件进出的噪声；重绑定的成员变化由专用路径处理。 */
void UmultiplayerPlayerOccupancyComponent::BroadcastIfPlayerCountChanged(
	int32 PreviousPlayerCount)
{
	const int32 NewPlayerCount = GetPlayerCount();
	if (NewPlayerCount != PreviousPlayerCount)
	{
		OnOccupancyChanged.Broadcast(NewPlayerCount);
	}
}
