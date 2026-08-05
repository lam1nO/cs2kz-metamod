
#include "common.h"
#include "kz/kz.h"
#include "kz/option/kz_option.h"
#include "sdk/gamerules.h"
// Make sure that the server can't run for too long.

// Original value
static_global CVValue_t *mp_timelimit_cvvalue;
static_global CVValue_t *mp_roundtime_cvvalue;
static_global CVValue_t *mp_roundtime_defuse_cvvalue;
static_global CVValue_t *mp_roundtime_hostage_cvvalue;

// Original max value. mp_timelimit has no max value.
static_global CVValue_t *mp_roundtime_cvvalue_max;
static_global CVValue_t *mp_roundtime_defuse_cvvalue_max;
static_global CVValue_t *mp_roundtime_hostage_cvvalue_max;

static_global CVValue_t hardcodedTimeLimit(1440.0f);
static_global bool cvarLoaded = false;
// Карты перетирают mp_timelimit/mp_roundtime своими cfg/point_servercommand
// (репро: kz_variety_fix -> 120). Чужие записи откатываем к defaultTimeLimit
// отложенно (следующий кадр), чтобы не входить в Set из колбэка конвара.
static_global bool g_timeLimitDirty = false;
static_global bool g_ownTimeLimitWrite = false;
CConVarRef<float> mp_timelimit("mp_timelimit");
CConVarRef<float> mp_roundtime("mp_roundtime");
CConVarRef<float> mp_roundtime_defuse("mp_roundtime_defuse");
CConVarRef<float> mp_roundtime_hostage("mp_roundtime_hostage");
CConVarRef<CUtlString> nextlevel("nextlevel");
ConVarRefAbstract *convars[] = {&mp_timelimit, &mp_roundtime, &mp_roundtime_defuse, &mp_roundtime_hostage};

// Значение для ПУСТОГО nextlevel — имя ТЕКУЩЕЙ карты (пустая строка = не знаем).
//
// Апстрим брал его из командной строки (`+map <name>`), и это первопричина падений
// kz-серверов по тайм-лимиту: наша launch-цепочка — «+map de_dust2 +host_workshop_map <id>»
// (workshop-карта обязана идти ПОСЛЕ +map), поэтому парсер ВСЕГДА возвращал de_dust2. Каждый
// старт ставил nextlevel=de_dust2 (факт с флота 05.08: так на всех восьми инстансах, при
// mp_match_end_changelevel false — тот этот путь не закрывает), и на «Game Over ... after N
// min» движок уводил kz-инстанс на официальную карту, чего он не переживает; контейнер потом
// поднимал node-agent. Правильный nextlevel для kz — та же карта: истечение тайм-лимита
// перезагружает её, ротацией занимается GG1 (голосование/setnextmap ставят nextlevel сами, и
// непустое значение мы не трогаем).
static_global std::string GetCurrentLevelName()
{
	bool hasMapName = false;
	CUtlString mapName = g_pKZUtils->GetCurrentMapName(&hasMapName);
	if (!hasMapName || mapName.IsEmpty())
	{
		// Ранняя фаза (EnforceTimeLimit из KZ::misc::Init до первой карты): НЕ подставляем
		// ничего. Пустой nextlevel хуже правильного, но безопаснее чужого: пустой движок
		// никуда не уводит, а EnforceTimeLimit зовётся ещё раз из OnActivateServer, когда имя
		// карты уже есть. Дефолт «de_dust2» в этой ветке недопустим ни в каком виде.
		return "";
	}
	return mapName.Get();
}

// Заполнить nextlevel текущей картой. Пишем только в ПУСТОЙ nextlevel (проверяет вызывающий);
// смена состояния сервера, не восстановимая из БД → логируем оба исхода с reason.
static_global void SetNextLevelToCurrentMap()
{
	std::string mapName = GetCurrentLevelName();
	if (mapName.empty())
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] nextlevel_skip reason=no_map_name\n");
		return;
	}
	nextlevel.Set(mapName.c_str());
	KZ_LOG_INFO(LogChannel::General, "[cyb] nextlevel_set map=%s reason=empty_nextlevel\n", mapName.c_str());
}

static_global void OnCvarChanged(ConVarRefAbstract *ref, CSplitScreenSlot nSlot, const char *pNewValue, const char *pOldValue, void *__unk01)
{
	assert(mp_timelimit.IsValidRef() && mp_timelimit.IsConVarDataAvailable());
	assert(mp_roundtime.IsValidRef() && mp_roundtime.IsConVarDataAvailable());
	assert(mp_roundtime_defuse.IsValidRef() && mp_roundtime_defuse.IsConVarDataAvailable());
	assert(mp_roundtime_hostage.IsValidRef() && mp_roundtime_hostage.IsConVarDataAvailable());
	assert(nextlevel.IsValidRef() && nextlevel.IsConVarDataAvailable());
	u16 refIndex = ref->GetAccessIndex();
	bool replicate = false;

	if (nextlevel.GetAccessIndex() == ref->GetAccessIndex() && nextlevel.Get().IsEmpty())
	{
		// The addon will be unloaded upon map change, so we need to set a valid nextlevel.
		SetNextLevelToCurrentMap();
		return;
	}

	for (ConVarRefAbstract *cvar : convars)
	{
		if (cvar->GetAccessIndex() == refIndex)
		{
			replicate = true;
			break;
		}
	}
	if (!replicate)
	{
		return;
	}
	if (!g_ownTimeLimitWrite)
	{
		f32 def = KZOptionService::GetOptionFloat("defaultTimeLimit", 60.0f);
		if (fabsf((f32)atof(pNewValue) - def) > 0.01f && !g_timeLimitDirty)
		{
			Msg("[kz] timelimit: внешняя запись %s (карта/cfg), откатываю к %.0f\n", pNewValue, def);
			g_timeLimitDirty = true;
		}
	}
	for (ConVarRefAbstract *cvar : convars)
	{
		if (!cvar->IsFlagSet(FCVAR_PERFORMING_CALLBACKS))
		{
			cvar->SetString(pNewValue);
		}
	}

	// Reflect the change in value to the HUD as well.
	CCSGameRules *gameRules = g_pKZUtils->GetGameRules();
	i32 newRoundTime = int(atof(pNewValue) * 60.0f);
	if (gameRules && gameRules->m_iRoundTime() != newRoundTime)
	{
		gameRules->m_iRoundTime(newRoundTime);
	}
}

void KZ::misc::EnforceTimeLimit()
{
	if (nextlevel.Get().IsEmpty())
	{
		SetNextLevelToCurrentMap();
	}

	if (cvarLoaded || !mp_timelimit.IsValidRef() || !mp_roundtime.IsValidRef() || !mp_roundtime_defuse.IsValidRef()
		|| !mp_roundtime_hostage.IsValidRef())
	{
		return;
	}
	cvarLoaded = true;
	mp_timelimit_cvvalue = mp_timelimit.GetConVarData()->Value(-1);
	mp_roundtime_cvvalue = mp_roundtime.GetConVarData()->Value(-1);
	mp_roundtime_defuse_cvvalue = mp_roundtime_defuse.GetConVarData()->Value(-1);
	mp_roundtime_hostage_cvvalue = mp_roundtime_hostage.GetConVarData()->Value(-1);

	mp_roundtime_cvvalue_max = mp_roundtime.GetConVarData()->MaxValue();
	mp_roundtime_defuse_cvvalue_max = mp_roundtime_defuse.GetConVarData()->MaxValue();
	mp_roundtime_hostage_cvvalue_max = mp_roundtime_hostage.GetConVarData()->MaxValue();

	mp_timelimit.GetConVarData()->SetMaxValue(&hardcodedTimeLimit);
	mp_roundtime.GetConVarData()->SetMaxValue(&hardcodedTimeLimit);
	mp_roundtime_defuse.GetConVarData()->SetMaxValue(&hardcodedTimeLimit);
	mp_roundtime_hostage.GetConVarData()->SetMaxValue(&hardcodedTimeLimit);
	g_pCVar->InstallGlobalChangeCallback(OnCvarChanged);
}

void KZ::misc::UnrestrictTimeLimit()
{
	if (!cvarLoaded)
	{
		return;
	}

	mp_timelimit.GetConVarData()->RemoveMaxValue();
	mp_roundtime.GetConVarData()->SetMaxValue(mp_roundtime_cvvalue_max);
	mp_roundtime_defuse.GetConVarData()->SetMaxValue(mp_roundtime_defuse_cvvalue_max);
	mp_roundtime_hostage.GetConVarData()->SetMaxValue(mp_roundtime_hostage_cvvalue_max);
	g_pCVar->RemoveGlobalChangeCallback(OnCvarChanged);
}

void KZ::misc::InitTimeLimit()
{
	g_ownTimeLimitWrite = true;
	mp_timelimit.Set(KZOptionService::GetOptionFloat("defaultTimeLimit", 60.0f));
	g_ownTimeLimitWrite = false;
	g_timeLimitDirty = false;
}

void KZ::misc::CheckTimeLimitOverride()
{
	if (!g_timeLimitDirty)
	{
		return;
	}
	g_timeLimitDirty = false;
	g_ownTimeLimitWrite = true;
	mp_timelimit.Set(KZOptionService::GetOptionFloat("defaultTimeLimit", 60.0f));
	g_ownTimeLimitWrite = false;
}
