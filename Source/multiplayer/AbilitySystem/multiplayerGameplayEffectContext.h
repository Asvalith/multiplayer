// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayEffectTypes.h"
#include "multiplayerGameplayEffectContext.generated.h"

UENUM(BlueprintType)
enum class EmultiplayerHitType : uint8
{
	Direct,
	Critical,
	Blocked
};

/** Project-specific network payload carried by damage GameplayEffects. */
USTRUCT()
struct MULTIPLAYER_API FmultiplayerGameplayEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

	virtual UScriptStruct* GetScriptStruct() const override;
	virtual FmultiplayerGameplayEffectContext* Duplicate() const override;
	virtual bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess) override;

	bool IsCriticalHit() const { return bIsCriticalHit; }
	void SetCriticalHit(bool bValue) { bIsCriticalHit = bValue; }

	EmultiplayerHitType GetHitType() const { return HitType; }
	void SetHitType(EmultiplayerHitType InHitType) { HitType = InHitType; }

	const FVector_NetQuantize10& GetImpactImpulse() const { return ImpactImpulse; }
	void SetImpactImpulse(const FVector& InImpulse) { ImpactImpulse = InImpulse; }

private:
	UPROPERTY()
	bool bIsCriticalHit = false;

	UPROPERTY()
	EmultiplayerHitType HitType = EmultiplayerHitType::Direct;

	UPROPERTY()
	FVector_NetQuantize10 ImpactImpulse = FVector::ZeroVector;
};

template<>
struct TStructOpsTypeTraits<FmultiplayerGameplayEffectContext>
	: public TStructOpsTypeTraitsBase2<FmultiplayerGameplayEffectContext>
{
	enum
	{
		WithCopy = true,
		WithNetSerializer = true
	};
};
