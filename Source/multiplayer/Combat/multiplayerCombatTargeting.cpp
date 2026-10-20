// Copyright Epic Games, Inc. All Rights Reserved.
#include "Combat/multiplayerCombatTargeting.h"

#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatCoop.h"
#include "Combat/multiplayerCombatTags.h"
#include "AbilitySystemComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"

namespace
{
	FCollisionResponseParams SceneResponses()
	{
		FCollisionResponseParams Result(ECR_Ignore);
		Result.CollisionResponse.SetResponse(ECC_WorldStatic, ECR_Block);
		Result.CollisionResponse.SetResponse(ECC_WorldDynamic, ECR_Block);
		return Result;
	}

	// 预览和弹体都穿过友军，不把碰撞盒或尸体当作可攻击对象。
	FCollisionQueryParams ProjectileQuery(const AmultiplayerCombatCharacter& Source)
	{
		FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatTargetProjectile), false, &Source);
		for (TActorIterator<AmultiplayerCombatCharacter> It(Source.GetWorld()); It; ++It)
			if (!Source.IsHostileTo(*It) || !It->IsCombatReady()) Query.AddIgnoredActor(*It);
		return Query;
	}

	FVector ProjectileTrace(const AmultiplayerCombatCharacter& Source, const FVector& Start, const FVector& End,
		float Radius, TWeakObjectPtr<AmultiplayerCombatCharacter>& Target)
	{
		TArray<FHitResult> Hits;
		Source.GetWorld()->SweepMultiByChannel(Hits, Start, End, FQuat::Identity, ECC_WorldDynamic,
			FCollisionShape::MakeSphere(FMath::Max(.1f, Radius)), ProjectileQuery(Source));
		for (const FHitResult& Hit : Hits)
		{
			if (auto* Character = Cast<AmultiplayerCombatCharacter>(Hit.GetActor());
				Character && Source.IsHostileTo(Character) && Character->IsCombatReady())
			{
				Target = Character;
				return Hit.Location;
			}
			if (Hit.bBlockingHit) return Hit.Location;
		}
		return End;
	}
}

// 小兵的预警与真正出手共用距离、正面角度和遮挡；客户端不需要服务器的AIController。
bool CombatTargeting::CanMeleeHit(const AmultiplayerCombatCharacter& Source,
	const AmultiplayerCombatCharacter& Target, bool bCheckFacing)
{
	const FVector Offset = Target.GetActorLocation() - Source.GetActorLocation();
	if (!Target.IsCombatReady() || !Target.IsPlayerControlled() || !Source.IsHostileTo(&Target)
		|| Offset.SizeSquared() > FMath::Square(Source.GetSkillRule(EMultiplayerCombatSkill::Attack).Range)
		|| (bCheckFacing && FVector::DotProduct(Source.GetActorForwardVector(), Offset.GetSafeNormal2D()) < MeleeFrontDot)) return false;
	FVector Eye; FRotator Rotation;
	Source.GetActorEyesViewPoint(Eye, Rotation);
	FHitResult Hit;
	return !Source.GetWorld()->LineTraceSingleByChannel(Hit, Eye, Target.GetActorLocation(), ECC_Visibility,
		FCollisionQueryParams(SCENE_QUERY_STAT(CombatMeleeVisibility), false, &Source)) || Hit.GetActor() == &Target;
}

// 范围圈表示水平半径，检测使用有限高度圆柱；地面边缘不再被球体截掉。
bool CombatTargeting::CanAffectArea(const AmultiplayerCombatCharacter& Source,
	const AmultiplayerCombatCharacter& Target, const FVector& Center, float Radius)
{
	if (!Source.IsHostileTo(&Target) || !Target.IsCombatReady()
		|| FVector::DistSquared2D(Center, Target.GetActorLocation()) > FMath::Square(Radius)
		|| FMath::Abs(Target.GetActorLocation().Z - Center.Z) > AreaHalfHeight) return false;
	FHitResult Hit;
	return !Source.GetWorld()->LineTraceSingleByChannel(Hit, Center + FVector(0, 0, 40), Target.GetActorLocation(),
		ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(CombatAreaVisibility), false, &Source), SceneResponses())
		|| Hit.GetActor() == &Target;
}

TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> CombatTargeting::CollectAreaCandidates(
	const AmultiplayerCombatCharacter& Source, const FVector& Center, float Radius)
{
	TArray<FOverlapResult> Hits;
	Source.GetWorld()->OverlapMultiByObjectType(Hits, Center, FQuat::Identity, FCollisionObjectQueryParams(ECC_Pawn),
		FCollisionShape::MakeBox(FVector(Radius, Radius, AreaHalfHeight)),
		FCollisionQueryParams(SCENE_QUERY_STAT(CombatAreaCandidates), false, &Source));
	TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> Targets;
	for (const FOverlapResult& Hit : Hits)
		if (auto* Target = Cast<AmultiplayerCombatCharacter>(Hit.GetActor()))
			Targets.AddUnique(Target);
	return Targets;
}

TArray<TWeakObjectPtr<AmultiplayerCombatCharacter>> CombatTargeting::CollectAreaTargets(
	const AmultiplayerCombatCharacter& Source, const FVector& Center, float Radius)
{
	auto Targets = CollectAreaCandidates(Source, Center, Radius);
	Targets.RemoveAll([&](const auto& Weak)
	{
		const auto* Target = Weak.Get();
		return !Target || !CanAffectArea(Source, *Target, Center, Radius);
	});
	return Targets;
}

FVector AmultiplayerCombatCharacter::GetCombatViewOrigin(const FRotator& Aim) const
{
	return GetPawnViewLocation();
}

FVector AmultiplayerCombatCharacter::GetCombatMuzzleLocation(const FRotator* AimSnapshot) const
{
	// 不依赖DS骨骼刷新；起手特效、轨迹预览和真实弹体从同一位置出发。
	const FRotator Aim = AimSnapshot ? *AimSnapshot
		: bHasAttackAim && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Attacking)
			? ActiveAttackAim : GetControlRotation();
	const FVector Chest = GetActorLocation() + FVector(0, 0, 40);
	// 发射点在身体正面胶囊外，额外留出弹体半径和6cm间距；俯仰只影响弹道，不把起点压进地面。
	const float ForwardDistance = GetCapsuleComponent()->GetScaledCapsuleRadius() + CombatTargeting::ProjectileRadius + 6.f;
	const FVector Forward = Chest + FRotator(0, Aim.Yaw, 0).Vector() * ForwardDistance;
	if (GetWorld())
	{
		// 近墙或贴身敌人不能被前移起点跳过；这种情况下仍从胸前原点开始真实扫掠。
		TWeakObjectPtr<AmultiplayerCombatCharacter> EarlyTarget;
		const FVector Reachable = ProjectileTrace(*this, Chest, Forward, CombatTargeting::ProjectileRadius, EarlyTarget);
		if (EarlyTarget.IsValid() || !Reachable.Equals(Forward, .01f)) return Chest;
	}
	return Forward;
}

// 客户端不提交治疗目标；服务器在接纳与实际提交时共用同一套选目标规则。
AmultiplayerCombatCharacter* AmultiplayerCombatCharacter::GetHealTarget() const
{
	// 优先最低血量比例；比例相同时选更近的目标，自身也属于合法候选。
	AmultiplayerCombatCharacter* Selected = nullptr;
	float LowestRatio = 1.f;
	float ClosestDistance = FMath::Square(GetSkillRule(EMultiplayerCombatSkill::Heal).Range);
	FVector EyeLocation; FRotator EyeRotation;
	GetActorEyesViewPoint(EyeLocation, EyeRotation);
	for (TActorIterator<AmultiplayerCombatCharacter> It(GetWorld()); It; ++It)
	{
		auto* Candidate = *It;
		if (!Candidate->IsCombatReady() || IsHostileTo(Candidate) || Candidate->GetHealth() >= Candidate->GetMaxHealth()) continue;
		const float Distance = FVector::DistSquared(GetActorLocation(), Candidate->GetActorLocation());
		if (Distance > FMath::Square(GetSkillRule(EMultiplayerCombatSkill::Heal).Range)) continue;
		// 对自己无需射线；对队友允许射线命中目标自身，但不能穿过墙或其他遮挡。
		FHitResult Hit;
		if (Candidate != this && GetWorld()->LineTraceSingleByChannel(Hit, EyeLocation, Candidate->GetActorLocation(),
			ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(CoopHealLineOfSight), false, this)) && Hit.GetActor() != Candidate) continue;
		const float Ratio = Candidate->GetHealth() / FMath::Max(1.f, Candidate->GetMaxHealth());
		if (Ratio < LowestRatio || (FMath::IsNearlyEqual(Ratio, LowestRatio) && Distance < ClosestDistance))
		{
			Selected = Candidate; LowestRatio = Ratio; ClosestDistance = Distance;
		}
	}
#if !UE_BUILD_SHIPPING
	// 无 AI 手动演示允许满血对自己释放；正式游戏仍要求真实受伤目标。
	if (!Selected && IsCombatReady() && FParse::Param(FCommandLine::Get(), TEXT("CoopSkillDemo")))
		return const_cast<AmultiplayerCombatCharacter*>(this);
#endif
	return Selected;
}

// 一次查询返回可展示的几何与真实候选；表现层不再复制一份选敌或范围计算。
FmultiplayerCombatTargetPreview AmultiplayerCombatCharacter::GetSkillTargetPreview(
	EMultiplayerCombatSkill Skill, const FRotator* AimSnapshot) const
{
	FmultiplayerCombatTargetPreview Result;
	Result.Skill = Skill;
	Result.Origin = Result.End = GetActorLocation();
	const FRotator Aim = AimSnapshot ? *AimSnapshot
		: Skill == EMultiplayerCombatSkill::Attack && bHasAttackAim && AbilitySystem->HasMatchingGameplayTag(CombatTags::State_Attacking)
			? ActiveAttackAim : GetControlRotation();
	if (Aim.ContainsNaN() || !GetWorld()) return Result;
	const auto Rule = GetSkillRule(Skill);
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CombatTargetGround), false, this);
	if (Skill == EMultiplayerCombatSkill::Attack)
	{
		Result.Origin = GetCombatMuzzleLocation(&Aim);
		const float LifetimeRange = GetCombatConfig().ProjectileSpeed * CombatTargeting::ProjectileLifetime;
		const float Range = Rule.Range > 0 ? FMath::Min(Rule.Range, LifetimeRange) : LifetimeRange;
		// 玩家普攻沿本轮身体出手的水平正前方发射；自由镜头俯仰不再把普攻引向地面。
		FVector DesiredEnd = Result.Origin + FRotator(0, Aim.Yaw, 0).Vector() * Range;
		if (GetCombatTeam() != EMultiplayerCombatTeam::Players)
		{
			// 远程敌人仍保留其AI目标的三维瞄准，不因玩家前射规则丢失高度差命中。
			const FVector View = GetCombatViewOrigin(Aim);
			TWeakObjectPtr<AmultiplayerCombatCharacter> ViewTarget;
			FVector Point = ProjectileTrace(*this, View, View + Aim.Vector() * (Range + FVector::Distance(View, Result.Origin)), .1f, ViewTarget);
			if (FVector::DotProduct(Point - Result.Origin, Aim.Vector()) <= 0) Point = Result.Origin + Aim.Vector() * Range;
			DesiredEnd = Result.Origin + (Point - Result.Origin).GetSafeNormal() * Range;
		}
		Result.End = ProjectileTrace(*this, Result.Origin, DesiredEnd, CombatTargeting::ProjectileRadius, Result.Target);
		Result.Radius = CombatTargeting::ProjectileRadius;
		Result.bValid = !GetWorld()->OverlapBlockingTestByChannel(Result.Origin, FQuat::Identity, ECC_WorldDynamic,
			FCollisionShape::MakeSphere(Result.Radius), Query, SceneResponses());
		if (Result.Target.IsValid()) Result.AffectedTargets.Add(Result.Target);
		return Result;
	}
	if (Skill == EMultiplayerCombatSkill::Heal)
	{
		Result.Radius = Rule.Range;
		Result.Target = GetHealTarget();
		Result.bValid = Result.Target.IsValid();
		if (Result.Target.IsValid()) { Result.End = Result.Target->GetActorLocation(); Result.AffectedTargets.Add(Result.Target); }
		return Result;
	}
	if (Skill == EMultiplayerCombatSkill::Defend)
	{
		Result.End = Result.Origin + GetActorForwardVector().GetSafeNormal2D() * 120.f;
		Result.bValid = true;
		return Result;
	}
	if (Skill == EMultiplayerCombatSkill::Dash)
	{
		const FVector DesiredEnd = Result.Origin + FRotator(0, Aim.Yaw, 0).Vector() * Rule.Range;
		FHitResult Hit;
		const auto* Capsule = GetCapsuleComponent();
		const bool bBlocked = GetWorld()->SweepSingleByChannel(Hit, Result.Origin, DesiredEnd, FQuat::Identity,
			Capsule->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Capsule->GetScaledCapsuleRadius(),
				Capsule->GetScaledCapsuleHalfHeight()), Query, FCollisionResponseParams(Capsule->GetCollisionResponseToChannels()));
		// 留出极小接触余量；实际根运动也在同一点结束，不持续推墙再沿墙滑行。
		Result.End = bBlocked ? Hit.Location - (DesiredEnd - Result.Origin).GetSafeNormal() * FMath::Min(2.f, Hit.Distance) : DesiredEnd;
		Result.Radius = Capsule->GetScaledCapsuleRadius();
		Result.bValid = !Hit.bStartPenetrating && FVector::DistSquared2D(Result.Origin, Result.End) > FMath::Square(5.f);
		return Result;
	}
	Result.Radius = GetCombatConfig().AreaRadius;
	if (IsCasting() && bHasAreaTarget)
	{
		Result.End = ConfirmedAreaTarget; Result.Normal = ConfirmedAreaNormal; Result.bValid = true;
	}
	else
	{
		const FVector View = GetCombatViewOrigin(Aim);
		FVector Point = GetActorLocation() + FRotator(0, Aim.Yaw, 0).Vector() * Rule.Range;
		FHitResult Hit;
		// 先沿镜头选地面，再限制角色可施法射程；平视时保留最大射程投地。
		if (GetWorld()->LineTraceSingleByChannel(Hit, View, View + Aim.Vector() * 10000.f,
			ECC_Visibility, Query, SceneResponses())) Point = Hit.ImpactPoint;
		else if (Aim.Vector().Z > .05f) { Result.End = Point; return Result; }
		FVector Offset = Point - GetActorLocation(); Offset.Z = 0;
		if (Offset.SizeSquared() > FMath::Square(Rule.Range)) Point = GetActorLocation() + Offset.GetSafeNormal() * Rule.Range;
		// 从身体检查到目标的遮挡，不能用绕过墙后的相机位置施法。
		if (GetWorld()->LineTraceSingleByChannel(Hit, GetActorLocation(), FVector(Point.X, Point.Y, GetActorLocation().Z),
			ECC_Visibility, Query, SceneResponses())) Point = Hit.ImpactPoint - (Point - GetActorLocation()).GetSafeNormal2D() * 5.f;
		if (GetWorld()->LineTraceSingleByChannel(Hit, Point + FVector(0, 0, 150), Point - FVector(0, 0, 1000),
			ECC_Visibility, Query, SceneResponses()) && Hit.ImpactNormal.Z >= GetCharacterMovement()->GetWalkableFloorZ())
		{
			Result.End = Hit.ImpactPoint; Result.Normal = Hit.ImpactNormal; Result.bValid = true;
		}
		else Result.End = Point;
	}
	if (Result.bValid)
		Result.AffectedTargets = CombatTargeting::CollectAreaTargets(*this, Result.End, Result.Radius);
	return Result;
}

void AmultiplayerCombatCharacter::SetAreaCastTarget(const FVector& Location, const FVector& Normal)
{
	ConfirmedAreaTarget = Location;
	ConfirmedAreaNormal = Normal;
	bHasAreaTarget = true;
	if (HasAuthority()) ForceNetUpdate();
}

void AmultiplayerCombatCharacter::BeginSlotTargeting(int32 Slot)
{
	BeginSkillTargeting(Progression->GetSlotSkill(Slot));
}

void AmultiplayerCombatCharacter::BeginSkillTargeting(EMultiplayerCombatSkill Skill)
{
	if (!IsLocallyControlled()) return;
	TargetingSkill = Skill;
	bSkillTargeting = true;
	// 新瞄准展示当前技能资格，清掉上一技能的短时失败提示，避免两者相互矛盾。
	OnCombatMessage.Broadcast(FText::GetEmpty());
	OnCombatStateChanged.Broadcast();
}

void AmultiplayerCombatCharacter::ConfirmSkillTargeting()
{
	if (!bSkillTargeting) return;
	const auto Skill = TargetingSkill;
	// 先关闭预览并完成本次请求，再通知外部；监听者取消或切换时，不得让旧请求在通知后才启动。
	bSkillTargeting = false;
	ON_SCOPE_EXIT { OnCombatStateChanged.Broadcast(); };
	FGameplayTagContainer Reasons;
	if (!CheckSkillAvailable(Skill, &Reasons)) { ReportSkillFailure(Skill, &Reasons); return; }
	const auto Preview = GetSkillTargetPreview(Skill);
	if (!Preview.bValid)
	{
		const FGameplayTag Reason = Skill == EMultiplayerCombatSkill::Heal ? CombatTags::Failure_NoHealTarget
			: Skill == EMultiplayerCombatSkill::Area ? CombatTags::Failure_InvalidGround
			: Skill == EMultiplayerCombatSkill::Dash ? CombatTags::Failure_DashBlocked : CombatTags::Failure_MuzzleBlocked;
		OnCombatMessage.Broadcast(GetSkillFailureText(Skill, Reason));
		return;
	}
	ActivateSkill(Skill);
}

void AmultiplayerCombatCharacter::CancelSkillTargeting()
{
	if (!bSkillTargeting) return;
	bSkillTargeting = false;
	OnCombatStateChanged.Broadcast();
}
