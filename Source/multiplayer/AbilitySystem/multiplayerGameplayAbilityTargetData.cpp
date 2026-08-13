// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerGameplayAbilityTargetData.h"

const TCHAR* GetMultiplayerDamageIntentResultName(
	EmultiplayerDamageIntentResult Result)
{
	switch (Result)
	{
	case EmultiplayerDamageIntentResult::Accepted:
		return TEXT("Accepted");
	case EmultiplayerDamageIntentResult::InvalidSchema:
		return TEXT("InvalidSchema");
	case EmultiplayerDamageIntentResult::InvalidShotId:
		return TEXT("InvalidShotId");
	case EmultiplayerDamageIntentResult::Duplicate:
		return TEXT("Duplicate");
	case EmultiplayerDamageIntentResult::StaleSequence:
		return TEXT("StaleSequence");
	case EmultiplayerDamageIntentResult::RateLimited:
		return TEXT("RateLimited");
	case EmultiplayerDamageIntentResult::InvalidTime:
		return TEXT("InvalidTime");
	case EmultiplayerDamageIntentResult::InvalidOrigin:
		return TEXT("InvalidOrigin");
	case EmultiplayerDamageIntentResult::InvalidDirection:
		return TEXT("InvalidDirection");
	case EmultiplayerDamageIntentResult::SourceDead:
		return TEXT("SourceDead");
	case EmultiplayerDamageIntentResult::TargetDataTimeout:
		return TEXT("TargetDataTimeout");
	case EmultiplayerDamageIntentResult::Miss:
		return TEXT("Miss");
	case EmultiplayerDamageIntentResult::InvalidTarget:
		return TEXT("InvalidTarget");
	case EmultiplayerDamageIntentResult::CommitFailed:
		return TEXT("CommitFailed");
	default:
		return TEXT("Unknown");
	}
}

const TCHAR* GetMultiplayerDamageIntentTestMutationName(
	EmultiplayerDamageIntentTestMutation Mutation)
{
	switch (Mutation)
	{
	case EmultiplayerDamageIntentTestMutation::None:
		return TEXT("None");
	case EmultiplayerDamageIntentTestMutation::DuplicateLastShotId:
		return TEXT("DuplicateLastShotId");
	case EmultiplayerDamageIntentTestMutation::ForgedOrigin:
		return TEXT("ForgedOrigin");
	case EmultiplayerDamageIntentTestMutation::OppositeDirection:
		return TEXT("OppositeDirection");
	case EmultiplayerDamageIntentTestMutation::TooOld:
		return TEXT("TooOld");
	case EmultiplayerDamageIntentTestMutation::Future:
		return TEXT("Future");
	case EmultiplayerDamageIntentTestMutation::CleanMiss:
		return TEXT("CleanMiss");
	default:
		return TEXT("Unknown");
	}
}

EmultiplayerDamageIntentResult FmultiplayerDamageIntentGuard::TryConsume(
	uint32 ShotId,
	double ServerNowSeconds,
	uint32 MaxForwardAdvance,
	double MinIntentIntervalSeconds)
{
	if (ShotId == 0)
	{
		return EmultiplayerDamageIntentResult::InvalidShotId;
	}
	if (!FMath::IsFinite(ServerNowSeconds))
	{
		return EmultiplayerDamageIntentResult::InvalidTime;
	}

	if (bHasProcessedShot)
	{
		const uint32 Delta = ShotId - LastProcessedShotId;
		if (Delta == 0)
		{
			return EmultiplayerDamageIntentResult::Duplicate;
		}
		if (Delta > MaxForwardAdvance)
		{
			return EmultiplayerDamageIntentResult::StaleSequence;
		}

		// Consume a valid new sequence number even when it is too fast. The client
		// cannot retry one ShotId after the rate window and sample the server again.
		LastProcessedShotId = ShotId;
		const double Interval = ServerNowSeconds - LastIntentServerTime;
		LastIntentServerTime = ServerNowSeconds;
		if (Interval < MinIntentIntervalSeconds)
		{
			return EmultiplayerDamageIntentResult::RateLimited;
		}
		return EmultiplayerDamageIntentResult::Accepted;
	}

	bHasProcessedShot = true;
	LastProcessedShotId = ShotId;
	LastIntentServerTime = ServerNowSeconds;
	return EmultiplayerDamageIntentResult::Accepted;
}

void FmultiplayerDamageIntentGuard::Reset()
{
	LastProcessedShotId = 0;
	bHasProcessedShot = false;
	LastIntentServerTime = -1.0e30;
}

FString FmultiplayerGameplayAbilityTargetData_DamageIntent::ToString() const
{
	return FString::Printf(
		TEXT("DamageIntent ShotId=%u Origin=%s Direction=%s ClientTime=%.3f"),
		ShotId,
		*FVector(Origin).ToCompactString(),
		*FVector(Direction).ToCompactString(),
		ClientFireServerTimeSeconds);
}

bool FmultiplayerGameplayAbilityTargetData_DamageIntent::NetSerialize(
	FArchive& Ar,
	UPackageMap* Map,
	bool& bOutSuccess)
{
	bool bOriginSuccess = true;
	bool bDirectionSuccess = true;
	Ar.SerializeIntPacked(ShotId);
	Origin.NetSerialize(Ar, Map, bOriginSuccess);
	Direction.NetSerialize(Ar, Map, bDirectionSuccess);
	Ar << ClientFireServerTimeSeconds;

	bOutSuccess = bOriginSuccess && bDirectionSuccess && !Ar.IsError();
	return true;
}

EmultiplayerDamageIntentResult ValidateMultiplayerDamageIntentSchema(
	const FGameplayAbilityTargetDataHandle& Handle,
	const FmultiplayerGameplayAbilityTargetData_DamageIntent*& OutIntent)
{
	OutIntent = nullptr;
	if (Handle.Num() != 1 || Handle.Get(0) == nullptr
		|| Handle.Get(0)->GetScriptStruct()
			!= FmultiplayerGameplayAbilityTargetData_DamageIntent::StaticStruct())
	{
		return EmultiplayerDamageIntentResult::InvalidSchema;
	}

	OutIntent = static_cast<
		const FmultiplayerGameplayAbilityTargetData_DamageIntent*>(Handle.Get(0));
	return OutIntent->ShotId == 0
		? EmultiplayerDamageIntentResult::InvalidShotId
		: EmultiplayerDamageIntentResult::Accepted;
}

EmultiplayerDamageIntentResult ValidateMultiplayerDamageIntentFields(
	const FmultiplayerGameplayAbilityTargetData_DamageIntent& Intent,
	const FmultiplayerDamageIntentServerContext& Context,
	const FmultiplayerDamageIntentValidationConfig& Config)
{
	const FVector ClientOrigin = FVector(Intent.Origin);
	const FVector ClientDirection = FVector(Intent.Direction);
	if (ClientOrigin.ContainsNaN()
		|| Context.ServerEyeOrigin.ContainsNaN()
		|| Config.MaxOriginErrorCm < 0.0f
		|| FVector::DistSquared(ClientOrigin, Context.ServerEyeOrigin)
			> FMath::Square(Config.MaxOriginErrorCm))
	{
		return EmultiplayerDamageIntentResult::InvalidOrigin;
	}
	if (ClientDirection.ContainsNaN()
		|| Context.ServerAimDirection.ContainsNaN()
		|| !FMath::IsNearlyEqual(
			ClientDirection.SizeSquared(),
			1.0f,
			Config.DirectionUnitTolerance)
		|| FVector::DotProduct(
			ClientDirection.GetSafeNormal(),
			Context.ServerAimDirection.GetSafeNormal()) < Config.MinAimDot)
	{
		return EmultiplayerDamageIntentResult::InvalidDirection;
	}
	if (!FMath::IsFinite(Intent.ClientFireServerTimeSeconds)
		|| !FMath::IsFinite(Context.ServerNowSeconds))
	{
		return EmultiplayerDamageIntentResult::InvalidTime;
	}

	const double RequestAge = Context.ServerNowSeconds
		- static_cast<double>(Intent.ClientFireServerTimeSeconds);
	if (RequestAge < -static_cast<double>(Config.MaxFutureLeadSeconds)
		|| RequestAge > static_cast<double>(Config.MaxPastAgeSeconds))
	{
		return EmultiplayerDamageIntentResult::InvalidTime;
	}
	return EmultiplayerDamageIntentResult::Accepted;
}
