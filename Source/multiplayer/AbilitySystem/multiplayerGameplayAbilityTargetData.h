// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Abilities/GameplayAbilityTargetTypes.h"
#include "Engine/NetSerialization.h"
#include "multiplayerGameplayAbilityTargetData.generated.h"

UENUM()
enum class EmultiplayerDamageIntentResult : uint8
{
	Accepted,
	InvalidSchema,
	InvalidShotId,
	Duplicate,
	StaleSequence,
	RateLimited,
	InvalidTime,
	InvalidOrigin,
	InvalidDirection,
	SourceDead,
	TargetDataTimeout,
	Miss,
	InvalidTarget,
	CommitFailed
};

UENUM()
enum class EmultiplayerDamageIntentTestMutation : uint8
{
	None,
	DuplicateLastShotId,
	ForgedOrigin,
	OppositeDirection,
	TooOld,
	Future,
	CleanMiss
};

MULTIPLAYER_API const TCHAR* GetMultiplayerDamageIntentTestMutationName(
	EmultiplayerDamageIntentTestMutation Mutation);

MULTIPLAYER_API const TCHAR* GetMultiplayerDamageIntentResultName(
	EmultiplayerDamageIntentResult Result);

/** Server-owned, non-replicated business idempotency and query-rate guard. */
struct MULTIPLAYER_API FmultiplayerDamageIntentGuard
{
	EmultiplayerDamageIntentResult TryConsume(
		uint32 ShotId,
		double ServerNowSeconds,
		uint32 MaxForwardAdvance = 64,
		double MinIntentIntervalSeconds = 0.05);
	void Reset();

private:
	uint32 LastProcessedShotId = 0;
	bool bHasProcessedShot = false;
	double LastIntentServerTime = -1.0e30;
};

struct MULTIPLAYER_API FmultiplayerDamageIntentValidationConfig
{
	float MaxOriginErrorCm = 150.0f;
	float MinAimDot = 0.819152f; // 35 degrees; intentionally tolerant for M6 current-world validation.
	float MaxPastAgeSeconds = 2.0f;
	float MaxFutureLeadSeconds = 0.25f;
	float DirectionUnitTolerance = 0.05f;
};

struct MULTIPLAYER_API FmultiplayerDamageIntentServerContext
{
	FVector ServerEyeOrigin = FVector::ZeroVector;
	FVector ServerAimDirection = FVector::ForwardVector;
	double ServerNowSeconds = 0.0;
};

/**
 * Minimal client damage request. It intentionally contains no replicated target Actor,
 * hit point, damage number, critical result or GameplayEffect data. The server validates
 * the intent and performs the authoritative scene query in the current server world.
 */
USTRUCT()
struct MULTIPLAYER_API FmultiplayerGameplayAbilityTargetData_DamageIntent
	: public FGameplayAbilityTargetData
{
	GENERATED_BODY()

	UPROPERTY()
	uint32 ShotId = 0;

	UPROPERTY()
	FVector_NetQuantize10 Origin = FVector::ZeroVector;

	UPROPERTY()
	FVector_NetQuantizeNormal Direction = FVector::ForwardVector;

	UPROPERTY()
	float ClientFireServerTimeSeconds = 0.0f;

	virtual UScriptStruct* GetScriptStruct() const override
	{
		return StaticStruct();
	}

	virtual bool HasOrigin() const override { return true; }
	virtual FTransform GetOrigin() const override
	{
		return FTransform(FVector(Direction).Rotation(), FVector(Origin));
	}
	virtual FString ToString() const override;

	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};

MULTIPLAYER_API EmultiplayerDamageIntentResult ValidateMultiplayerDamageIntentSchema(
	const FGameplayAbilityTargetDataHandle& Handle,
	const FmultiplayerGameplayAbilityTargetData_DamageIntent*& OutIntent);

MULTIPLAYER_API EmultiplayerDamageIntentResult ValidateMultiplayerDamageIntentFields(
	const FmultiplayerGameplayAbilityTargetData_DamageIntent& Intent,
	const FmultiplayerDamageIntentServerContext& Context,
	const FmultiplayerDamageIntentValidationConfig& Config =
		FmultiplayerDamageIntentValidationConfig());

template<>
struct TStructOpsTypeTraits<FmultiplayerGameplayAbilityTargetData_DamageIntent>
	: public TStructOpsTypeTraitsBase2<
		FmultiplayerGameplayAbilityTargetData_DamageIntent>
{
	enum
	{
		WithNetSerializer = true
	};
};
