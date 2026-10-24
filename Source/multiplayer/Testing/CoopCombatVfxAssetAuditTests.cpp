// Copyright Epic Games, Inc. All Rights Reserved.

// 只读审计正式特效配置、Cascade 原始轴／分布及 Niagara 公开参数。
// 输出用于校准的证据，不修改第三方素材，也不把配置检查通过等同于渲染观感正确。

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Combat/multiplayerCombatConfig.h"
#include "Materials/MaterialInterface.h"
#include "NiagaraParameterStore.h"
#include "NiagaraSystem.h"
#include "Particles/Acceleration/ParticleModuleAccelerationConstant.h"
#include "Particles/Lifetime/ParticleModuleLifetime.h"
#include "Particles/Location/ParticleModuleLocation.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleModuleRequired.h"
#include "Particles/ParticleSystem.h"
#include "Particles/Size/ParticleModuleSize.h"
#include "Particles/TypeData/ParticleModuleTypeDataBase.h"
#include "Particles/Velocity/ParticleModuleVelocity.h"

namespace
{
	// 直接读取原始分布，不改模块或初始化烘焙缓存；日志用于校准素材，不冒充渲染验收。
	FString DescribeVector(const FRawDistributionVector& Raw)
	{
		if (!Raw.Distribution) return TEXT("no authored distribution");
		FVector Min, Max;
		Raw.Distribution->GetRange(Min, Max);
		return FString::Printf(TEXT("%s range=[%s]..[%s]"), *Raw.Distribution->GetClass()->GetName(),
			*Min.ToCompactString(), *Max.ToCompactString());
	}

	// 检查素材分布范围没有非法向量，不通过初始化缓存或改资产掩盖问题。
	bool FiniteVector(const FRawDistributionVector& Raw)
	{
		if (!Raw.Distribution) return true;
		FVector Min, Max;
		Raw.Distribution->GetRange(Min, Max);
		return !Min.ContainsNaN() && !Max.ContainsNaN();
	}

	// 遍历有效发射器的原始位置、速度、空间和寿命，记录轴向诊断而非推定实际渲染轨迹。
	void AuditCascade(FAutomationTestBase& Test, const TCHAR* Role, UParticleSystem* System)
	{
		if (!System) return;
		Test.AddInfo(FString::Printf(TEXT("VFX_ASSET role=%s asset=%s emitters=%d"), Role,
			*System->GetPathName(), System->Emitters.Num()));
		for (const auto& Emitter : System->Emitters)
		{
			if (!Emitter) continue;
			for (const auto& LOD : Emitter->LODLevels)
			{
				if (!LOD || !LOD->bEnabled || !LOD->RequiredModule) continue;
				const auto* Required = LOD->RequiredModule.Get();
				Test.AddInfo(FString::Printf(TEXT("VFX_EMITTER role=%s emitter=%s lod=%d local=%d origin=[%s] rotation=[%s] type=%s material=%s"),
					Role, *Emitter->EmitterName.ToString(), LOD->Level, Required->bUseLocalSpace,
					*Required->EmitterOrigin.ToCompactString(), *Required->EmitterRotation.ToCompactString(),
					*GetNameSafe(LOD->TypeDataModule.Get()), *GetPathNameSafe(Required->Material.Get())));
				FVector Location = Required->EmitterOrigin, LocalVelocity = FVector::ZeroVector;
				FVector WorldVelocity = FVector::ZeroVector, LocalAcceleration = FVector::ZeroVector;
				FVector WorldAcceleration = FVector::ZeroVector;
				bool bHasVelocity = false;
				for (const auto& Module : LOD->Modules)
				{
					if (!Module || !Module->bEnabled) continue;
					if (const auto* Initial = Cast<UParticleModuleLocation>(Module))
					{
						Test.AddInfo(FString::Printf(TEXT("VFX_LOCATION role=%s emitter=%s %s"),
							Role, *Emitter->EmitterName.ToString(), *DescribeVector(Initial->StartLocation)));
						Test.TestTrue(TEXT("Authored initial locations are finite"), FiniteVector(Initial->StartLocation));
						if (Initial->StartLocation.Distribution)
						{
							FVector Min, Max;
							Initial->StartLocation.Distribution->GetRange(Min, Max);
							Location += (Min + Max) * .5;
						}
					}
					if (const auto* Velocity = Cast<UParticleModuleVelocity>(Module))
					{
						Test.AddInfo(FString::Printf(TEXT("VFX_VELOCITY role=%s emitter=%s world=%d ownerScale=%d %s"), Role,
							*Emitter->EmitterName.ToString(), Velocity->bInWorldSpace, Velocity->bApplyOwnerScale,
							*DescribeVector(Velocity->StartVelocity)));
						Test.TestTrue(TEXT("Authored particle velocity is finite"), FiniteVector(Velocity->StartVelocity));
						if (Velocity->StartVelocity.Distribution)
						{
							FVector Min, Max;
							Velocity->StartVelocity.Distribution->GetRange(Min, Max);
							(Velocity->bInWorldSpace ? WorldVelocity : LocalVelocity) += (Min + Max) * .5;
							bHasVelocity = true;
						}
					}
					if (const auto* Acceleration = Cast<UParticleModuleAccelerationConstant>(Module))
					{
						Test.AddInfo(FString::Printf(TEXT("VFX_ACCELERATION role=%s emitter=%s world=%d value=[%s]"),
							Role, *Emitter->EmitterName.ToString(), Acceleration->bAlwaysInWorldSpace,
							*Acceleration->Acceleration.ToCompactString()));
						(Acceleration->bAlwaysInWorldSpace ? WorldAcceleration : LocalAcceleration) += Acceleration->Acceleration;
					}
					if (const auto* Size = Cast<UParticleModuleSize>(Module))
						Test.AddInfo(FString::Printf(TEXT("VFX_SIZE role=%s emitter=%s %s"),
							Role, *Emitter->EmitterName.ToString(), *DescribeVector(Size->StartSize)));
					if (const auto* Lifetime = Cast<UParticleModuleLifetime>(Module); Lifetime && Lifetime->Lifetime.Distribution)
					{
						float Min, Max;
						Lifetime->Lifetime.Distribution->GetOutRange(Min, Max);
						Test.AddInfo(FString::Printf(TEXT("VFX_LIFETIME role=%s emitter=%s range=%.3f..%.3f"),
							Role, *Emitter->EmitterName.ToString(), Min, Max));
					}
				}
				// 不把随机范围的中点误称真实粒子轨迹：它只揭示原始轴和世界空间漂移，GPU/碰撞仍需画面检查。
				if (bHasVelocity)
				{
					const FVector LocalQuarterSecond = Required->EmitterRotation.RotateVector(
						Location + LocalVelocity * .25 + LocalAcceleration * .03125);
					const FVector WorldQuarterSecond = WorldVelocity * .25 + WorldAcceleration * .03125;
					Test.AddInfo(FString::Printf(TEXT("VFX_AUTHORED_AXIS role=%s emitter=%s midpointAt025sLocal=[%s] worldDrift=[%s] localVelocityForwardDot=%.3f (diagnostic only; not actual rendered bounds)"),
						Role, *Emitter->EmitterName.ToString(), *LocalQuarterSecond.ToCompactString(),
						*WorldQuarterSecond.ToCompactString(), LocalVelocity.GetSafeNormal().X));
				}
			}
		}
	}

	// 读取 Niagara 已公开的参数默认值，为位置与缩放校准保留素材侧证据。
	void AuditNiagara(FAutomationTestBase& Test, const TCHAR* Role, UNiagaraSystem* System)
	{
		if (!System) return;
		const FNiagaraParameterStore& Store = System->GetExposedParameters();
		TArray<FNiagaraVariable> Variables;
		Store.GetParameters(Variables);
		for (const FNiagaraVariable& Variable : Variables)
		{
			FString Value = TEXT("non-numeric");
			if (Variable.GetType() == FNiagaraTypeDefinition::GetFloatDef())
				Value = FString::SanitizeFloat(Store.GetParameterValue<float>(Variable));
			else if (Variable.GetType() == FNiagaraTypeDefinition::GetVec3Def())
				Value = Store.GetParameterValue<FVector3f>(Variable).ToString();
			Test.AddInfo(FString::Printf(TEXT("VFX_USER role=%s asset=%s name=%s type=%s value=%s"),
				Role, *System->GetPathName(), *Variable.GetName().ToString(), *Variable.GetType().GetName(), *Value));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatVfxAuthoredAssetAudit, "Multiplayer.Combat.VfxAuthoredAssetAudit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 使用正式配置审计素材内部的位置、轴、速度和公开缩放参数；本测试通过不代表视觉验收通过。
bool FCoopCombatVfxAuthoredAssetAudit::RunTest(const FString& Parameters)
{
	const auto* Config = LoadObject<UmultiplayerCombatConfig>(nullptr, TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig"));
	if (!TestNotNull(TEXT("Saved formal combat config exists"), Config)) return false;
	AddInfo(TEXT("VFX_AUDIT_SCOPE=read-only authored data; rendered particles and two-client visual acceptance remain separate"));
	AuditCascade(*this, TEXT("Attack"), Config->AttackFeedback.CascadeEffect.LoadSynchronous());
	AuditCascade(*this, TEXT("Projectile"), Config->ProjectileCascadeEffect.LoadSynchronous());
	AuditCascade(*this, TEXT("Heal"), Config->HealFeedback.CascadeEffect.LoadSynchronous());
	AuditCascade(*this, TEXT("CandidateCompactMuzzle"), LoadObject<UParticleSystem>(nullptr,
		TEXT("/Game/AdvancedARPGCombat/MagicCombatSystem/DemoAssets/GideonVFX/Particles/Primary/P_Gideon_Primary_MuzzleFlash.P_Gideon_Primary_MuzzleFlash")));
	AuditNiagara(*this, TEXT("Defense"), Config->DefenseFeedback.Effect.LoadSynchronous());
	AuditNiagara(*this, TEXT("Heal"), Config->HealFeedback.Effect.LoadSynchronous());
	AuditNiagara(*this, TEXT("Area"), Config->AreaFeedback.Effect.LoadSynchronous());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatVfxProjectCalibration, "Multiplayer.Combat.VfxProjectCalibration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 冷读项目副本的有效发射器及尾焰寿命；只证明配置界限，不能替代双客户端画面检查。
bool FCoopCombatVfxProjectCalibration::RunTest(const FString& Parameters)
{
	const auto* Config = LoadObject<UmultiplayerCombatConfig>(nullptr, TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig"));
	if (!TestNotNull(TEXT("Saved config exists"), Config)) return false;
	auto* Projectile = Config->ProjectileCascadeEffect.LoadSynchronous();
	auto* Shield = Config->HealFeedback.Effect.LoadSynchronous();
	if (!TestNotNull(TEXT("Project projectile copy exists"), Projectile) || !TestNotNull(TEXT("Project shield copy exists"), Shield)) return false;
	TestEqual(TEXT("Formal projectile uses project copy"), Projectile->GetPathName(),
		FString(TEXT("/Game/Combat/Presentation/Effects/P_CombatFireProjectile.P_CombatFireProjectile")));
	TestEqual(TEXT("Formal healing shield uses project copy"), Shield->GetPathName(),
		FString(TEXT("/Game/Combat/Presentation/Effects/NS_CombatDefenseShield.NS_CombatDefenseShield")));
	TestTrue(TEXT("Defense has no Niagara effect"), Config->DefenseFeedback.Effect.IsNull());
	TestTrue(TEXT("Defense has no Cascade effect"), Config->DefenseFeedback.CascadeEffect.IsNull());
	TestTrue(TEXT("Healing does not replace the shield with a Cascade fallback"), Config->HealFeedback.CascadeEffect.IsNull());
	int32 Visible = 0;
	for (const auto& Emitter : Projectile->Emitters)
		if (Emitter)
			for (const auto& LOD : Emitter->LODLevels)
				if (LOD && LOD->bEnabled)
				{
					++Visible;
					const FName Name = Emitter->EmitterName;
					TestTrue(TEXT("Only compact flame/head emitters remain"), Name == TEXT("LittleFlareSoft") || Name == TEXT("Flame_Smoke") || Name == TEXT("detail_fires"));
					for (const auto& Module : LOD->Modules)
						if (const auto* Lifetime = Cast<UParticleModuleLifetime>(Module); Lifetime && Lifetime->Lifetime.Distribution)
						{
							float Min, Max; Lifetime->Lifetime.Distribution->GetOutRange(Min, Max);
							TestTrue(TEXT("Projectile particles expire within sixty milliseconds"), Min > 0 && Max <= .0601f);
						}
				}
	TestTrue(TEXT("Compact projectile retains active emitters"), Visible > 0);
	const auto& Store = Shield->GetExposedParameters();
	for (const TCHAR* Name : { TEXT("User.Scale_Wave1"), TEXT("User.Scale_Wave2"), TEXT("User.Scale_Bubble1"), TEXT("User.Scale_Sparks1") })
		TestEqual(TEXT("Unrelated ground waves and orbiting particles are disabled"),
			Store.GetParameterValue<float>(FNiagaraVariable(FNiagaraTypeDefinition::GetFloatDef(), Name)), 0.f);
	TestTrue(TEXT("Foot-origin shield keeps authored ninety-centimeter center offset"),
		Store.GetParameterValue<FVector3f>(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("User.Offset_Shield"))).Equals(FVector3f(0, 0, 90)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCoopCombatCastForwardBurst, "Multiplayer.Combat.CastForwardBurst",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 冷读实际项目起手副本，不能用组件朝向代替检查素材内部下坠和宽散射。
bool FCoopCombatCastForwardBurst::RunTest(const FString& Parameters)
{
	const auto* Config = LoadObject<UmultiplayerCombatConfig>(nullptr, TEXT("/Game/Combat/DA_CombatConfig.DA_CombatConfig"));
	auto* CastEffect = Config ? Config->AttackFeedback.CascadeEffect.LoadSynchronous() : nullptr;
	if (!TestNotNull(TEXT("Formal cast effect exists"), CastEffect)) return false;
	TestEqual(TEXT("Only project cast copy is calibrated"), CastEffect->GetPathName(),
		FString(TEXT("/Game/Combat/Presentation/Effects/P_CombatFireCast.P_CombatFireCast")));
	int32 ForwardModules = 0;
	for (const auto& Emitter : CastEffect->Emitters)
		if (Emitter)
			for (const auto& LOD : Emitter->LODLevels)
				if (LOD && LOD->bEnabled)
					for (const auto& Module : LOD->Modules)
					{
						if (const auto* Velocity = Cast<UParticleModuleVelocity>(Module); Velocity && Velocity->bEnabled)
						{
							TestFalse(TEXT("Cast velocity rotates with local forward, not world axes"), bool(Velocity->bInWorldSpace));
							if (!TestNotNull(TEXT("Cast has authored velocity distribution"), Velocity->StartVelocity.Distribution.Get())) continue;
							FVector Min, Max; Velocity->StartVelocity.Distribution->GetRange(Min, Max);
							TestTrue(TEXT("Cast is a bounded narrow forward cone without a downward bias"), Min.X >= 350 && Max.X <= 550
								&& FMath::Abs(Min.Y) <= 18 && FMath::Abs(Max.Y) <= 18 && FMath::Abs(Min.Z) <= 18 && FMath::Abs(Max.Z) <= 18
								&& FMath::IsNearlyZero(Min.Z + Max.Z));
							++ForwardModules;
						}
						if (const auto* Gravity = Cast<UParticleModuleAccelerationConstant>(Module); Gravity && Gravity->bEnabled)
							TestTrue(TEXT("Cast particles do not retain source world gravity"), Gravity->Acceleration.IsNearlyZero());
					}
	TestTrue(TEXT("Visible cast retains actual forward emission modules"), ForwardModules > 0);
	return true;
}

#endif
