// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatProjectile.h"

#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Combat/multiplayerCombatTargeting.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "NiagaraComponent.h"
#include "NiagaraFunctionLibrary.h"
#include "NiagaraSystem.h"
#include "Particles/ParticleSystem.h"
#include "Particles/ParticleSystemComponent.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

// 根球体承担扫掠碰撞，可见网格不参与碰撞；投射物不依赖角色动画或额外美术资源。
AmultiplayerCombatProjectile::AmultiplayerCombatProjectile()
{
	bReplicates = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(30.0f);
	InitialLifeSpan = CombatTargeting::ProjectileLifetime;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	SetRootComponent(CollisionSphere);
	CollisionSphere->InitSphereRadius(CombatTargeting::ProjectileRadius);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);
	// Pawn 用重叠而非阻挡，友方（包括发射后才加入的角色）不会截停能量弹。
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
	ProjectileMesh->SetupAttachment(CollisionSphere);
	ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileMesh->SetVisibility(false);
	ProjectileMesh->SetRelativeScale3D(FVector(0.24f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		ProjectileMesh->SetStaticMesh(SphereMesh.Object);
	}

	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->SetUpdatedComponent(CollisionSphere);
	ProjectileMovement->InitialSpeed = 1600.0f;
	ProjectileMovement->MaxSpeed = 1600.0f;
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->bShouldBounce = false;
	ProjectileMovement->bIsHomingProjectile = false;
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bInitialVelocityInLocalSpace = false;
	ProjectileMovement->bAutoActivate = false;
	ProjectileMovement->PrimaryComponentTick.bStartWithTickEnabled = false;
}

// 只在服务器初始化真实投射物，忽略来源角色避免在枪口附近重复重叠。
void AmultiplayerCombatProjectile::Initialize(AmultiplayerCombatCharacter* Source, float Damage, float Speed, float MaxDistance)
{
	if (!HasAuthority() || !IsValid(Source) || LaunchState.bInitialized)
	{
		return;
	}

	SourceCharacter = Source;
	// 有显式射程时真实弹体也按距离结束，不能越过预览的最大射程继续造成伤害。
	if (MaxDistance > 0 && Speed > 0) InitialLifeSpan = FMath::Min(CombatTargeting::ProjectileLifetime, MaxDistance / Speed);
	DamageAmount = FMath::Max(0.0f, Damage);
	ReflectionLimit = Source->GetCombatConfig().MaxProjectileReflections;
	SetOwner(Source);
	SetInstigator(Source);
	ProjectileMovement->InitialSpeed = FMath::Max(0.0f, Speed);
	ProjectileMovement->MaxSpeed = ProjectileMovement->InitialSpeed;
	ProjectileMovement->Velocity = GetActorForwardVector() * ProjectileMovement->InitialSpeed;
	LaunchState.Origin = FRepMovement::RebaseOntoZeroOrigin(GetActorLocation(), this);
	LaunchState.Velocity = ProjectileMovement->Velocity;
	LaunchState.ServerLaunchTime = GetWorld()->GetTimeSeconds();
	LaunchState.ServerExpiryTime = LaunchState.ServerLaunchTime + InitialLifeSpan;
	LaunchState.Revision = 1;
	LaunchState.bInitialized = true;

	CollisionSphere->IgnoreActorWhenMoving(Source, true);
}

// 服务器组件执行真实扫掠；客户端关闭碰撞和组件模拟，等待发射状态与服务器时钟。
void AmultiplayerCombatProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		CollisionSphere->OnComponentHit.AddUniqueDynamic(this, &AmultiplayerCombatProjectile::HandleHit);
		CollisionSphere->OnComponentBeginOverlap.AddUniqueDynamic(this, &AmultiplayerCombatProjectile::HandleOverlap);
		if (LaunchState.bInitialized)
		{
			ProjectileMesh->SetVisibility(true);
			ProjectileMovement->Activate(true);
		}
	}
	else
	{
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ProjectileMovement->bSimulationEnabled = false;
		ProjectileMovement->SetComponentTickEnabled(false);
		StartClientTrajectory();
	}
	RequestProjectileEffect();
}

// 初始状态随每条新 Actor 通道发送；弹反后发送最新段落，晚相关客户端不必重放之前的碰撞。
void AmultiplayerCombatProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCombatProjectile, LaunchState);
}

// 状态可能先于 BeginPlay 收到；后者会再次进入入口，避免漏启表现。
void AmultiplayerCombatProjectile::OnRep_LaunchState()
{
	if (HasActorBegunPlay()) StartClientTrajectory();
}

// Actor 的 Owner 引用可能晚于初始发射包解析；补齐角色配置即可，不轮询 Owner。
void AmultiplayerCombatProjectile::OnRep_Owner()
{
	Super::OnRep_Owner();
	if (HasActorBegunPlay()) RequestProjectileEffect();
}

// 只在发射入口查找 GameState；未到达时短暂重试，不拿客户端世界时间误算弹龄。
void AmultiplayerCombatProjectile::StartClientTrajectory()
{
	if (HasAuthority() || !LaunchState.bInitialized) return;
	ClientGameState = GetWorld()->GetGameState();
	if (!ClientGameState.IsValid())
	{
		GetWorldTimerManager().SetTimer(ClientLaunchRetry, this, &AmultiplayerCombatProjectile::StartClientTrajectory, 0.05f, false);
		return;
	}
	GetWorldTimerManager().ClearTimer(ClientLaunchRetry);
	SetActorRotation(FVector(LaunchState.Velocity).Rotation());
	SetActorTickEnabled(true);
	UpdateClientTrajectory();
	RequestProjectileEffect();
}

// 角色通常已预载素材；漏过预载窗口的短命弹体仍只发一次异步请求，并保留句柄到结束。
void AmultiplayerCombatProjectile::RequestProjectileEffect()
{
	if (GetNetMode() == NM_DedicatedServer || bPresentationEnded || ProjectileEffect || ProjectileCascadeEffect || !LaunchState.bInitialized) return;
	const auto* Source = Cast<AmultiplayerCombatCharacter>(GetOwner());
	if (!Source) return;
	ProjectileEffectAsset = Source->GetCombatConfig().ProjectileEffect;
	ProjectileCascadeAsset = Source->GetCombatConfig().ProjectileCascadeEffect;
	const bool bUseCascade = !ProjectileCascadeAsset.IsNull();
	const FSoftObjectPath EffectPath = bUseCascade ? ProjectileCascadeAsset.ToSoftObjectPath() : ProjectileEffectAsset.ToSoftObjectPath();
	if (EffectPath.IsNull()) return;
	if (bUseCascade ? ProjectileCascadeAsset.IsValid() : ProjectileEffectAsset.IsValid()) AttachProjectileEffect();
	else if (!ProjectileEffectLoad.IsValid())
	{
		ProjectileEffectLoad = UAssetManager::GetStreamableManager().RequestAsyncLoad(
			EffectPath, FStreamableDelegate::CreateUObject(this, &ThisClass::AttachProjectileEffect));
	}
}

// 先按弹龄定位再显示；资源加载过晚时不补播已结束的飞行，也不改变命中或生命周期。
void AmultiplayerCombatProjectile::AttachProjectileEffect()
{
	if (bPresentationEnded || GetNetMode() == NM_DedicatedServer || ProjectileEffect || ProjectileCascadeEffect || bHitResolved) return;
	const AGameStateBase* Clock = ClientGameState.Get();
	if (!HasAuthority() && !Clock) return;
	const double Age = HasAuthority() ? GetWorld()->GetTimeSeconds() - LaunchState.ServerLaunchTime
		: Clock->GetServerWorldTimeSeconds() - LaunchState.ServerLaunchTime;
	const double SegmentLifetime = FMath::Max(0.0, LaunchState.ServerExpiryTime - LaunchState.ServerLaunchTime);
	if (Age >= SegmentLifetime) return;
	if (!ProjectileCascadeAsset.IsNull())
	{
		if (auto* System = ProjectileCascadeAsset.Get())
			ProjectileCascadeEffect = UGameplayStatics::SpawnEmitterAttached(System, CollisionSphere, NAME_None,
				FVector::ZeroVector, FRotator::ZeroRotator, EAttachLocation::SnapToTarget, false);
		if (ProjectileCascadeEffect) ProjectileMesh->SetVisibility(false);
		return;
	}
	UNiagaraSystem* System = ProjectileEffectAsset.Get();
	if (!System) return;
	ProjectileEffect = UNiagaraFunctionLibrary::SpawnSystemAttached(System, CollisionSphere, NAME_None,
		FVector::ZeroVector, FRotator::ZeroRotator, EAttachLocation::SnapToTarget, false, false,
		ENCPoolMethod::None, false);
	if (!ProjectileEffect) return;
	ProjectileEffect->SetVariableFloat(TEXT("User.Duration"), float(SegmentLifetime - FMath::Max(0.0, Age)));
	ProjectileEffect->Activate(true);
	ProjectileMesh->SetVisibility(false);
}

// 时钟同步首包可能晚于弹体；逐帧读取缓存时钟的估计值即可自动纠正，无需位置复制。
void AmultiplayerCombatProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateClientTrajectory();
}

// 这里只做显示数学，不执行 sweep、命中或销毁；服务器销毁到达前可能短暂视觉过冲。
void AmultiplayerCombatProjectile::UpdateClientTrajectory()
{
	const AGameStateBase* Clock = ClientGameState.Get();
	if (!Clock) return;
	const double Age = FMath::Max(0.0, Clock->GetServerWorldTimeSeconds() - LaunchState.ServerLaunchTime);
	const double SegmentLifetime = FMath::Max(0.0, LaunchState.ServerExpiryTime - LaunchState.ServerLaunchTime);
	const bool bVisible = Age < SegmentLifetime;
	ProjectileMesh->SetVisibility(bVisible && !ProjectileEffect && !ProjectileCascadeEffect);
	if (ProjectileEffect)
	{
		ProjectileEffect->SetVisibility(bVisible, true);
		if (!bVisible && ProjectileEffect->IsActive()) ProjectileEffect->DeactivateImmediate();
	}
	if (ProjectileCascadeEffect)
	{
		ProjectileCascadeEffect->SetVisibility(bVisible, true);
		if (!bVisible && ProjectileCascadeEffect->IsActive()) ProjectileCascadeEffect->DeactivateSystem();
	}
	const FVector Origin = FRepMovement::RebaseOntoLocalOrigin(LaunchState.Origin, this);
	SetActorLocation(Origin + FVector(LaunchState.Velocity) * FMath::Min(Age, SegmentLifetime), false);
}

// 结束相关性或收到服务器销毁时清理入口重试，客户端不另行延长或决定弹体寿命。
void AmultiplayerCombatProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(ClientLaunchRetry);
	GetWorldTimerManager().ClearTimer(ReflectionResume);
	bPresentationEnded = true;
	if (ProjectileEffectLoad.IsValid())
	{
		ProjectileEffectLoad->CancelHandle();
		ProjectileEffectLoad.Reset();
	}
	if (ProjectileEffect)
	{
		ProjectileEffect->DestroyComponent();
		ProjectileEffect = nullptr;
	}
	if (ProjectileCascadeEffect)
	{
		ProjectileCascadeEffect->DestroyComponent();
		ProjectileCascadeEffect = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

// 场景阻挡只结束投射物，与角色重叠共用一次性结算逻辑。
void AmultiplayerCombatProjectile::HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
	ResolveImpact(OtherActor, Hit);
}

// 非战斗角色和队友直接穿过，只有敌方角色进入碰撞球时才消耗投射物。
void AmultiplayerCombatProjectile::HandleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult)
{
	AmultiplayerCombatCharacter* Source = SourceCharacter.Get();
	AmultiplayerCombatCharacter* Target = Cast<AmultiplayerCombatCharacter>(OtherActor);
	if (Source != nullptr && IsValid(Target) && Source->IsHostileTo(Target))
	{
		// 出生重叠没有有效扫掠接触点；用服务器弹体位置补齐上下文，避免命中特效落到世界原点。
		// 该点代表当前重叠位置，不伪造精确表面接触；真正的扫掠结果保持原样。
		const FHitResult Hit = bFromSweep ? SweepResult
			: FHitResult(Target, OtherComponent, GetActorLocation(), -ProjectileMovement->Velocity.GetSafeNormal());
		ResolveImpact(Target, Hit);
	}
}

// 弹反是服务器对当前来向的裁决，不读取客户端命中，也不把 Q 的减伤持续时间当成弹反窗口。
bool AmultiplayerCombatProjectile::TryReflectFrom(AmultiplayerCombatCharacter* Defender, const FHitResult& Hit)
{
	AmultiplayerCombatCharacter* PreviousSource = SourceCharacter.Get();
	if (!HasAuthority() || bHitResolved || bReflectionPending || !LaunchState.bInitialized
		|| !IsValid(PreviousSource) || !IsValid(Defender) || !Defender->IsCombatReady()
		|| !Defender->IsParrying() || !PreviousSource->IsHostileTo(Defender)
		|| ReflectionCount >= ReflectionLimit || GetWorld()->GetTimeSeconds() >= LaunchState.ServerExpiryTime) return false;

	const FVector IncomingVelocity = ProjectileMovement->Velocity;
	const FVector Normal = Defender->GetActorForwardVector().GetSafeNormal2D();
	if (IncomingVelocity.IsNearlyZero() || FVector::DotProduct(-IncomingVelocity.GetSafeNormal(), Normal)
		< Defender->GetCombatConfig().ParryFrontDot) return false;
	const FVector ReflectedVelocity = IncomingVelocity - 2.f * FVector::DotProduct(IncomingVelocity, Normal) * Normal;
	// Overlap 在整段 Move 完成后才通知；用真实扫掠接触的球心回到接触点，而非从穿过目标后的终点反弹。
	const FVector Contact = Hit.GetActor() == Defender ? FVector(Hit.Location) : GetActorLocation();
	const FVector Origin = Contact + ReflectedVelocity.GetSafeNormal() * 2.f;
	bReflectionPending = true;
	ProjectileMovement->Deactivate();
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	CollisionSphere->IgnoreActorWhenMoving(PreviousSource, false);
	CollisionSphere->IgnoreActorWhenMoving(Defender, true);
	SourceCharacter = Defender;
	SetOwner(Defender);
	SetInstigator(Defender);
	SetActorLocationAndRotation(Origin, ReflectedVelocity.Rotation(), false);
	++ReflectionCount;
	LaunchState.Origin = FRepMovement::RebaseOntoZeroOrigin(Origin, this);
	LaunchState.Velocity = ReflectedVelocity;
	LaunchState.ServerLaunchTime = GetWorld()->GetTimeSeconds();
	++LaunchState.Revision;
	// Deactivate 已清空速度；新值留在本次快照中，下一 Tick 从相同位置和方向继续真实扫掠。
	ReflectionResume = GetWorldTimerManager().SetTimerForNextTick(this, &ThisClass::ResumeReflectedTrajectory);
	ForceNetUpdate();
	return true;
}

// 同次旧方向扫掠的通知已结束后再开碰撞；旧来源现在是合法敌方，新防御者不再截住自己的弹体。
void AmultiplayerCombatProjectile::ResumeReflectedTrajectory()
{
	if (!HasAuthority() || bHitResolved || IsActorBeingDestroyed()) return;
	const uint16 ResumingRevision = LaunchState.Revision;
	ProjectileMovement->SetUpdatedComponent(CollisionSphere);
	ProjectileMovement->Velocity = LaunchState.Velocity;
	bReflectionPending = false;
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	// 开碰撞也可能同步触发另一个真实重叠；不能覆盖该回调刚结束或再次反射的状态。
	if (bHitResolved || bReflectionPending || LaunchState.Revision != ResumingRevision) return;
	ProjectileMovement->Activate(true);
}

// 撞到敌人施加伤害，撞到场景直接销毁；来源死亡不撤销已发出的攻击，来源销毁则不再伤害。
void AmultiplayerCombatProjectile::ResolveImpact(AActor* OtherActor, const FHitResult& Hit)
{
	if (!HasAuthority() || bHitResolved || bReflectionPending || bResolvingImpact)
	{
		return;
	}
	TGuardValue<bool> ResolvingGuard(bResolvingImpact, true);

	AmultiplayerCombatCharacter* Source = SourceCharacter.Get();
	AmultiplayerCombatCharacter* Target = Cast<AmultiplayerCombatCharacter>(OtherActor);
	if (Source != nullptr && IsValid(Target) && !Source->IsHostileTo(Target))
	{
		return;
	}
	if (IsValid(Target) && TryReflectFrom(Target, Hit)) return;

	// 先终止碰撞与移动，再通知伤害监听者；同步回调不能重复结算这颗投射物。
	bHitResolved = true;
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileMovement->StopMovementImmediately();
	if (IsValid(Source) && IsValid(Target) && Target->IsAlive())
	{
		Source->ApplyCombatDamage(Target, DamageAmount, &Hit);
	}
	Destroy();
}
