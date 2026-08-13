// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystem/multiplayerGameplayEffectContext.h"

UScriptStruct* FmultiplayerGameplayEffectContext::GetScriptStruct() const
{
	return StaticStruct();
}

FmultiplayerGameplayEffectContext* FmultiplayerGameplayEffectContext::Duplicate() const
{
	FmultiplayerGameplayEffectContext* NewContext =
		new FmultiplayerGameplayEffectContext(*this);
	if (const FHitResult* ExistingHitResult = GetHitResult())
	{
		NewContext->AddHitResult(*ExistingHitResult, true);
	}
	return NewContext;
}

bool FmultiplayerGameplayEffectContext::NetSerialize(
	FArchive& Ar,
	UPackageMap* Map,
	bool& bOutSuccess)
{
	bool bParentSuccess = false;
	FGameplayEffectContext::NetSerialize(Ar, Map, bParentSuccess);

	uint8 PackedFlags = bIsCriticalHit ? 1 : 0;
	Ar.SerializeBits(&PackedFlags, 1);
	if (Ar.IsLoading())
	{
		bIsCriticalHit = (PackedFlags & 1) != 0;
	}

	uint8 SerializedHitType = static_cast<uint8>(HitType);
	Ar.SerializeBits(&SerializedHitType, 2);
	if (Ar.IsLoading())
	{
		HitType = static_cast<EmultiplayerHitType>(SerializedHitType);
	}

	bool bImpulseSuccess = false;
	ImpactImpulse.NetSerialize(Ar, Map, bImpulseSuccess);
	bOutSuccess = bParentSuccess && bImpulseSuccess;
	return bOutSuccess;
}
