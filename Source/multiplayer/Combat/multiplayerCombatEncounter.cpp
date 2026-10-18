// Copyright Epic Games, Inc. All Rights Reserved.
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

int32 AmultiplayerCombatEncounter::GetLivingEnemyCount() const
{
	int32 Count = 0;
	for (auto Weak : Enemies) if (const auto* Enemy = Weak.Get(); Enemy && Enemy->IsAlive()) ++Count;
	return Count;
}

// 固定间隔施压，但不超过当前人数上限；旧波尚存时等待，不在同一帧补齐多个波次。
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

void AmultiplayerCombatEncounter::RegisterEnemy(AmultiplayerCombatEnemy* Enemy)
{
	if (bStopped || !IsValid(Enemy)) return;
	Enemies.AddUnique(Enemy);
	Enemy->OnCombatStateChanged.AddUniqueDynamic(this, &ThisClass::HandleEnemyChanged);
	Enemy->OnDestroyed.AddUniqueDynamic(this, &ThisClass::HandleEnemyDestroyed);
	PublishState();
}
void AmultiplayerCombatEncounter::HandleEnemyChanged() { PublishState(); }
void AmultiplayerCombatEncounter::HandleEnemyDestroyed(AActor* Actor)
{
	Enemies.RemoveAll([Actor](auto Weak) { return !Weak.IsValid() || Weak.Get() == Actor; });
	PublishState();
}
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
}

void AmultiplayerCombatSupply::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority()) Trigger->OnComponentBeginOverlap.AddUniqueDynamic(this, &ThisClass::HandleOverlap);
	OnRep_Consumed();
}
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
void AmultiplayerCombatSupply::OnRep_Consumed()
{
	SetActorHiddenInGame(bConsumed);
	Trigger->SetCollisionEnabled(HasAuthority() && !bConsumed ? ECollisionEnabled::QueryOnly : ECollisionEnabled::NoCollision);
}
void AmultiplayerCombatSupply::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerCombatSupply, bConsumed);
}
void AmultiplayerCombatSupply::EndPlay(EEndPlayReason::Type Reason)
{
	Trigger->OnComponentBeginOverlap.RemoveDynamic(this, &ThisClass::HandleOverlap);
	Super::EndPlay(Reason);
}
