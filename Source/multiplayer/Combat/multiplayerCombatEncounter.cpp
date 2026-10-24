// Copyright Epic Games, Inc. All Rights Reserved.
// 波次只负责何时生成和最多生成多少，敌人能力不在关卡编排中结算。
// 补给先提交消费状态再修改属性，外部同步通知不能重入领取。
#include "Combat/multiplayerCombatEncounter.h"
#include "AbilitySystemComponent.h"
#include "Combat/multiplayerCombatAttributes.h"
#include "Combat/multiplayerCombatCharacter.h"
#include "Combat/multiplayerCombatEnemy.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameMode.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "BrainComponent.h"
#include "GameFramework/PlayerController.h"
#include "NativeGameplayTags.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

UE_DEFINE_GAMEPLAY_TAG_STATIC(SupplyHealth, "Data.Supply.Health");
UE_DEFINE_GAMEPLAY_TAG_STATIC(SupplyEnergy, "Data.Supply.Energy");

// 默认引用现有小兵蓝图；关卡编排本身无逐帧工作和独立客户端生成。
AmultiplayerCombatEncounter::AmultiplayerCombatEncounter()
{
	PrimaryActorTick.bCanEverTick = false;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("EncounterRoot")));
	static ConstructorHelpers::FClassFinder<AmultiplayerCombatEnemy> Minion(TEXT("/Game/Combat/Presentation/BP_CombatEnemy"));
	EnemyClass = Minion.Class;
}

// 两位玩家接纳后才开始；单机允许一人预览，不把原机关地图或独立测试出生器改成多波。
void AmultiplayerCombatEncounter::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_Client) return;
	BoundGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
	if (auto* State = BoundGameState.Get())
	{
		State->OnGameWon.AddUniqueDynamic(this, &ThisClass::StopEncounter);
		State->OnGameLost.AddUniqueDynamic(this, &ThisClass::StopEncounter);
		if (State->HasGameWon() || State->HasGameLost()) { StopEncounter(); return; }
	}
	BoundGameMode = GetWorld()->GetAuthGameMode<AmultiplayerGameMode>();
	if (auto* Mode = BoundGameMode.Get()) AcceptedHandle = Mode->OnCombatSessionAccepted.AddUObject(this, &ThisClass::TryStart);
	PublishState();
	TryStart();
}

// 接纳事件可重复到达，只有满足人数且尚未启动时才推进第一波。
void AmultiplayerCombatEncounter::TryStart()
{
	if (bStarted || bStopped || !EnemyClass || WaveSizes.IsEmpty()) return;
	int32 Ready = 0;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		if (const auto* Player = Cast<AmultiplayerCombatCharacter>(It->Get()->GetPawn()); Player && Player->IsCombatReady()) ++Ready;
	if (Ready < (GetNetMode() == NM_Standalone ? 1 : FMath::Max(1, StartPlayers))) return;
	bStarted = true;
	StartNextWave();
}

// 每次从弱引用和真实生命状态重算，避免重复死亡/销毁通知造成计数漂移。
int32 AmultiplayerCombatEncounter::GetLivingEnemyCount() const
{
	int32 Count = 0;
	for (auto Weak : Enemies) if (const auto* Enemy = Weak.Get(); Enemy && Enemy->IsAlive()) ++Count;
	return Count;
}

// 固定间隔施压，但存活与待出生敌人合计不得超过上限；容量不足时等待，不在同一帧补齐多个波次。
void AmultiplayerCombatEncounter::StartNextWave()
{
	if (bStopped || CurrentWave >= FMath::Min(WaveSizes.Num(), 10)) return;
	const int32 MaxCount = FMath::Clamp(MaxLivingEnemies, 1, 20);
	const int32 Count = FMath::Clamp(WaveSizes[CurrentWave], 1, FMath::Min(MaxCount, 10));
	int32 Pending = 0;
	for (auto Weak : Spawners) if (const auto* Spawner = Weak.Get()) Pending += Spawner->GetPendingSpawnCount();
	if (GetLivingEnemyCount() + Pending + Count > MaxCount)
	{
		GetWorldTimerManager().SetTimer(WaveTimer, this, &ThisClass::StartNextWave, 2.f);
		return;
	}
	const FTransform Transform(GetActorRotation(), GetActorLocation());
	auto* Spawner = GetWorld()->SpawnActorDeferred<AmultiplayerEnemySpawner>(AmultiplayerEnemySpawner::StaticClass(), Transform, this);
	if (!Spawner || !Spawner->ConfigureWave(EnemyClass, Count))
	{
		if (Spawner) Spawner->Destroy();
		StopEncounter();
		return;
	}
	++CurrentWave;
	Spawners.Add(Spawner);
	Spawner->OnEnemySpawned.AddUObject(this, &ThisClass::RegisterEnemy);
	Spawner->FinishSpawning(Transform);
	PublishState();
	if (!bStopped && CurrentWave < FMath::Min(WaveSizes.Num(), 10))
		GetWorldTimerManager().SetTimer(WaveTimer, this, &ThisClass::StartNextWave, FMath::Max(5.f, WaveInterval));
}

// 出生后才登记实体并监听状态，终局不再接收晚到的出生回调。
void AmultiplayerCombatEncounter::RegisterEnemy(AmultiplayerCombatEnemy* Enemy)
{
	if (bStopped || !IsValid(Enemy)) return;
	Enemies.AddUnique(Enemy);
	Enemy->OnCombatStateChanged.AddUniqueDynamic(this, &ThisClass::HandleEnemyChanged);
	Enemy->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandleEnemyDestroyed);
	PublishState();
}
// 属性或生命阶段变化只触发一次当前快照重算，不直接增减计数。
void AmultiplayerCombatEncounter::HandleEnemyChanged() { PublishState(); }
// 移除销毁对象及其他已失效引用，之后发布同一权威计数。
void AmultiplayerCombatEncounter::HandleEnemyDestroyed(AActor* Actor)
{
	Enemies.RemoveAll([Actor](auto Weak) { return !Weak.IsValid() || Weak.Get() == Actor; });
	PublishState();
}
// 服务器按出生/状态/销毁事件发布整局快照；HUD 读取 GameState 副本，不另行推算波次或存活数。
void AmultiplayerCombatEncounter::PublishState()
{
	if (auto* State = BoundGameState.Get())
	{
		FmultiplayerCombatEncounterState Snapshot;
		Snapshot.Wave = CurrentWave;
		Snapshot.TotalWaves = FMath::Min(WaveSizes.Num(), 10);
		Snapshot.LivingEnemies = GetLivingEnemyCount();
		Snapshot.bActive = bStarted && !bStopped;
		State->ApplyEncounterState(Snapshot);
	}
}

// 终局取消尚未出生的任务，现存小兵停止决策和技能，不再攻击胜利/失败界面后的玩家。
void AmultiplayerCombatEncounter::StopEncounter()
{
	if (bStopped) return;
	bStopped = true;
	GetWorldTimerManager().ClearTimer(WaveTimer);
	for (auto Weak : Spawners) if (auto* Spawner = Weak.Get()) Spawner->Destroy();
	Spawners.Reset();
	for (auto Weak : Enemies)
		if (auto* Enemy = Weak.Get())
		{
			Enemy->SetCombatSessionReady(false);
			if (auto* AI = Cast<AAIController>(Enemy->GetController()))
			{
				AI->StopMovement();
				if (AI->GetBrainComponent()) AI->GetBrainComponent()->StopLogic(TEXT("Coop round ended"));
			}
		}
	if (bStarted) PublishState();
}

// 退出先停出生和战斗，再解除所有本关卡订阅，不让旧关卡继续发布状态。
void AmultiplayerCombatEncounter::EndPlay(EEndPlayReason::Type Reason)
{
	StopEncounter();
	if (auto* Mode = BoundGameMode.Get()) Mode->OnCombatSessionAccepted.Remove(AcceptedHandle);
	if (auto* State = BoundGameState.Get())
	{
		State->OnGameWon.RemoveDynamic(this, &ThisClass::StopEncounter);
		State->OnGameLost.RemoveDynamic(this, &ThisClass::StopEncounter);
	}
	for (auto Weak : Enemies) if (auto* Enemy = Weak.Get())
	{
		Enemy->OnCombatStateChanged.RemoveDynamic(this, &ThisClass::HandleEnemyChanged);
		Enemy->OnDestroyed.RemoveDynamic(this, &ThisClass::HandleEnemyDestroyed);
	}
	Super::EndPlay(Reason);
}

// 一个即时 GE 同时定义生命和能量加法，最终上限约束仍在 AttributeSet。
UmultiplayerCombatSupplyEffect::UmultiplayerCombatSupplyEffect()
{
	for (bool bHealth : {true, false})
	{
		auto& Modifier = Modifiers.AddDefaulted_GetRef();
		Modifier.Attribute = bHealth ? UmultiplayerCombatAttributes::GetHealthAttribute() : UmultiplayerCombatAttributes::GetEnergyAttribute();
		FSetByCallerFloat Value; Value.DataTag = bHealth ? SupplyHealth.GetTag() : SupplyEnergy.GetTag();
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Value);
	}
}

// 触发器只识别 Pawn 重叠，视觉网格不参与碰撞或权威领取判定。
AmultiplayerCombatSupply::AmultiplayerCombatSupply()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	Trigger = CreateDefaultSubobject<USphereComponent>(TEXT("SupplyTrigger"));
	SetRootComponent(Trigger);
	Trigger->SetSphereRadius(70.f);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("SupplyVisual"));
	Visual->SetupAttachment(Trigger);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Visual->SetRelativeScale3D(FVector(.45f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Mesh.Succeeded()) Visual->SetStaticMesh(Mesh.Object);
	Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("SupplyLabel"));
	Label->SetupAttachment(Trigger);
	Label->SetRelativeLocation(FVector(0,0,45));
	Label->SetRelativeRotation(FRotator(0,90,0));
	Label->SetWorldSize(12.f);
	Label->SetHorizontalAlignment(EHTA_Center);
	Label->SetTextRenderColor(FColor(100,255,120));
	Label->SetText(FText::FromString(TEXT("SUPPLY")));
	Label->SetHiddenInGame(true);
}

// 只有服务器安装领取回调，各端立即按已知消费状态校正表现。
void AmultiplayerCombatSupply::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) Trigger->OnComponentBeginOverlap.AddUniqueDynamic(this, &ThisClass::HandleOverlap);
	OnRep_Consumed();
}
// 只有服务器消费补给；先占用再应用属性效果，避免同步属性回调或重复重叠重复发放。
void AmultiplayerCombatSupply::HandleOverlap(UPrimitiveComponent*, AActor* Other, UPrimitiveComponent*, int32, bool, const FHitResult&)
{
	auto* Player = Cast<AmultiplayerCombatCharacter>(Other);
	if (!HasAuthority() || bConsumed || !Player || !Player->IsPlayerControlled() || !Player->IsCombatReady()) return;
	if (const auto* State = GetWorld()->GetGameState<AmultiplayerCoopGameState>(); State && (State->HasGameWon() || State->HasGameLost())) return;
	if ((HealthAmount <= 0 || Player->GetHealth() >= Player->GetMaxHealth())
		&& (EnergyAmount <= 0 || Player->GetEnergy() >= Player->GetMaxEnergy())) return;
	bConsumed = true;
	OnRep_Consumed();
	ForceNetUpdate();
	auto* ASC = Player->GetAbilitySystemComponent();
	auto Spec = ASC->MakeOutgoingSpec(UmultiplayerCombatSupplyEffect::StaticClass(), 1, ASC->MakeEffectContext());
	Spec.Data->SetSetByCallerMagnitude(SupplyHealth, FMath::Max(0.f, HealthAmount));
	Spec.Data->SetSetByCallerMagnitude(SupplyEnergy, FMath::Max(0.f, EnergyAmount));
	ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}
// 以消费位统一驱动隐藏和碰撞，客户端永远不保留领取触发器。
void AmultiplayerCombatSupply::OnRep_Consumed()
{
	SetActorHiddenInGame(bConsumed);
	Trigger->SetCollisionEnabled(HasAuthority() && !bConsumed ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
}
// 只同步一次性消费结果，补给资源由角色属性的现有复制链路传递。
void AmultiplayerCombatSupply::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCombatSupply, bConsumed);
}
// 解绑服务器重叠回调后交还 Actor 生命周期，不追加任何领取结算。
void AmultiplayerCombatSupply::EndPlay(EEndPlayReason::Type Reason)
{
	Trigger->OnComponentBeginOverlap.RemoveDynamic(this, &ThisClass::HandleOverlap);
	Super::EndPlay(Reason);
}
