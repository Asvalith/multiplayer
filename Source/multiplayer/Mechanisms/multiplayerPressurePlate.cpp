// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mechanisms/multiplayerPressurePlate.h"

#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Core/multiplayerCoopGameState.h"
#include "Core/multiplayerGameplayConfig.h"
#include "Mechanisms/multiplayerMeshMovement.h"
#include "Mechanisms/multiplayerPlayerOccupancyComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

AmultiplayerPressurePlate::AmultiplayerPressurePlate()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	bReplicates = true;
	SetReplicateMovement(false);
	SetNetUpdateFrequency(5.0f);
	NetDormancy = DORM_DormantAll;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	SceneRoot->SetMobility(EComponentMobility::Movable);

	PlateMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlateMesh"));
	PlateMesh->SetupAttachment(SceneRoot);
	PlateMesh->SetMobility(EComponentMobility::Movable);
	PlateMesh->SetCollisionProfileName(TEXT("BlockAll"));
	PlateMesh->SetGenerateOverlapEvents(false);
	PlateMesh->SetRelativeScale3D(FVector(1.5f, 1.5f, 0.1f));

	ActivationTrigger = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationTrigger"));
	ActivationTrigger->SetupAttachment(SceneRoot);
	ActivationTrigger->bEditableWhenInherited = true;
	ActivationTrigger->SetRelativeLocation(FVector(0.0f, 0.0f, 60.0f));
	ActivationTrigger->SetBoxExtent(FVector(150.0f, 150.0f, 60.0f));
	ActivationTrigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationTrigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationTrigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	PlayerOccupancy = CreateDefaultSubobject<UmultiplayerPlayerOccupancyComponent>(TEXT("PlayerOccupancy"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		PlateMesh->SetStaticMesh(CubeMesh.Object);
	}
}

void AmultiplayerPressurePlate::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority())
	{
		// 初始速度也是复制数据，不能在休眠期间静默改写。
		FlushNetDormancy();
		RuntimePressMoveSpeed = FmultiplayerGameplayConfig::Get(this).PlateMoveSpeed;
	}

	ReleasedRelativeLocation = PlateMesh->GetRelativeLocation();
	PlayerOccupancy->OnOccupancyChanged.AddUniqueDynamic(
		this, &AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->BindTrigger(ActivationTrigger, bRequirePlayerControlledCharacter);
	PlateMesh->SetRelativeLocation(GetMeshTargetLocation());
	SetActorTickEnabled(false);

	if (!HasAuthority())
	{
		return;
	}

	if (bRequireObjectiveComplete)
	{
		CoopGameState = GetWorld()->GetGameState<AmultiplayerCoopGameState>();
		if (CoopGameState != nullptr)
		{
			CoopGameState->OnObjectiveProgressChanged.AddUniqueDynamic(
				this, &AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
		}
	}
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 先取消本 Actor 的监听，再清空人数，避免清理时触发自身规则。
	PlayerOccupancy->OnOccupancyChanged.RemoveDynamic(
		this, &AmultiplayerPressurePlate::HandleOccupancyChanged);
	PlayerOccupancy->UnbindTrigger();

	if (CoopGameState != nullptr)
	{
		CoopGameState->OnObjectiveProgressChanged.RemoveDynamic(
			this, &AmultiplayerPressurePlate::HandleObjectiveProgressChanged);
	}
	Super::EndPlay(EndPlayReason);
}

void AmultiplayerPressurePlate::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (MultiplayerMeshMovement::MoveTo(*PlateMesh, GetMeshTargetLocation(), DeltaSeconds,
		RuntimePressMoveSpeed, 0.25f, MultiplayerMeshMovement::ESpace::Relative))
	{
		SetActorTickEnabled(false);
	}
}

void AmultiplayerPressurePlate::GetLifetimeReplicatedProps(
	TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AmultiplayerPressurePlate, bPlateActive);
	DOREPLIFETIME_CONDITION(AmultiplayerPressurePlate, RuntimePressMoveSpeed, COND_InitialOnly);
}

void AmultiplayerPressurePlate::GetOccupyingCharacters(
	TArray<ACharacter*>& OutCharacters) const
{
	PlayerOccupancy->GetOccupyingCharacters(OutCharacters);
}

/** 先更新本板开关，再通知上层占用改变，使监听者读取到相互一致的最新状态。 */
void AmultiplayerPressurePlate::HandleOccupancyChanged(int32 PlayerCount)
{
	EvaluatePlateState();
	if (HasAuthority())
	{
		// 激活布尔值不变时也必须通知门重新统计不同玩家。
		OnPlateOccupancyChanged.Broadcast(this, PlayerCount);
	}
}

void AmultiplayerPressurePlate::HandleObjectiveProgressChanged(
	int32 ActivatedKeys, int32 RequiredKeys)
{
	EvaluatePlateState();
}

void AmultiplayerPressurePlate::EvaluatePlateState()
{
	if (!HasAuthority())
	{
		return;
	}

	const bool bObjectiveReady = !bRequireObjectiveComplete
		|| (CoopGameState != nullptr && CoopGameState->IsObjectiveComplete());
	const bool bNewPlateActive = (bLatchOnceActivated && bPlateActive)
		|| (bObjectiveReady && PlayerOccupancy->GetPlayerCount() > 0);

	if (bPlateActive == bNewPlateActive)
	{
		return;
	}

	// 先刷新休眠再写属性，不能只依赖赋值后的 ForceNetUpdate。
	FlushNetDormancy();
	bPlateActive = bNewPlateActive;
	OnRep_PlateActive();
	ForceNetUpdate();
}

void AmultiplayerPressurePlate::OnRep_PlateActive()
{
	SetActorTickEnabled(true);
	OnPlateActiveChanged.Broadcast(this, bPlateActive);
	ReceivePlateVisualStateChanged(bPlateActive);
}

FVector AmultiplayerPressurePlate::GetMeshTargetLocation() const
{
	return ReleasedRelativeLocation + (bPlateActive ? PressedOffset : FVector::ZeroVector);
}
