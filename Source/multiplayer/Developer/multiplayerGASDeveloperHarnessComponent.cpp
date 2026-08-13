// Copyright Epic Games, Inc. All Rights Reserved.

#include "Developer/multiplayerGASDeveloperHarnessComponent.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAbilitySystemComponent.h"
#include "AbilitySystem/multiplayerAttributeSet.h"
#include "AbilitySystem/multiplayerGameplayEffects.h"
#include "AbilitySystem/multiplayerGameplayTags.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Player/multiplayerGASPlayerState.h"
#include "UI/multiplayerGASCuePresenterComponent.h"
#include "multiplayer.h"
#include "multiplayerGASTargetDummy.h"

namespace
{
	bool bGASAutomationStarted = false;

	bool HasCommandLineFlag(const TCHAR* Flag)
	{
		return FParse::Param(FCommandLine::Get(), Flag);
	}
}

UmultiplayerGASDeveloperHarnessComponent::UmultiplayerGASDeveloperHarnessComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	SetIsReplicatedByDefault(true);
#else
	SetIsReplicatedByDefault(false);
#endif
}
void UmultiplayerGASDeveloperHarnessComponent::OnAbilitySystemReady(
	UmultiplayerAbilitySystemComponent* InAbilitySystemComponent)
{
	AbilitySystemComponent = InAbilitySystemComponent;
	TryStartGASAutomation();
}

void UmultiplayerGASDeveloperHarnessComponent::BindDeveloperInput(
	UInputComponent* PlayerInputComponent)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (PlayerInputComponent == nullptr
		|| !HasCommandLineFlag(TEXT("GASDeveloperControls")))
	{
		return;
	}

	PlayerInputComponent->BindKey(
		EKeys::Seven,
		IE_Pressed,
		this,
		&UmultiplayerGASDeveloperHarnessComponent::RequestBaselineEnemyTarget);
	PlayerInputComponent->BindKey(
		EKeys::Eight,
		IE_Pressed,
		this,
		&UmultiplayerGASDeveloperHarnessComponent::RequestBaselineEnemyDamage);
	PlayerInputComponent->BindKey(
		EKeys::Nine,
		IE_Pressed,
		this,
		&UmultiplayerGASDeveloperHarnessComponent::RequestArmNextImmunityPredictionRejection);
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::EndPlay(
	const EEndPlayReason::Type EndPlayReason)
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(GASAutomationTimer);
	}
	AbilitySystemComponent = nullptr;
	BaselineEnemyTarget = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UmultiplayerGASDeveloperHarnessComponent::RequestBaselineEnemyTarget()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	ServerRequestBaselineEnemyTarget();
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::RequestBaselineEnemyDamage()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	ServerRequestBaselineEnemyDamage();
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::RequestArmNextImmunityPredictionRejection()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (!HasCommandLineFlag(TEXT("GASM6Lab")))
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M6_REJECT Phase=ArmIgnored Reason=MissingGASM6Lab Actor=%s"),
			*GetNameSafe(GetOwner()));
		return;
	}

	if (AbilitySystemComponent != nullptr)
	{
		++GASM6AutomationTrialId;
		AbilitySystemComponent->ServerArmNextImmunityPredictionRejection(
			GASM6AutomationTrialId);
	}
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::TriggerAbilityInput(
	const FGameplayTag& InputTag)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	const AmultiplayerGASPlayerState* GASPlayerState = OwnerPawn != nullptr
		? OwnerPawn->GetPlayerState<AmultiplayerGASPlayerState>()
		: nullptr;
	if (GASPlayerState != nullptr && GASPlayerState->IsDead())
	{
		return;
	}

	if (AbilitySystemComponent != nullptr)
	{
		AbilitySystemComponent->AbilityInputTagPressed(InputTag);
		AbilitySystemComponent->AbilityInputTagReleased(InputTag);
	}
#endif
}

bool UmultiplayerGASDeveloperHarnessComponent::ServerRequestBaselineEnemyDamage_Validate()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	return IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed();
#else
	return false;
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::ServerRequestBaselineEnemyDamage_Implementation()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (!IsValid(BaselineEnemyTarget))
	{
		return;
	}

	UAbilitySystemComponent* EnemyASC = BaselineEnemyTarget->GetAbilitySystemComponent();
	if (EnemyASC == nullptr || AbilitySystemComponent == nullptr)
	{
		return;
	}

	FGameplayEffectContextHandle Context = EnemyASC->MakeEffectContext();
	Context.AddSourceObject(BaselineEnemyTarget);
	FGameplayEffectSpecHandle DamageSpec = EnemyASC->MakeOutgoingSpec(
		UmultiplayerDamageEffect::StaticClass(),
		1.0f,
		Context);
	if (DamageSpec.IsValid())
	{
		DamageSpec.Data->SetSetByCallerMagnitude(
			MultiplayerGameplayTags::Data_Damage,
			25.0f);
		EnemyASC->ApplyGameplayEffectSpecToTarget(
			*DamageSpec.Data.Get(),
			AbilitySystemComponent);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_ENEMY_DAMAGE Source=%s Target=%s Amount=25"),
			*GetNameSafe(BaselineEnemyTarget),
			*GetNameSafe(GetOwner()));
	}
#endif
}

bool UmultiplayerGASDeveloperHarnessComponent::ServerRequestBaselineEnemyTarget_Validate()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	return IsValid(GetOwner()) && !GetOwner()->IsActorBeingDestroyed();
#else
	return false;
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::ServerRequestBaselineEnemyTarget_Implementation()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	UWorld* World = GetWorld();
	AActor* OwnerActor = GetOwner();
	if (World == nullptr || OwnerActor == nullptr)
	{
		return;
	}

	const FVector SpawnLocation =
		OwnerActor->GetActorLocation() + OwnerActor->GetActorForwardVector() * 300.0f;
	if (IsValid(BaselineEnemyTarget))
	{
		BaselineEnemyTarget->ResetForBaseline(SpawnLocation);
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Owner = OwnerActor;
	SpawnParameters.Instigator = Cast<APawn>(OwnerActor);
	SpawnParameters.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	BaselineEnemyTarget = World->SpawnActor<AmultiplayerGASTargetDummy>(
		AmultiplayerGASTargetDummy::StaticClass(),
		SpawnLocation,
		OwnerActor->GetActorRotation(),
		SpawnParameters);
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::TryStartGASAutomation()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const APawn* OwnerPawn = Cast<APawn>(GetOwner());
	if (bGASAutomationStarted
		|| OwnerPawn == nullptr
		|| !OwnerPawn->IsLocallyControlled()
		|| (!HasCommandLineFlag(TEXT("GASM5Auto"))
			&& !HasCommandLineFlag(TEXT("GASM6Auto"))
			&& !HasCommandLineFlag(TEXT("GASM6IntentAuto"))))
	{
		return;
	}

	bGASAutomationStarted = true;
	GASM5AutomationStep = 0;
	GASM6AutomationStep = 0;
	GASM6IntentAutomationStep = 0;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_AUTO Phase=Started Stage=%s Actor=%s Role=%s"),
		HasCommandLineFlag(TEXT("GASM6IntentAuto"))
			? TEXT("M6Intent")
			: (HasCommandLineFlag(TEXT("GASM6Auto")) ? TEXT("M6") : TEXT("M5")),
		*GetNameSafe(GetOwner()),
		*UEnum::GetValueAsString(GetOwner()->GetLocalRole()));
	ScheduleNextGASM5AutomationStep(2.0f);
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::ScheduleNextGASM5AutomationStep(
	float DelaySeconds)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			GASAutomationTimer,
			this,
			&UmultiplayerGASDeveloperHarnessComponent::RunNextGASM5AutomationStep,
			FMath::Max(DelaySeconds, 0.05f),
			false);
	}
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::RunNextGASM5AutomationStep()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (HasCommandLineFlag(TEXT("GASM6Auto")))
	{
		RunNextGASM6AutomationStep();
		return;
	}
	if (HasCommandLineFlag(TEXT("GASM6IntentAuto")))
	{
		RunNextGASM6IntentAutomationStep();
		return;
	}

	const int32 ExecutedStep = GASM5AutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M5_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetNameSafe(GetOwner()));

	switch (ExecutedStep)
	{
	case 0:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(2.0f);
		break;
	case 1:
	case 2:
	case 3:
		if (!AimGASM5AutomationAtTarget())
		{
			--GASM5AutomationStep;
			ScheduleNextGASM5AutomationStep(0.5f);
			break;
		}
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Damage);
		ScheduleNextGASM5AutomationStep(ExecutedStep == 3 ? 2.0f : 1.3f);
		break;
	case 4:
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Immunity);
		ScheduleNextGASM5AutomationStep(6.0f);
		break;
	case 5:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(0.8f);
		break;
	case 6:
		AimGASM5AutomationAtTarget();
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Damage);
		ScheduleNextGASM5AutomationStep(8.8f);
		break;
	case 7:
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M5_AUTO Phase=VulnerabilityExpiryCheckpoint"));
		AimGASM5AutomationAtTarget();
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Damage);
		ScheduleNextGASM5AutomationStep(0.6f);
		break;
	case 8:
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(1.0f);
		break;
	case 9:
		RequestBaselineEnemyDamage();
		ScheduleNextGASM5AutomationStep(0.8f);
		break;
	case 10:
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Heal);
		ScheduleNextGASM5AutomationStep(3.5f);
		break;
	case 11:
	case 12:
	case 13:
		RequestBaselineEnemyDamage();
		ScheduleNextGASM5AutomationStep(0.5f);
		break;
	case 14:
		RequestBaselineEnemyDamage();
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M5_AUTO Phase=SequenceComplete AwaitingDeathRespawn=true"));
		break;
	default:
		break;
	}
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::RunNextGASM6AutomationStep()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const int32 ExecutedStep = GASM6AutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetNameSafe(GetOwner()));

	switch (ExecutedStep)
	{
	case 0:
		RequestArmNextImmunityPredictionRejection();
		LogGASM6Snapshot(TEXT("Initial"));
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	case 1:
		if (AbilitySystemComponent == nullptr
			|| !AbilitySystemComponent->HasPredictionRejectLabArmResult(
				GASM6AutomationTrialId))
		{
			if (IsGASM6AutomationTimedOut())
			{
				FailGASM6Automation(TEXT("ArmConfirmationTimeout"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		if (!AbilitySystemComponent->WasPredictionRejectLabArmSuccessful(
			GASM6AutomationTrialId))
		{
			FailGASM6Automation(TEXT("ServerRefusedToArm"));
			break;
		}
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=ForcedRejectInput TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Immunity);
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	case 2:
	{
		const float Energy = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute())
			: -1.0f;
		const bool bRollbackSettled = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetLastPredictionLabRejectedKey() != 0
			&& FMath::IsNearlyEqual(Energy, 100.0f, 0.1f)
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRollbackSettled)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RollbackTimeout"));
				FailGASM6Automation(TEXT("RollbackDidNotSettle"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("PostRejectCheckpoint"));
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=RecoveryInput TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Immunity);
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	}
	case 3:
	{
		const int16 RejectedKey = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabRejectedKey()
			: 0;
		const int16 CaughtUpKey = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetLastPredictionLabCaughtUpKey()
			: 0;
		const float Energy = AbilitySystemComponent != nullptr
			? AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute())
			: -1.0f;
		const bool bRecoveryAccepted = AbilitySystemComponent != nullptr
			&& CaughtUpKey != 0
			&& CaughtUpKey != RejectedKey
			&& FMath::IsNearlyEqual(Energy, 70.0f, 0.1f)
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 1
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 1
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRecoveryAccepted)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RecoveryTimeout"));
				FailGASM6Automation(TEXT("RecoveryDidNotConverge"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("PostRecoveryCheckpoint"));
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		break;
	}
	case 4:
	{
		const bool bRecoveryExpired = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Immunity) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::State_Immune) == 0
			&& AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending) == 0;
		if (!bRecoveryExpired)
		{
			if (IsGASM6AutomationTimedOut())
			{
				LogGASM6Snapshot(TEXT("RecoveryExpiryTimeout"));
				FailGASM6Automation(TEXT("RecoveryDidNotExpire"));
				break;
			}
			--GASM6AutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			break;
		}
		LogGASM6Snapshot(TEXT("RecoveryExpiryCheckpoint"));
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_AUTO Phase=SequenceComplete Result=Pass TrialId=%u Ability=Immunity"),
			GASM6AutomationTrialId);
		break;
	}
	default:
		break;
	}
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::RunNextGASM6IntentAutomationStep()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const int32 ExecutedStep = GASM6IntentAutomationStep++;
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_INTENT_AUTO Phase=Step Step=%d Actor=%s"),
		ExecutedStep,
		*GetNameSafe(GetOwner()));

	auto Submit = [this](
		EmultiplayerDamageIntentTestMutation Mutation,
		EmultiplayerDamageIntentResult ExpectedResult)
	{
		if (AbilitySystemComponent == nullptr || !AimGASM5AutomationAtTarget())
		{
			--GASM6IntentAutomationStep;
			ScheduleNextGASM5AutomationStep(0.25f);
			return false;
		}
		GASM6IntentResultSerialBefore =
			AbilitySystemComponent->GetDamageIntentResultSerial();
		AbilitySystemComponent->SetDamageIntentLabMutation(Mutation);
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT_AUTO Phase=Input Mutation=%s Expected=%s ResultSerialBefore=%u Energy=%.1f"),
			GetMultiplayerDamageIntentTestMutationName(Mutation),
			GetMultiplayerDamageIntentResultName(ExpectedResult),
			GASM6IntentResultSerialBefore,
			AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute()));
		TriggerAbilityInput(MultiplayerGameplayTags::InputTag_Ability_Damage);
		GASM6AutomationDeadlineSeconds = GetWorld()->GetTimeSeconds() + 20.0f;
		ScheduleNextGASM5AutomationStep(0.1f);
		return true;
	};

	auto WaitForResult = [this](
		EmultiplayerDamageIntentResult ExpectedResult,
		float ExpectedEnergy,
		const TCHAR* Checkpoint)
	{
		const bool bResultArrived = AbilitySystemComponent != nullptr
			&& AbilitySystemComponent->GetDamageIntentResultSerial()
				> GASM6IntentResultSerialBefore;
		const bool bResultMatches = bResultArrived
			&& AbilitySystemComponent->GetLastDamageIntentResult() == ExpectedResult;
		const bool bStateSettled = AbilitySystemComponent != nullptr
			&& FMath::IsNearlyEqual(
				AbilitySystemComponent->GetNumericAttribute(
					UmultiplayerAttributeSet::GetEnergyAttribute()),
				ExpectedEnergy,
				0.1f)
			&& (ExpectedResult == EmultiplayerDamageIntentResult::Accepted
				|| AbilitySystemComponent->GetTagCount(
					MultiplayerGameplayTags::Cooldown_Ability_Damage) == 0);
		if (!bResultMatches || !bStateSettled)
		{
			if (IsGASM6AutomationTimedOut())
			{
				UE_LOG(
					LogMultiplayerGAS,
					Error,
					TEXT("GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Fail Reason=ResultTimeout Expected=%s Actual=%s SerialBefore=%u SerialNow=%u Energy=%.1f Cooldown=%d"),
					GetMultiplayerDamageIntentResultName(ExpectedResult),
					AbilitySystemComponent != nullptr
						? GetMultiplayerDamageIntentResultName(
							AbilitySystemComponent->GetLastDamageIntentResult())
						: TEXT("NoASC"),
					GASM6IntentResultSerialBefore,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetDamageIntentResultSerial()
						: 0,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetNumericAttribute(
							UmultiplayerAttributeSet::GetEnergyAttribute())
						: -1.0f,
					AbilitySystemComponent != nullptr
						? AbilitySystemComponent->GetTagCount(
							MultiplayerGameplayTags::Cooldown_Ability_Damage)
						: -1);
				GASM6IntentAutomationStep = 1000;
				return false;
			}
			--GASM6IntentAutomationStep;
			ScheduleNextGASM5AutomationStep(0.1f);
			return false;
		}
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT_AUTO Phase=Checkpoint Name=%s ShotId=%u Result=%s Energy=%.1f Cooldown=%d"),
			Checkpoint,
			AbilitySystemComponent->GetLastDamageIntentResultShotId(),
			GetMultiplayerDamageIntentResultName(
				AbilitySystemComponent->GetLastDamageIntentResult()),
			AbilitySystemComponent->GetNumericAttribute(
				UmultiplayerAttributeSet::GetEnergyAttribute()),
			AbilitySystemComponent->GetTagCount(
				MultiplayerGameplayTags::Cooldown_Ability_Damage));
		return true;
	};

	struct FIntentCase
	{
		EmultiplayerDamageIntentTestMutation Mutation;
		EmultiplayerDamageIntentResult ExpectedResult;
		float ExpectedEnergy;
		const TCHAR* Checkpoint;
	};

	static const FIntentCase Cases[] =
	{
		{ EmultiplayerDamageIntentTestMutation::None, EmultiplayerDamageIntentResult::Accepted, 90.0f, TEXT("ValidAccepted") },
		{ EmultiplayerDamageIntentTestMutation::DuplicateLastShotId, EmultiplayerDamageIntentResult::Duplicate, 90.0f, TEXT("DuplicateRejected") },
		{ EmultiplayerDamageIntentTestMutation::ForgedOrigin, EmultiplayerDamageIntentResult::InvalidOrigin, 90.0f, TEXT("OriginRejected") },
		{ EmultiplayerDamageIntentTestMutation::OppositeDirection, EmultiplayerDamageIntentResult::InvalidDirection, 90.0f, TEXT("DirectionRejected") },
		{ EmultiplayerDamageIntentTestMutation::TooOld, EmultiplayerDamageIntentResult::InvalidTime, 90.0f, TEXT("StaleTimeRejected") },
		{ EmultiplayerDamageIntentTestMutation::Future, EmultiplayerDamageIntentResult::InvalidTime, 90.0f, TEXT("FutureTimeRejected") },
		{ EmultiplayerDamageIntentTestMutation::CleanMiss, EmultiplayerDamageIntentResult::Miss, 90.0f, TEXT("MissRejected") },
		{ EmultiplayerDamageIntentTestMutation::None, EmultiplayerDamageIntentResult::Accepted, 80.0f, TEXT("RecoveryAccepted") }
	};

	if (ExecutedStep == 0)
	{
		RequestBaselineEnemyTarget();
		ScheduleNextGASM5AutomationStep(1.5f);
		return;
	}

	const int32 CaseIndex = (ExecutedStep - 1) / 2;
	if (CaseIndex < 0 || CaseIndex >= UE_ARRAY_COUNT(Cases))
	{
		return;
	}

	const FIntentCase& IntentCase = Cases[CaseIndex];
	if ((ExecutedStep & 1) != 0)
	{
		Submit(IntentCase.Mutation, IntentCase.ExpectedResult);
		return;
	}

	const bool bWaitForInitialCooldown =
		CaseIndex == 0
		&& AbilitySystemComponent != nullptr
		&& AbilitySystemComponent->GetTagCount(
			MultiplayerGameplayTags::Cooldown_Ability_Damage) > 0;
	if (bWaitForInitialCooldown)
	{
		--GASM6IntentAutomationStep;
		ScheduleNextGASM5AutomationStep(0.1f);
		return;
	}

	if (!WaitForResult(
		IntentCase.ExpectedResult,
		IntentCase.ExpectedEnergy,
		IntentCase.Checkpoint))
	{
		return;
	}

	if (CaseIndex == UE_ARRAY_COUNT(Cases) - 1)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Display,
			TEXT("GAS_M6_INTENT_AUTO Phase=SequenceComplete Result=Pass"));
		return;
	}

	ScheduleNextGASM5AutomationStep(0.05f);
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::FailGASM6Automation(
	const TCHAR* Reason)
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(GASAutomationTimer);
	}
	UE_LOG(
		LogMultiplayerGAS,
		Error,
		TEXT("GAS_M6_AUTO Phase=SequenceComplete Result=Fail TrialId=%u Ability=Immunity Reason=%s"),
		GASM6AutomationTrialId,
		Reason);
#endif
}

bool UmultiplayerGASDeveloperHarnessComponent::IsGASM6AutomationTimedOut() const
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	return GetWorld() == nullptr
		|| GetWorld()->GetTimeSeconds() >= GASM6AutomationDeadlineSeconds;
#else
	return true;
#endif
}

void UmultiplayerGASDeveloperHarnessComponent::LogGASM6Snapshot(
	const TCHAR* Phase) const
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const UAbilitySystemComponent* ASC = AbilitySystemComponent;
	if (ASC == nullptr)
	{
		return;
	}

	auto CountEffectsWithEffectTag = [ASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return ASC->GetActiveEffectsWithAllTags(Tags).Num();
	};
	auto CountEffectsWithOwningTag = [ASC](const FGameplayTag& Tag)
	{
		FGameplayTagContainer Tags;
		Tags.AddTag(Tag);
		return ASC->GetActiveEffects(
			FGameplayEffectQuery::MakeQuery_MatchAllOwningTags(Tags)).Num();
	};
	const int32 PendingCueCount = ASC->GetTagCount(
		MultiplayerGameplayTags::GameplayCue_Coop_Prediction_Pending);
	const UmultiplayerGASCuePresenterComponent* CuePresenter =
		GetOwner() != nullptr
			? GetOwner()->FindComponentByClass<UmultiplayerGASCuePresenterComponent>()
			: nullptr;
	const int32 PendingVisual = CuePresenter != nullptr
		? (CuePresenter->IsPredictionPendingActive() ? 1 : 0)
		: -1;

	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M6_SNAPSHOT Phase=%s TrialId=%u RejectedKey=%d CaughtUpKey=%d EnergyBase=%.1f EnergyCurrent=%.1f CostGECount=%d CooldownGECount=%d ImmunityCooldown=%d PersistentGECount=%d ImmuneCount=%d PendingGECount=%d PendingCue=%d PendingVisual=%d"),
		Phase,
		GASM6AutomationTrialId,
		static_cast<int32>(AbilitySystemComponent->GetLastPredictionLabRejectedKey()),
		static_cast<int32>(AbilitySystemComponent->GetLastPredictionLabCaughtUpKey()),
		ASC->GetNumericAttributeBase(UmultiplayerAttributeSet::GetEnergyAttribute()),
		ASC->GetNumericAttribute(UmultiplayerAttributeSet::GetEnergyAttribute()),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Cost_Immunity),
		CountEffectsWithOwningTag(MultiplayerGameplayTags::Cooldown_Ability_Immunity),
		ASC->GetTagCount(MultiplayerGameplayTags::Cooldown_Ability_Immunity),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Positive_Immunity),
		ASC->GetTagCount(MultiplayerGameplayTags::State_Immune),
		CountEffectsWithEffectTag(MultiplayerGameplayTags::Effect_Debug_PredictionPending),
		PendingCueCount,
		PendingVisual);
#endif
}

bool UmultiplayerGASDeveloperHarnessComponent::AimGASM5AutomationAtTarget()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	APawn* OwnerPawn = Cast<APawn>(GetOwner());
	APlayerController* PlayerController = OwnerPawn != nullptr
		? Cast<APlayerController>(OwnerPawn->GetController())
		: nullptr;
	UWorld* World = GetWorld();
	if (PlayerController == nullptr || World == nullptr)
	{
		return false;
	}

	AmultiplayerGASTargetDummy* ClosestTarget = nullptr;
	float ClosestDistanceSquared = TNumericLimits<float>::Max();
	for (TActorIterator<AmultiplayerGASTargetDummy> It(World); It; ++It)
	{
		AmultiplayerGASTargetDummy* Candidate = *It;
		if (!IsValid(Candidate) || Candidate->GetHealth() <= 0.0f)
		{
			continue;
		}

		const float DistanceSquared = FVector::DistSquared(
			GetOwner()->GetActorLocation(),
			Candidate->GetActorLocation());
		if (DistanceSquared < ClosestDistanceSquared)
		{
			ClosestTarget = Candidate;
			ClosestDistanceSquared = DistanceSquared;
		}
	}

	if (ClosestTarget == nullptr)
	{
		UE_LOG(
			LogMultiplayerGAS,
			Warning,
			TEXT("GAS_M5_AUTO Phase=AimFailed Reason=NoLiveTarget"));
		return false;
	}

	FVector ViewLocation;
	FRotator ViewRotation;
	PlayerController->GetPlayerViewPoint(ViewLocation, ViewRotation);
	const FVector AimLocation =
		ClosestTarget->GetActorLocation() + FVector(0.0f, 0.0f, 50.0f);
	const FRotator AimRotation = (AimLocation - ViewLocation).Rotation();
	PlayerController->SetControlRotation(AimRotation);
	UE_LOG(
		LogMultiplayerGAS,
		Display,
		TEXT("GAS_M5_AUTO Phase=Aimed Target=%s Distance=%.1f Rotation=%s"),
		*ClosestTarget->GetName(),
		FMath::Sqrt(ClosestDistanceSquared),
		*AimRotation.ToCompactString());
	return true;
#else
	return false;
#endif
}
