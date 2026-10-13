// Copyright Epic Games, Inc. All Rights Reserved.

#include "Combat/multiplayerCombatProjectile.h"

#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAbility.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatTags.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

// 根球体承担扫掠碰撞，可见网格不参与碰撞；投射物不依赖角色动画或额外美术资源。
AmultiplayerCombatProjectile::AmultiplayerCombatProjectile()
{
	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(60.0f);
	InitialLifeSpan = 3.0f;

	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	SetRootComponent(CollisionSphere);
	CollisionSphere->InitSphereRadius(12.0f);
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
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->bInitialVelocityInLocalSpace = false;
}

// 只在服务器初始化真实投射物，忽略来源角色避免在枪口附近重复重叠。
void AmultiplayerCombatProjectile::Initialize(AmultiplayerCombatCharacter* Source, float Damage, float Speed)
{
	if (!HasAuthority() || !IsValid(Source))
	{
		return;
	}

	SourceCharacter = Source;
	DamageAmount = FMath::Max(0.0f, Damage);
	SetOwner(Source);
	SetInstigator(Source);
	ProjectileMovement->InitialSpeed = FMath::Max(0.0f, Speed);
	ProjectileMovement->MaxSpeed = ProjectileMovement->InitialSpeed;
	ProjectileMovement->Velocity = GetActorForwardVector() * ProjectileMovement->InitialSpeed;

	CollisionSphere->IgnoreActorWhenMoving(Source, true);
}

// 客户端不做碰撞和二次模拟；服务器位移复制负责显示真实弹道。
void AmultiplayerCombatProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		CollisionSphere->OnComponentHit.AddUniqueDynamic(this, &AmultiplayerCombatProjectile::HandleHit);
		CollisionSphere->OnComponentBeginOverlap.AddUniqueDynamic(this, &AmultiplayerCombatProjectile::HandleOverlap);
	}
	else
	{
		CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ProjectileMovement->SetComponentTickEnabled(false);
	}
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
		ResolveImpact(Target, SweepResult);
	}
}

// 撞到敌人施加伤害，撞到场景直接销毁；来源死亡不撤销已发出的攻击，来源销毁则不再伤害。
void AmultiplayerCombatProjectile::ResolveImpact(AActor* OtherActor, const FHitResult& Hit)
{
	if (!HasAuthority() || bHitResolved)
	{
		return;
	}

	AmultiplayerCombatCharacter* Source = SourceCharacter.Get();
	AmultiplayerCombatCharacter* Target = Cast<AmultiplayerCombatCharacter>(OtherActor);
	if (Source != nullptr && IsValid(Target) && !Source->IsHostileTo(Target))
	{
		return;
	}

	// 先终止碰撞与移动，再通知伤害监听者；同步回调不能重复结算这颗投射物。
	bHitResolved = true;
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ProjectileMovement->StopMovementImmediately();
	if (IsValid(Source) && IsValid(Target) && Target->IsAlive())
	{
		UAbilitySystemComponent* SourceASC = Source->GetAbilitySystemComponent();
		UAbilitySystemComponent* TargetASC = Target->GetAbilitySystemComponent();
		if (SourceASC != nullptr && TargetASC != nullptr)
		{
			FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
			Context.AddSourceObject(this);
			Context.AddHitResult(Hit);
			FGameplayEffectSpecHandle DamageSpec = SourceASC->MakeOutgoingSpec(UmultiplayerDamageEffect::StaticClass(), 1.0f, Context);
			if (DamageSpec.IsValid())
			{
				DamageSpec.Data->SetSetByCallerMagnitude(CombatTags::Data_Damage, DamageAmount);
				SourceASC->ApplyGameplayEffectSpecToTarget(*DamageSpec.Data.Get(), TargetASC);
			}
		}
	}
	Destroy();
}
