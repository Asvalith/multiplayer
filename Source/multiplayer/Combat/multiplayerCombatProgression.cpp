// Copyright Epic Games, Inc. All Rights Reserved.
// 成长提交遵循“验证快照 → 写状态 → 更新能力与 GE → 通知”的单一路径。
// 临时战斗状态不在本模块保存；复制、短时重连和长期存档复用相同成长校验。

#include "Combat/multiplayerCombatProgression.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "GameplayEffectAggregator.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "NativeGameplayTags.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// 演示只改变本进程的初始配装；不是玩家档案，不能读写正式成长存档。
	bool IsSkillDemoSession()
	{
#if !UE_BUILD_SHIPPING
		return FParse::Param(FCommandLine::Get(), TEXT("CoopSkillDemo"));
#else
		return false;
#endif
	}
}

UE_DEFINE_GAMEPLAY_TAG_STATIC(ProgressionHealth, "Data.Progression.MaxHealth");
UE_DEFINE_GAMEPLAY_TAG_STATIC(ProgressionAttack, "Data.Progression.AttackPower");

// 同一个定义承载两项加法，数值来自服务器 Spec；不叠到 Base，也不与另一来源的句柄合并。
UmultiplayerCombatProgressionEffect::UmultiplayerCombatProgressionEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	FGameplayModifierInfo& Health = Modifiers.AddDefaulted_GetRef();
	Health.Attribute = UmultiplayerCombatAttributes::GetMaxHealthAttribute();
	Health.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat HealthValue; HealthValue.DataTag = ProgressionHealth;
	Health.ModifierMagnitude = FGameplayEffectModifierMagnitude(HealthValue);
	FGameplayModifierInfo& Attack = Modifiers.AddDefaulted_GetRef();
	Attack.Attribute = UmultiplayerCombatAttributes::GetAttackPowerAttribute();
	Attack.ModifierOp = EGameplayModOp::Additive;
	FSetByCallerFloat AttackValue; AttackValue.DataTag = ProgressionAttack;
	Attack.ModifierMagnitude = FGameplayEffectModifierMagnitude(AttackValue);
}

// 组件不逐帧工作；只有提交状态和所属玩家收到复制时才通知界面。
UmultiplayerCombatProgressionComponent::UmultiplayerCombatProgressionComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	GrantedAbilities.SetNum(5);
}

// 只向拥有者复制配装和奖励记录；其他玩家通过现有属性/动作复制观察结果。
void UmultiplayerCombatProgressionComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UmultiplayerCombatProgressionComponent, State, COND_OwnerOnly, REPNOTIFY_Always);
}

// 客户端从已到达的快照重绘，不自行发奖或重新授予能力。
void UmultiplayerCombatProgressionComponent::OnRep_State() { OnProgressionChanged.Broadcast(); }

// 角色拥有组件和 ASC；不接收客户端任意 ASC 或能力类引用。
AmultiplayerCombatCharacter* UmultiplayerCombatProgressionComponent::Character() const
{
	return Cast<AmultiplayerCombatCharacter>(GetOwner());
}

// 本地请求依赖正常 owning connection 路由；服务器仍在每个 RPC 内做业务检查。
bool UmultiplayerCombatProgressionComponent::CanRequest() const
{
	const auto* Pawn = Character();
	return Pawn && (Pawn->HasAuthority() || Pawn->IsLocallyControlled());
}

// 换装不能取消或替换正在执行的技能；恢复快照允许未接纳/死亡的新 Pawn。
bool UmultiplayerCombatProgressionComponent::CanChangeLoadout(bool bRequireReady) const
{
	const auto* Pawn = Character();
	if (!Pawn || !Pawn->HasAuthority() || bUpdatingLoadout || !bInitialized
		|| (bRequireReady && !Pawn->IsCombatReady())) return false;
	for (const FGameplayAbilitySpec& Spec : Pawn->GetAbilitySystemComponent()->GetActivatableAbilities())
		if (Spec.IsActive()) return false;
	return true;
}

// 初次仅签发服务器档案标识；重复初始化不会重置点数、重复技能或重叠永久效果。
void UmultiplayerCombatProgressionComponent::InitializeLoadout()
{
	const auto* Pawn = Character();
	if (!Pawn || !Pawn->HasAuthority() || bInitialized || bUpdatingLoadout) return;
	bInitialized = true;
	FmultiplayerCombatProgressionState Initial = State;
	if (!Initial.ProfileId.IsValid()) Initial.ProfileId = FGuid::NewGuid();
	// 仅当前手动演示局消耗初始技能点解锁冲刺，不写存档或修改正式初始配装。
	if (IsSkillDemoSession() && Initial.ActiveLevels[int32(EMultiplayerCombatSkill::Dash)] == 0)
	{
		Initial.ActiveLevels[int32(EMultiplayerCombatSkill::Dash)] = 1;
		--Initial.SkillPoints;
	}
	CommitState(Initial);
}

// 保存/恢复都检查尺寸、等级、点数收支和槽位唯一性，不默默修补坏档造成隐性多发奖励。
bool UmultiplayerCombatProgressionComponent::IsStateValid(const FmultiplayerCombatProgressionState& Candidate)
{
	if (Candidate.Version != 1 || !Candidate.ProfileId.IsValid() || Candidate.Level < 1 || Candidate.Level > 100
		|| Candidate.Experience < 0 || (Candidate.Level == 100 ? Candidate.Experience != 0 : Candidate.Experience >= 100 * Candidate.Level)
		|| Candidate.ActiveLevels.Num() != 5 || Candidate.Slots.Num() != 5 || Candidate.PassiveLevels.Num() != 2
		|| Candidate.ClaimedRewards.Num() > 64) return false;
	int32 Spent = 0;
	for (int32 Index = 0; Index < Candidate.ActiveLevels.Num(); ++Index)
	{
		const int32 SkillLevel = Candidate.ActiveLevels[Index];
		if (SkillLevel < (Index == 4 ? 0 : 1) || SkillLevel > 3 || SkillLevel > Candidate.Level) return false;
		Spent += Index == 4 ? SkillLevel : SkillLevel - 1;
	}
	for (int32 PassiveLevel : Candidate.PassiveLevels)
	{
		if (PassiveLevel < 0 || PassiveLevel > 3 || PassiveLevel > Candidate.Level) return false;
		Spent += PassiveLevel;
	}
	if (Candidate.SkillPoints != 2 + Candidate.Level - 1 - Spent || Candidate.SkillPoints < 0) return false;
	TSet<EMultiplayerCombatSkill> SeenSkills;
	for (EMultiplayerCombatSkill Skill : Candidate.Slots)
	{
		if (int32(Skill) >= 5 || SeenSkills.Contains(Skill)) return false;
		SeenSkills.Add(Skill);
	}
	const int32 PassiveIndex = int32(Candidate.EquippedPassive) - 1;
	if (Candidate.EquippedPassive != EMultiplayerCombatPassive::None
		&& (!Candidate.PassiveLevels.IsValidIndex(PassiveIndex) || Candidate.PassiveLevels[PassiveIndex] <= 0)) return false;
	TSet<FGuid> Rewards;
	for (const FGuid& Id : Candidate.ClaimedRewards)
	{
		if (!Id.IsValid() || Rewards.Contains(Id)) return false;
		Rewards.Add(Id);
	}
	return true;
}

// 一次提交先写完整快照，再更新 GAS；同步属性回调不能在半套装配期间嵌套修改。
bool UmultiplayerCombatProgressionComponent::CommitState(const FmultiplayerCombatProgressionState& Candidate)
{
	if (!Character() || !Character()->HasAuthority() || bUpdatingLoadout || !IsStateValid(Candidate)) return false;
	{
		TGuardValue<bool> Updating(bUpdatingLoadout, true);
		State = Candidate;
		RefreshAbilities();
		RefreshBonuses();
	}
	GetOwner()->ForceNetUpdate();
	OnRep_State();
	return true;
}

// 服务器短时重连与长期加载共用同一校验入口，不复用旧 Pawn 的活动技能实例。
bool UmultiplayerCombatProgressionComponent::RestoreState(const FmultiplayerCombatProgressionState& Snapshot)
{
	return CanChangeLoadout(false) && CommitState(Snapshot);
}

// 只更新本组件授予的 Spec.Level，学习后 GAS 的实际能力等级与快照一致。
void UmultiplayerCombatProgressionComponent::RefreshAbilities()
{
	auto* Pawn = Character();
	auto* ASC = Pawn->GetAbilitySystemComponent();
	for (int32 Index = 0; Index < 5; ++Index)
	{
		// 零级尚未学习，不留一份可以绕过快捷栏直接激活的 GAS Spec。
		if (State.ActiveLevels[Index] == 0)
		{
			if (GrantedAbilities[Index].IsValid()) ASC->ClearAbility(GrantedAbilities[Index]);
			GrantedAbilities[Index] = FGameplayAbilitySpecHandle();
			continue;
		}
		const auto Class = Pawn->GetSkillAbilityClass(EMultiplayerCombatSkill(Index));
		if (!Class) continue;
		if (auto* Spec = ASC->FindAbilitySpecFromHandle(GrantedAbilities[Index]))
		{
			if (Spec->Level != State.ActiveLevels[Index])
			{
				Spec->Level = State.ActiveLevels[Index]; ASC->MarkAbilitySpecDirty(*Spec);
			}
		}
		else GrantedAbilities[Index] = ASC->GiveAbility(FGameplayAbilitySpec(Class, State.ActiveLevels[Index], INDEX_NONE, this));
	}
}

// 永久加成仍由真实 GAS 效果提供，句柄移除即可撤销，不把基础属性越叠越高。
FActiveGameplayEffectHandle UmultiplayerCombatProgressionComponent::ApplyBonus(float Health, float Attack)
{
	if (Health == 0.f && Attack == 0.f) return {};
	auto* ASC = Character()->GetAbilitySystemComponent();
	auto Spec = ASC->MakeOutgoingSpec(UmultiplayerCombatProgressionEffect::StaticClass(), 1.f, ASC->MakeEffectContext());
	Spec.Data->SetSetByCallerMagnitude(ProgressionHealth, Health);
	Spec.Data->SetSetByCallerMagnitude(ProgressionAttack, Attack);
	return ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}

// 成长与当前被动各占一个句柄；换装不补血，上限降低时只把越界生命压回新上限。
void UmultiplayerCombatProgressionComponent::RefreshBonuses()
{
	auto* ASC = Character()->GetAbilitySystemComponent();
	// 同一装配提交先换完两份 GE，再通知最终聚合值；不能按移除旧 GE 的中间上限扣血。
	// 最终资源上限统一由 AttributeSet 钳制，成长层不再重复写生命。
	// 仅合并 GAS 聚合通知，不提供退款事务；换装既不免费回血，也不保留超出最终上限的资源。
	FScopedAggregatorOnDirtyBatch ReplacingBonuses;
	if (AppliedLevel != State.Level)
	{
		ASC->RemoveActiveGameplayEffect(GrowthHandle); GrowthHandle.Invalidate();
		AppliedLevel = State.Level;
		GrowthHandle = ApplyBonus((State.Level - 1) * 5.f, (State.Level - 1) * 2.f);
	}
	const int32 PassiveLevel = GetPassiveLevel(State.EquippedPassive);
	if (AppliedPassive != State.EquippedPassive || AppliedPassiveLevel != PassiveLevel)
	{
		ASC->RemoveActiveGameplayEffect(PassiveHandle); PassiveHandle.Invalidate();
		AppliedPassive = State.EquippedPassive; AppliedPassiveLevel = PassiveLevel;
		PassiveHandle = ApplyBonus(AppliedPassive == EMultiplayerCombatPassive::Fortitude ? PassiveLevel * 20.f : 0.f,
			AppliedPassive == EMultiplayerCombatPassive::Power ? PassiveLevel * 5.f : 0.f);
	}
}

// 无效槽位不用于激活，查询返回基础攻击仅为蓝图显示提供稳定默认值。
EMultiplayerCombatSkill UmultiplayerCombatProgressionComponent::GetSlotSkill(int32 Slot) const
{
	return State.Slots.IsValidIndex(Slot) ? State.Slots[Slot] : EMultiplayerCombatSkill::Attack;
}

// 等级来自所属玩家快照；客户端不自行修改能力等级。
int32 UmultiplayerCombatProgressionComponent::GetSkillLevel(EMultiplayerCombatSkill Skill) const
{
	return State.ActiveLevels.IsValidIndex(int32(Skill)) ? State.ActiveLevels[int32(Skill)] : 0;
}

// None 表示不装配被动，不占用第三个被动等级条目。
int32 UmultiplayerCombatProgressionComponent::GetPassiveLevel(EMultiplayerCombatPassive Passive) const
{
	const int32 Index = int32(Passive) - 1;
	return State.PassiveLevels.IsValidIndex(Index) ? State.PassiveLevels[Index] : 0;
}

// 定向技能仍从角色的瞄准事件入口提交；返回值只表示本地请求启动，不代替服务器确认。
bool UmultiplayerCombatProgressionComponent::ActivateSlot(int32 Slot)
{
	if (!CanRequest() || !State.Slots.IsValidIndex(Slot)) return false;
	// 包括未学习与生命限制在内的拒绝统一经过角色入口，不能让按键无声失效。
	return Character()->ActivateSkill(State.Slots[Slot]);
}

// RPC 包装只开放操作，不开放任意成长快照或任意属性写入。
void UmultiplayerCombatProgressionComponent::RequestLearnSkill(EMultiplayerCombatSkill Skill)
{
	if (CanRequest()) ServerLearnSkill(Skill);
}
// 客户端只提出槽位交换，实际映射由服务器提交后复制回来。
void UmultiplayerCombatProgressionComponent::RequestEquipSkill(int32 Slot, EMultiplayerCombatSkill Skill)
{
	if (CanRequest()) ServerEquipSkill(Slot, Skill);
}
// 学习请求只携带被动枚举，不允许客户端提交等级或点数。
void UmultiplayerCombatProgressionComponent::RequestLearnPassive(EMultiplayerCombatPassive Passive)
{
	if (CanRequest()) ServerLearnPassive(Passive);
}
// 装配请求与学习分开，服务器负责撤销旧被动和应用新效果。
void UmultiplayerCombatProgressionComponent::RequestEquipPassive(EMultiplayerCombatPassive Passive)
{
	if (CanRequest()) ServerEquipPassive(Passive);
}

// 每级消耗一点，角色等级必须达到目标技能等级；活动技能执行中不调整 Spec。
void UmultiplayerCombatProgressionComponent::ServerLearnSkill_Implementation(EMultiplayerCombatSkill Skill)
{
	const int32 Level = GetSkillLevel(Skill);
	if (!CanChangeLoadout(true) || !State.ActiveLevels.IsValidIndex(int32(Skill)) || Level >= 3
		|| State.SkillPoints <= 0 || State.Level < Level + 1) return;
	auto Next = State; ++Next.ActiveLevels[int32(Skill)]; --Next.SkillPoints; CommitState(Next);
}

// 五个已解锁技能一一对应五个槽；装备已有技能时交换位置，而不是复制同一技能占多个槽。
void UmultiplayerCombatProgressionComponent::ServerEquipSkill_Implementation(int32 Slot, EMultiplayerCombatSkill Skill)
{
	if (!CanChangeLoadout(true) || !State.Slots.IsValidIndex(Slot) || GetSkillLevel(Skill) <= 0) return;
	const int32 Previous = State.Slots.IndexOfByKey(Skill);
	if (Previous == INDEX_NONE || Previous == Slot) return;
	auto Next = State; Next.Slots.Swap(Previous, Slot); CommitState(Next);
}

// 被动等级与主动一样消耗技能点，最多三级，但学习本身不自动覆盖当前装配。
void UmultiplayerCombatProgressionComponent::ServerLearnPassive_Implementation(EMultiplayerCombatPassive Passive)
{
	const int32 Index = int32(Passive) - 1; const int32 Level = GetPassiveLevel(Passive);
	if (!CanChangeLoadout(true) || !State.PassiveLevels.IsValidIndex(Index) || Level >= 3
		|| State.SkillPoints <= 0 || State.Level < Level + 1) return;
	auto Next = State; ++Next.PassiveLevels[Index]; --Next.SkillPoints; CommitState(Next);
}

// 同时只装配一个被动；None 是主动卸下，不会移除成长或其他来源的效果。
void UmultiplayerCombatProgressionComponent::ServerEquipPassive_Implementation(EMultiplayerCombatPassive Passive)
{
	if (!CanChangeLoadout(true) || State.EquippedPassive == Passive
		|| (Passive != EMultiplayerCombatPassive::None && GetPassiveLevel(Passive) <= 0)) return;
	auto Next = State; Next.EquippedPassive = Passive; CommitState(Next);
}

// 发奖允许在击杀技能的收招期间发生，但不重授予/取消能力；去重标识先提交再通知。
bool UmultiplayerCombatProgressionComponent::AwardExperience(int32 Amount, FGuid RewardId)
{
	const auto* Pawn = Character();
	if (!Pawn || !Pawn->HasAuthority() || !bInitialized || bUpdatingLoadout || Amount <= 0 || !RewardId.IsValid()
		|| State.ClaimedRewards.Contains(RewardId)) return false;
	auto Next = State; Next.ClaimedRewards.Add(RewardId);
	if (Next.ClaimedRewards.Num() > 64) Next.ClaimedRewards.RemoveAt(0);
	int64 Experience = int64(Next.Experience) + Amount;
	while (Next.Level < 100 && Experience >= 100 * Next.Level)
	{
		Experience -= 100 * Next.Level; ++Next.Level; ++Next.SkillPoints;
	}
	Next.Experience = Next.Level == 100 ? 0 : int32(Experience);
	return CommitState(Next);
}

// 单机使用固定本地槽；联机按服务器签发的 ProfileId 分档，没有账号系统就不自动跨服务器识别玩家。
FString UmultiplayerCombatProgressionComponent::SaveSlot() const
{
	return GetNetMode() == NM_Standalone ? TEXT("CombatProgress_Local")
		: FString::Printf(TEXT("CombatProgress_%s"), *State.ProfileId.ToString(EGuidFormats::Digits));
}

// 只写版本化成长数据，既不抬血量也不保留已消费资源；路径不来自 RPC 参数。
bool UmultiplayerCombatProgressionComponent::SaveProgress()
{
	// 演示和正式单机原先共用 Local 槽；在创建 SaveGame 前拒绝，保证演示不污染玩家档案。
	if (IsSkillDemoSession() || !Character() || !Character()->HasAuthority() || !bInitialized || bUpdatingLoadout || !IsStateValid(State)) return false;
	auto* Save = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::CreateSaveGameObject(UmultiplayerCombatProgressionSaveGame::StaticClass()));
	Save->Progression = State; return UGameplayStatics::SaveGameToSlot(Save, SaveSlot(), 0);
}

// 加载失败保持当前状态；联机只允许读取当前服务器档案，单机可还原固定本地槽里的稳定标识。
bool UmultiplayerCombatProgressionComponent::LoadProgress()
{
	// 与保存共用同一演示边界，不把普通存档加载进带额外初始解锁的演示局。
	if (IsSkillDemoSession() || !CanChangeLoadout(false) || !UGameplayStatics::DoesSaveGameExist(SaveSlot(), 0)) return false;
	const auto* Save = Cast<UmultiplayerCombatProgressionSaveGame>(UGameplayStatics::LoadGameFromSlot(SaveSlot(), 0));
	if (!Save || (GetNetMode() != NM_Standalone && Save->Progression.ProfileId != State.ProfileId)) return false;
	return RestoreState(Save->Progression);
}

// 仅释放本组件持有的永久效果与授予记录，退出时不按标签误删其他业务的技能或状态。
void UmultiplayerCombatProgressionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (auto* Pawn = Character(); Pawn && Pawn->HasAuthority())
	{
		auto* ASC = Pawn->GetAbilitySystemComponent();
		ASC->RemoveActiveGameplayEffect(GrowthHandle); ASC->RemoveActiveGameplayEffect(PassiveHandle);
		for (FGameplayAbilitySpecHandle Handle : GrantedAbilities) if (Handle.IsValid()) ASC->ClearAbility(Handle);
	}
	Super::EndPlay(EndPlayReason);
}
