#include "ArenaManager.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Math/UnrealMathUtility.h"
#include "UObject/UnrealType.h"
#include "Components/CapsuleComponent.h"

AArenaManager::AArenaManager()
{
	PrimaryActorTick.bCanEverTick = false;
	RoundConfigs.SetNum(ArenaRoundCount);
}

void AArenaManager::BeginPlay()
{
	Super::BeginPlay();
	CacheBaseStats();

	if (RoundConfigs.Num() != ArenaRoundCount)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("ArenaManager: RoundConfigs has %d entries — expected %d. Extra entries are ignored; missing ones use defaults."),
			RoundConfigs.Num(), ArenaRoundCount);
	}
}

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

void AArenaManager::StartArena()
{
	CurrentRound  = 0;
	EnemiesKilled = 0;
	TotalEnemies  = 0;
	BeginPreRound();
}

void AArenaManager::NotifyEnemyDied()
{
	HandleEnemyDied();
}

// -----------------------------------------------------------------------
// Round flow
// -----------------------------------------------------------------------

void AArenaManager::BeginPreRound()
{
	if (CurrentRound >= ArenaRoundCount || CurrentRound >= RoundConfigs.Num())
	{
		OnArenaComplete();
		return;
	}

	PreRoundTimeRemaining = PreRoundDuration;
	OnPreRoundBegin(CurrentRound + 1);

	GetWorldTimerManager().SetTimer(
		PreRoundCountdownHandle,
		this, &AArenaManager::TickPreRoundCountdown,
		0.1f, true);

	GetWorldTimerManager().SetTimer(
		RoundStartHandle,
		this, &AArenaManager::BeginRound,
		PreRoundDuration, false);
}

void AArenaManager::TickPreRoundCountdown()
{
	PreRoundTimeRemaining = FMath::Max(0.f, PreRoundTimeRemaining - 0.1f);
}

void AArenaManager::BeginRound()
{
	GetWorldTimerManager().ClearTimer(PreRoundCountdownHandle);
	PreRoundTimeRemaining = 0.f;

	const FArenaRoundConfig& Config = RoundConfigs[CurrentRound];

	// Build the full spawn queue before any enemy enters the world so TotalEnemies is accurate
	PendingSpawns.Empty();

	auto Enqueue = [&](int32 Count, TSubclassOf<AActor> Class, const TArray<AActor*>& Points)
	{
		if (!Class || Points.Num() == 0 || Count <= 0) return;
		for (int32 i = 0; i < Count; i++)
		{
			if (AActor* Point = PickRandomPoint(Points))
				PendingSpawns.Add({ Class, Point->GetActorTransform() });
		}
	};

	Enqueue(Config.BearCount,  BearClass,  BearSpawnPoints);
	Enqueue(Config.MothCount,  MothClass,  MothSpawnPoints);
	Enqueue(Config.SpidaCount, SpidaClass, SpidaSpawnPoints);

	// Shuffle so the spawn order is unpredictable
	for (int32 i = PendingSpawns.Num() - 1; i > 0; i--)
		PendingSpawns.Swap(i, FMath::RandRange(0, i));

	TotalEnemies  = PendingSpawns.Num();
	EnemiesKilled = 0;
	SpawnIndex    = 0;

	OnRoundBegin(CurrentRound + 1);

	if (TotalEnemies == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArenaManager: Round %d has no valid enemies/spawn points — skipping."), CurrentRound + 1);
		CheckRoundEnd();
		return;
	}

	const float Interval = Config.SpawnStaggerDuration / static_cast<float>(TotalEnemies);
	// Spawn the first enemy immediately, then repeat on the interval
	GetWorldTimerManager().SetTimer(SpawnHandle, this, &AArenaManager::SpawnNextEnemy, Interval, true, 0.f);
}

void AArenaManager::SpawnNextEnemy()
{
	if (SpawnIndex >= PendingSpawns.Num())
	{
		GetWorldTimerManager().ClearTimer(SpawnHandle);
		return;
	}

	const TPair<TSubclassOf<AActor>, FTransform>& Entry = PendingSpawns[SpawnIndex++];
	TSubclassOf<AActor>  Class     = Entry.Key;
	const FTransform&    Transform = Entry.Value;

	AActor* Enemy = GetWorld()->SpawnActorDeferred<AActor>(
		Class, Transform,
		nullptr, nullptr,
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

	if (!Enemy)
	{
		// Count the failed spawn as dead so it never blocks the round
		UE_LOG(LogTemp, Warning, TEXT("ArenaManager: SpawnActorDeferred failed for class %s."), *Class->GetName());
		HandleEnemyDied();
	}
	else
	{
		const FArenaRoundConfig& Config = RoundConfigs[CurrentRound];

		SetBoolProp(Enemy, TEXT("inArena"), true);

		Enemy->FinishSpawning(Transform);

		// Apply after FinishSpawning so BeginPlay (which may reset health) runs first
		if      (Class == BearClass)  ApplyStatsToEnemy(Enemy, Config, BearBase);
		else if (Class == MothClass)  ApplyStatsToEnemy(Enemy, Config, MothBase);
		else                          ApplyStatsToEnemy(Enemy, Config, SpidaBase);

		BindToEnemyDeath(Enemy);
		OnEnemySpawned(Enemy, CurrentRound + 1);
	}

	if (SpawnIndex >= PendingSpawns.Num())
		GetWorldTimerManager().ClearTimer(SpawnHandle);
}

void AArenaManager::CheckRoundEnd()
{
	if (EnemiesKilled < TotalEnemies) return;

	// Stop any remaining spawn timers — round is over
	GetWorldTimerManager().ClearTimer(SpawnHandle);

	OnRoundEnd(CurrentRound + 1);
	CurrentRound++;

	if (CurrentRound >= ArenaRoundCount || CurrentRound >= RoundConfigs.Num())
	{
		OnArenaComplete();
		return;
	}

	BeginPreRound();
}

// -----------------------------------------------------------------------
// Enemy wiring
// -----------------------------------------------------------------------

void AArenaManager::HandleEnemyDied()
{
	EnemiesKilled++;
	CheckRoundEnd();
}

void AArenaManager::BindToEnemyDeath(AActor* Enemy)
{
	UActorComponent* DmgSys = FindDmgSys(Enemy);
	if (!DmgSys)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArenaManager: Could not find BPC_dmgSys on %s — use NotifyEnemyDied() as fallback."), *Enemy->GetName());
		return;
	}

	FMulticastDelegateProperty* OnDeathProp =
		FindFProperty<FMulticastDelegateProperty>(DmgSys->GetClass(), TEXT("onDeath"));

	if (!OnDeathProp)
	{
		UE_LOG(LogTemp, Warning, TEXT("ArenaManager: Could not find onDeath dispatcher on BPC_dmgSys — use NotifyEnemyDied() as fallback."));
		return;
	}

	FScriptDelegate Delegate;
	Delegate.BindUFunction(this, FName("HandleEnemyDied"));

	FMulticastScriptDelegate* MulticastDelegate = OnDeathProp->ContainerPtrToValuePtr<FMulticastScriptDelegate>(DmgSys);
	if (MulticastDelegate && !MulticastDelegate->Contains(Delegate))
		MulticastDelegate->Add(Delegate);
}

// -----------------------------------------------------------------------
// Stat helpers
// -----------------------------------------------------------------------

void AArenaManager::CacheBaseStats()
{
	auto Cache = [&](TSubclassOf<AActor> Class, FEnemyBaseStats& Out, const TCHAR* Label)
	{
		if (!Class)
		{
			UE_LOG(LogTemp, Warning, TEXT("ArenaManager: %s class not set — base stats will be 0."), Label);
			return;
		}

		AActor* CDO = Class->GetDefaultObject<AActor>();
		Out.BaseDamage    = GetFloatProp(CDO, TEXT("DamageDealt"));
		if (UActorComponent* DmgSys = FindDmgSys(CDO))
			Out.BaseMaxHealth = GetFloatProp(DmgSys, TEXT("maxHealth"));

		UE_LOG(LogTemp, Log, TEXT("ArenaManager: %s base stats — Damage=%.1f  MaxHealth=%.1f"),
			Label, Out.BaseDamage, Out.BaseMaxHealth);
	};

	Cache(BearClass,  BearBase,  TEXT("Bear"));
	Cache(MothClass,  MothBase,  TEXT("Moth"));
	Cache(SpidaClass, SpidaBase, TEXT("Spida"));
}

void AArenaManager::ApplyStatsToEnemy(AActor* Enemy, const FArenaRoundConfig& Config, const FEnemyBaseStats& Base) const
{
	SetFloatProp(Enemy, TEXT("DamageDealt"), Base.BaseDamage * Config.DamageMultiplier);

	if (UActorComponent* DmgSys = FindDmgSys(Enemy))
	{
		const float CurrentMaxHealth = GetFloatProp(DmgSys, TEXT("maxHealth"));
		const float ScaledMaxHealth  = CurrentMaxHealth * Config.HealthMultiplier;
		SetFloatProp(DmgSys, TEXT("maxHealth"), ScaledMaxHealth);
		SetFloatProp(DmgSys, TEXT("health"),    ScaledMaxHealth);
	}
}

// -----------------------------------------------------------------------
// Reflection utilities
// -----------------------------------------------------------------------

float AArenaManager::GetFloatProp(UObject* Obj, FName Name, float Default)
{
	if (!Obj) return Default;
	if (const FDoubleProperty* P = FindFProperty<FDoubleProperty>(Obj->GetClass(), Name))
		return static_cast<float>(P->GetPropertyValue_InContainer(Obj));
	if (const FFloatProperty* P = FindFProperty<FFloatProperty>(Obj->GetClass(), Name))
		return P->GetPropertyValue_InContainer(Obj);
	return Default;
}

void AArenaManager::SetFloatProp(UObject* Obj, FName Name, float Value)
{
	if (!Obj) return;
	if (FDoubleProperty* P = FindFProperty<FDoubleProperty>(Obj->GetClass(), Name))
	{
		P->SetPropertyValue_InContainer(Obj, static_cast<double>(Value));
		return;
	}
	if (FFloatProperty* P = FindFProperty<FFloatProperty>(Obj->GetClass(), Name))
		P->SetPropertyValue_InContainer(Obj, Value);
}

void AArenaManager::SetBoolProp(UObject* Obj, FName Name, bool Value)
{
	if (!Obj) return;
	if (FBoolProperty* P = FindFProperty<FBoolProperty>(Obj->GetClass(), Name))
		P->SetPropertyValue_InContainer(Obj, Value);
}

UActorComponent* AArenaManager::FindDmgSys(AActor* Actor)
{
	if (!Actor) return nullptr;
	for (UActorComponent* Comp : Actor->GetComponents())
	{
		if (Comp && Comp->GetClass()->GetName().Contains(TEXT("BPC_dmgSys")))
			return Comp;
	}
	return nullptr;
}

AActor* AArenaManager::PickRandomPoint(const TArray<AActor*>& Points)
{
	if (Points.Num() == 0) return nullptr;
	return Points[FMath::RandRange(0, Points.Num() - 1)];
}
