#include "kz_invisible.h"
#include "kz/kz.h"
#include "kz/language/kz_language.h"
#include "kz/timer/kz_timer.h"

#include "sdk/services.h"
#include "utils/utils.h"
#include "utils/ctimer.h"
#include "utils/json.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "tier0/memdbgon.h"

#define KZ_INVISIBLE_LIST_FILE "cfg/cyb_invisible.json"

// Возврат выдернутого наблюдателя: не чаще раза в этот интервал.
#define KZ_INVISIBLE_RESTORE_PERIOD 5.0f
// Что считать ОДНОЙ серией. Драка — это возвраты, ложащиеся вплотную к рубежу выше;
// вдвое больший интервал даёт запас на джиттер и при этом в разы меньше периода
// mp_force_pick_time (60 с), чтобы ожидаемый периодический форс в серию не складывался.
#define KZ_INVISIBLE_RESTORE_STREAK (2.0f * KZ_INVISIBLE_RESTORE_PERIOD)
// Длина серии, после которой сдаёмся (см. OnGameFrame).
#define KZ_INVISIBLE_MAX_RESTORES 3

// Список невидимок (steamid64). Мутации только на игровом потоке (плагин-лоад,
// map start, ConCommand'ы), чтение — сторож OnGameFrame/JoinTeam/!specs там же.
static_global std::unordered_set<u64> s_invisibleSteamIds;

// Сколько невидимок сейчас ОНЛАЙН (кэш-флаги игроков). Гейт горячих путей: файл со
// списком админов на проде непуст всегда, а вот невидимка на сервере — редкость.
static_global i32 s_onlineInvisibleCount = 0;

// 0 — прятать из TAB командой NONE (клиент открывает меню команды), 1 — флагом HLTV в наблюдателях,
// 2 (по умолчанию с 20.11) — настоящие наблюдатели, контроллер не передаётся остальным.
CConVar<i32> kz_invisible_mode("kz_invisible_mode", FCVAR_NONE,
								"Invisible hide mode: 0 team NONE, 1 HLTV flag in spectators, 2 spectators + controller not transmitted", 2);
// Опыт 19.11: принудительно прятать индекс от всех (бот как подопытный — смотреть со стороны).
static_global bool s_forceHideIndex[MAXPLAYERS + 1] = {};

// Пересчитать кэш-флаги всем игрокам после смены списка. Счётчик онлайн-невидимок
// пересобирается с нуля — защита от дрейфа инкрементальной арифметики (сиротский флаг
// отклонённого коннекта чинит декремент в OnPlayerConnect при переиспользовании слота).
static_function void RefreshAllPlayers()
{
	i32 onlineInvisibleCount = 0;
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->invisibleService)
		{
			continue;
		}
		player->invisibleService->RefreshFlag();
		if (player->invisibleService->IsInvisible())
		{
			onlineInvisibleCount++;
		}
	}
	s_onlineInvisibleCount = onlineInvisibleCount;
}

// Разбор {"steamids": ["7656119...", ...]}. Любая невалидная запись бракует весь файл
// (список не меняем): полуприменённый список хуже старого.
static_function bool ParseList(const std::string &text, std::unordered_set<u64> &out, const char **reason)
{
	nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
	if (json.is_discarded())
	{
		*reason = "invalid_json";
		return false;
	}
	if (!json.is_object() || !json.contains("steamids") || !json["steamids"].is_array())
	{
		*reason = "missing_steamids_array";
		return false;
	}
	for (const auto &item : json["steamids"])
	{
		if (!item.is_string())
		{
			*reason = "steamid_not_string";
			return false;
		}
		const std::string &str = item.get_ref<const std::string &>();
		char *end = nullptr;
		u64 steamId = strtoull(str.c_str(), &end, 10);
		if (steamId == 0 || !end || *end != '\0')
		{
			*reason = "bad_steamid";
			return false;
		}
		out.insert(steamId);
	}
	return true;
}

void KZInvisibleService::Init()
{
	KZInvisibleService::LoadFromFile("plugin_load");
}

void KZInvisibleService::OnActivateServer()
{
	KZInvisibleService::LoadFromFile("map_start");
}

void KZInvisibleService::LoadFromFile(const char *source)
{
	char absPath[1024];
	V_snprintf(absPath, sizeof(absPath), "%s/csgo/%s", Plat_GetGameDirectory(), KZ_INVISIBLE_LIST_FILE);

	FILE *fp = fopen(absPath, "rb");
	if (!fp)
	{
		// Файл отсутствует — легальное состояние: пустой список.
		s_invisibleSteamIds.clear();
		RefreshAllPlayers();
		KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_list_loaded count=0 source=%s file=absent\n", source);
		return;
	}
	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	std::string text;
	if (size > 0)
	{
		text.resize(size);
		size_t bytesRead = fread(text.data(), 1, size, fp);
		text.resize(bytesRead);
	}
	fclose(fp);

	std::unordered_set<u64> parsed;
	const char *reason = "unknown";
	if (!ParseList(text, parsed, &reason))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_list_rejected reason=%s source=%s file=%s\n", reason, source, KZ_INVISIBLE_LIST_FILE);
		return;
	}

	s_invisibleSteamIds = std::move(parsed);
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_list_loaded count=%u source=%s\n", (u32)s_invisibleSteamIds.size(), source);
}

void KZInvisibleService::OnAllPluginsLoaded()
{
	// Late load: наш Init() уже применил список, но ResetPlayers() из AllPluginsLoaded
	// обнуляет сервисы ПОСЛЕ него — без повторного применения meta reload на живом
	// сервере молча снимал бы невидимость до смены карты. Заодно пересобирается счётчик.
	RefreshAllPlayers();
}

bool KZInvisibleService::IsInvisibleSteamId(u64 steamID64)
{
	return steamID64 != 0 && s_invisibleSteamIds.count(steamID64) > 0;
}

bool KZInvisibleService::HasOnlineInvisibles()
{
	return s_onlineInvisibleCount > 0;
}

bool KZInvisibleService::IsInvisible(KZPlayer *player)
{
	return player && player->invisibleService && player->invisibleService->IsInvisible();
}

bool KZInvisibleService::ShouldHideFrom(KZPlayer *subject, KZPlayer *viewer)
{
	if (!subject || !viewer || subject == viewer)
	{
		return false;
	}
	return IsInvisible(subject) && !IsInvisible(viewer);
}

void KZInvisibleService::Reset()
{
	if (this->invisible)
	{
		s_onlineInvisibleCount--;
		// Второй (и единственный молчаливый) путь гашения флага — без него карта диагностики
		// неполна: по логу видно, что невидимость снял дисконнект/реюз слота, а не список.
		KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_flag steam_id=%llu state=off reason=reset\n", this->steamId64);
	}
	this->steamId64 = 0;
	this->invisible = false;
	this->observing = false;
	this->suppressTeamEvent = false;
	this->nextRestoreTime = 0.0f;
	this->restoreAttempts = 0;
	this->lastRestoreTime = 0.0f;
}

void KZInvisibleService::OnPlayerConnect(u64 steamID64)
{
	// Слот мог остаться «сиротой» после отклонённого коннекта (OnClientDisconnect не
	// приходил, Reset не звался) — сперва снимаем его вклад из счётчика.
	if (this->invisible)
	{
		s_onlineInvisibleCount--;
	}
	// Эпоха OnClientConnect: xuid уже известен — невидимка скрыт с первой секунды.
	this->steamId64 = steamID64;
	this->invisible = IsInvisibleSteamId(steamID64);
	if (this->invisible)
	{
		s_onlineInvisibleCount++;
	}
}

// Вход невидимки (20.11 владелец: «зашёл на сервер — и сразу невидимый наблюдатель, без выбора
// команды»). Через паузу после active: на самом active пешки наблюдателя ещё нет, а клиент
// ещё не открыл меню команды — закрывать нечего.
static_function f64 AutoSpectateOnJoin(CPlayerUserId userID)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
	if (!player || !player->invisibleService || !player->invisibleService->IsInvisible() || !player->IsInGame())
	{
		return 0.0f;
	}
	CCSPlayerController *controller = player->GetController();
	if (!controller || controller->GetTeam() != CS_TEAM_NONE)
	{
		return 0.0f; // уже выбрал команду сам
	}
	KZ::misc::JoinTeam(player, CS_TEAM_SPECTATOR, false, false);
	KZ::misc::CloseTeamMenu(player, 0.25f);
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_auto_spec steam_id=%llu team_after=%d\n", player->GetSteamId64(false),
				controller->GetTeam());
	return 0.0f;
}

void KZInvisibleService::OnPlayerActive()
{
	if (this->invisible && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, "Invisible - Active");
		if (kz_invisible_mode.Get() == 2)
		{
			if (CServerSideClient *client = this->player->GetClient())
			{
				StartTimer<CPlayerUserId>(AutoSpectateOnJoin, client->GetUserID(), 0.5f, false, false);
			}
		}
	}
}

// Вернуть цель наблюдения после смены команды. false — observer pawn ещё не создан,
// вызывающему надо повторить.
static_function bool ApplyObserverTarget(CCSPlayerController *controller, CHandle<CBaseEntity> target, ObserverMode_t mode)
{
	if (!target.IsValid())
	{
		return true; // наблюдали фрикамом — возвращать нечего
	}
	if (!controller)
	{
		return true;
	}
	CCSPlayerPawnBase *observerPawn = controller->GetObserverPawn();
	if (!observerPawn || !observerPawn->m_pObserverServices)
	{
		return false;
	}
	CPlayer_ObserverServices *obsService = observerPawn->m_pObserverServices;
	if (obsService->m_hObserverTarget() != target)
	{
		obsService->m_iObserverMode(mode);
		obsService->m_hObserverTarget(target);
	}
	return true;
}

// Одна повторная попытка на ближайшем ProcessTimers (пост-симулейт того же кадра — между
// планированием и повтором проходит шаг симуляции, в котором пешки и создаются). Больше не
// повторяем: если pawn не появился и там, цель потеряна по причине, которую отсюда не починить.
// Отказ ОБЯЗАН быть в логах: живьём механизм не проверен, и без строки он неотличим от мёртвого
// кода — с флота никак не узнать, срабатывает ли гипотеза «ChangeTeam роняет цель» вообще.
static_function f64 ReassertObserverTarget(CPlayerUserId userID, CHandle<CBaseEntity> target, ObserverMode_t mode)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
	if (player && !ApplyObserverTarget(player->GetController(), target, mode))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_observer_target_lost steam_id=%llu reason=pawn_not_ready\n",
					player->GetSteamId64(false));
	}
	return 0.0f;
}

void KZInvisibleService::SetObserverTeam(int team)
{
	CCSPlayerController *controller = this->player->GetController();
	if (!controller || controller->GetTeam() == team)
	{
		return;
	}
	// Смена команды может пересоздать observer pawn и потерять цель наблюдения — админ
	// оказался бы во фрикаме вместо читера, за которым следил. Живьём это не проверено,
	// поэтому снимаем слепок цели и возвращаем его после: верно и если pawn пережил смену
	// команды (тогда восстановление — no-op), и если нет.
	ObserverMode_t mode = OBS_MODE_NONE;
	CHandle<CBaseEntity> target;
	if (CCSPlayerPawnBase *observerPawn = controller->GetObserverPawn())
	{
		if (CPlayer_ObserverServices *obsService = observerPawn->m_pObserverServices)
		{
			mode = obsService->m_iObserverMode();
			target = obsService->m_hObserverTarget();
		}
	}

	this->suppressTeamEvent = true;
	controller->ChangeTeam(team);
	// Снимаем и здесь: хук ChangeTeam синхронный и флаг уже съеден, но если движок
	// почему-то не позвал его, подавление не должно утечь на чужую смену команды.
	this->suppressTeamEvent = false;

	if (ApplyObserverTarget(controller, target, mode))
	{
		return;
	}
	// pawn ещё не готов — доделываем таймером, а НЕ сторожом: сторож фильтрует игроков по
	// IsInvisible и может вовсе не тикать (гейт HasOnlineInvisibles), поэтому обратный путь —
	// «сняли из списка во время слежки» — до него не дошёл бы никогда. Ключ — userID, а не
	// указатель: за кадр игрок мог выйти, а слот — переиспользоваться. Идиома взята у
	// TeleportObserver (kz_spec.cpp). Нулевой интервал здесь ещё и защита от переноса CHandle
	// через смену карты: RemoveNonPersistentTimers() в форке ниоткуда не зовётся, так что на
	// флаг preserveMapChange полагаться нельзя — таймер успевает отработать задолго до
	// changelevel только потому, что интервал нулевой.
	CServerSideClient *client = this->player->GetClient();
	if (!client)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_observer_target_lost steam_id=%llu reason=no_client\n", this->steamId64);
		return;
	}
	StartTimer<CPlayerUserId, CHandle<CBaseEntity>, ObserverMode_t>(ReassertObserverTarget, client->GetUserID(), target, mode, 0.0f, false, false);
}

void KZInvisibleService::EnforceObserverTeam()
{
	if (!this->invisible || !this->player->IsInGame())
	{
		return;
	}
	CCSPlayerController *controller = this->player->GetController();
	// Живая команда — это игрок, который сам попросился в игру: невидимость даётся только
	// наблюдателю (инвариант), возвращать его сюда нельзя.
	if (!controller || controller->GetTeam() != CS_TEAM_SPECTATOR)
	{
		return;
	}
	if (kz_invisible_mode.Get() == 2)
	{
		// Режим 2: остаёмся в НАСТОЯЩИХ наблюдателях (клиент не открывает меню команды), а из
		// TAB прячет CheckTransmit — контроллер невидимки не уходит остальным (FilterTransmit).
		if (!this->observing)
		{
			this->observing = true;
			KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_observer_team steam_id=%llu mode=transmit\n", this->steamId64);
		}
		return;
	}
	if (kz_invisible_mode.Get() == 1)
	{
		// Режим 1 (опыт 19.11): остаёмся в НАСТОЯЩИХ наблюдателях — клиент не открывает меню
		// команды, — а из TAB прячемся флагом HLTV на контроллере (GOTV в табе не виден).
		if (this->observing && controller->m_bIsHLTV())
		{
			return;
		}
		controller->m_bIsHLTV(true);
		this->observing = true;
		KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_observer_team steam_id=%llu mode=hltv\n", this->steamId64);
		return;
	}
	this->SetObserverTeam(CS_TEAM_NONE);
	this->observing = true;
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_observer_team steam_id=%llu team=none\n", this->steamId64);
	// Клиент CS2 сам открывает меню выбора команды, когда его команда становится NONE (проверено
	// 19.11 на канарейке). Закрываем ПОСЛЕ того, как клиент получил смену команды: закрытие до неё
	// (в jointeam) меню не спасало, а при входе на сервер в этом меню нет даже «Отмены» — невидимка
	// застревал. Задержка — чтобы смена команды дошла до клиента раньше спуфа.
	KZ::misc::CloseTeamMenu(this->player, 0.25f);
}

bool KZInvisibleService::OnChangeTeamPost(i32 team)
{
	if (this->suppressTeamEvent)
	{
		this->suppressTeamEvent = false;
		return true;
	}
	// Страховка к явным OnObserveEnd() в обёртках: если заход в живую команду прошёл
	// мимо них, но внутри намеренной смены (changingTeam), это всё равно не форс движка —
	// наблюдение окончено, сторож игрока больше не возвращает.
	if (team >= CS_TEAM_T && this->player->timerService->IsChangingTeam())
	{
		this->observing = false;
	}
	return false;
}

void KZInvisibleService::OnGameFrame()
{
	if (!HasOnlineInvisibles())
	{
		return;
	}
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer((u32)i);
		if (!player || !player->invisibleService || !player->invisibleService->IsInvisible() || !player->IsInGame())
		{
			continue;
		}
		KZInvisibleService *service = player->invisibleService;
		CCSPlayerController *controller = player->GetController();
		if (!controller)
		{
			continue;
		}
		// Штатный путь скрытия: сюда игрок приходит следующим кадром после ухода в
		// наблюдатели (KZ::misc::JoinTeam намеренно НЕ прячет сам — см. комментарий там).
		if (controller->GetTeam() == CS_TEAM_SPECTATOR)
		{
			if (!service->observing)
			{
				KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_watch_spec steam_id=%llu in_game=%d\n", service->steamId64, player->IsInGame());
			}
			service->EnforceObserverTeam();
			continue;
		}
		// Наблюдателя выдернули в живую команду мимо наших обёрток — единственный известный
		// источник этого движковый mp_force_pick_time. Возвращаем, но не чаще раза в 5 с:
		// если движок будет спорить, мы не уйдём с ним в покадровую драку, а оставим след.
		// realtime, а не curtime: игровое время обнуляется на смене карты, и рубеж из
		// прошлой карты заблокировал бы возврат на новой.
		f32 now = g_pKZUtils->GetServerGlobals()->realtime;
		if (service->observing && controller->GetTeam() >= CS_TEAM_T && now >= service->nextRestoreTime)
		{
			// Новый эпизод, а не продолжение драки: серию рвёт пауза. Без этого счётчик
			// мерил бы «сколько раз за сессию нас трогали» и выдыхался бы на самом
			// ожидаемом источнике — периодическом mp_force_pick_time.
			if (now - service->lastRestoreTime > KZ_INVISIBLE_RESTORE_STREAK)
			{
				service->restoreAttempts = 0;
			}
			service->lastRestoreTime = now;
			service->nextRestoreTime = now + KZ_INVISIBLE_RESTORE_PERIOD;
			if (service->restoreAttempts >= KZ_INVISIBLE_MAX_RESTORES)
			{
				// Капитуляция: команду держит не движок, а кто-то, кто спорит с нами всерьёз.
				// Оставляем игрока обычным видимым живым игроком — инвариант «невидимость
				// только наблюдателю» этим не нарушен, а вечный флап был бы хуже.
				// Игроку говорим В ЧАТ: он пришёл следить за читером и обязан узнать, что
				// скрытности больше нет, — молчаливая её потеря хуже самого отказа.
				service->observing = false;
				KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_restore_gaveup steam_id=%llu attempts=%d\n", service->steamId64,
							service->restoreAttempts);
				if (!player->IsFakeClient())
				{
					player->languageService->PrintChat(true, false, "Invisible - Observe Lost");
				}
				continue;
			}
			service->restoreAttempts++;
			KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_team_forced steam_id=%llu team=%d attempt=%d reason=external\n", service->steamId64,
						controller->GetTeam(), service->restoreAttempts);
			// savePos=false: точку возврата игрок задал, когда уходил наблюдать сам, —
			// перезаписывать её местом форс-спавна нельзя.
			KZ::misc::JoinTeam(player, CS_TEAM_SPECTATOR, false, false);
		}
	}
}

void KZInvisibleService::RefreshFlag()
{
	// steamId64 эпохи коннекта; фолбэк для late load плагина на живом сервере.
	// Пересчёт зависит ТОЛЬКО от steamId64 + списка; любой гейт по состоянию
	// клиента/сущностей на границах сессии/смены карты необратимо гасит флаг до
	// следующей мутации списка. Сиротский флаг (коннект отклонён) безвреден: pawn нет,
	// максимум держит горячий гейт открытым; вклад в счётчик снимает декремент в
	// OnPlayerConnect при переиспользовании слота.
	u64 steamId = this->steamId64 ? this->steamId64 : this->player->GetSteamId64(false);
	bool newInvisible = IsInvisibleSteamId(steamId);
	if (newInvisible == this->invisible)
	{
		return;
	}
	this->invisible = newInvisible;
	// Диагностика репорта 05.08 («мигаю в TAB»): по этой строке видно, мигает ли САМ ФЛАГ
	// (список приезжает то с игроком, то без — значит на платформе спорят два писателя) или
	// флаг стоит, а мигает картинка у зрителя. Путь холодный — только мутация списка.
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_flag steam_id=%llu state=%s reason=list\n", steamId, newInvisible ? "on" : "off");
	// Счётчик здесь не трогаем: единственный вызывающий — RefreshAllPlayers, он
	// пересобирает s_onlineInvisibleCount целиком после обхода.
	if (this->player->IsInGame() && !this->player->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, newInvisible ? "Invisible - Active" : "Invisible - Inactive");
	}
	// Смена статуса на живом игроке в наблюдателях меняет и его команду: попал в список —
	// прячем (NONE), выпал — возвращаем в спектаторы, иначе он остался бы скрыт навсегда.
	if (newInvisible)
	{
		this->EnforceObserverTeam();
	}
	else if (this->observing)
	{
		this->SetObserverTeam(CS_TEAM_SPECTATOR);
		this->ClearHltvFlag();
		this->observing = false;
	}
}

// Живое управление списком без файла. Настоящие ConCommand'ы (не SCMD) — зовутся с
// серверной консоли/RCON без игрока (урок cyb.33, см. kz_customchangemap.cpp).
// Игрокам команды недоступны: наличие controller у слота = отказ.

static_function bool DenyIfPlayer(const CCommandContext &context, const char *command)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_cmd_denied cmd=%s reason=not_server slot=%d\n", command, context.GetPlayerSlot().Get());
		return true;
	}
	return false;
}

CON_COMMAND_F(kz_invisible_reload, "Reload the invisible players list from cfg/cyb_invisible.json.", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_reload"))
	{
		return;
	}
	KZInvisibleService::LoadFromFile("rcon_reload");
}

CON_COMMAND_F(kz_invisible_add, "Add a SteamID64 to the invisible players list (runtime only, not persisted).", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_add"))
	{
		return;
	}
	u64 steamId = args.ArgC() >= 2 ? strtoull(args.Arg(1), nullptr, 10) : 0;
	if (steamId == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_add_rejected reason=bad_steamid arg=%s\n", args.ArgC() >= 2 ? args.Arg(1) : "<none>");
		return;
	}
	s_invisibleSteamIds.insert(steamId);
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_add steam_id=%llu count=%u\n", steamId, (u32)s_invisibleSteamIds.size());
}

CON_COMMAND_F(kz_invisible_remove, "Remove a SteamID64 from the invisible players list (runtime only, not persisted).", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_remove"))
	{
		return;
	}
	u64 steamId = args.ArgC() >= 2 ? strtoull(args.Arg(1), nullptr, 10) : 0;
	if (steamId == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_remove_rejected reason=bad_steamid arg=%s\n", args.ArgC() >= 2 ? args.Arg(1) : "<none>");
		return;
	}
	if (s_invisibleSteamIds.erase(steamId) == 0)
	{
		KZ_LOG_WARN(LogChannel::General, "[cyb] invisible_remove_rejected reason=not_in_list steam_id=%llu\n", steamId);
		return;
	}
	RefreshAllPlayers();
	KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_remove steam_id=%llu count=%u\n", steamId, (u32)s_invisibleSteamIds.size());
}

// Снять флаг HLTV режима 1 — при выходе из наблюдения/невидимости.
void KZInvisibleService::ClearHltvFlag()
{
	CCSPlayerController *controller = this->player->GetController();
	if (controller && controller->m_bIsHLTV())
	{
		controller->m_bIsHLTV(false);
		KZ_LOG_INFO(LogChannel::General, "[cyb] invisible_hltv_off steam_id=%llu\n", this->steamId64);
	}
}

// Отладка опыта 19.11: поменять поле контроллера игрока на лету. Только консоль/RCON.
CON_COMMAND_F(kz_invisible_dbg, "kz_invisible_dbg <steamid64> <hltv|pending|team|connected> <value>", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_dbg") || args.ArgC() < 4)
	{
		return;
	}
	u64 steamId = strtoull(args.Arg(1), nullptr, 10);
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer((u32)i);
		if (!player || player->GetSteamId64(false) != steamId || !player->GetController())
		{
			continue;
		}
		CCSPlayerController *c = player->GetController();
		int v = atoi(args.Arg(3));
		const char *f = args.Arg(2);
		if (!V_stricmp(f, "hltv"))
		{
			c->m_bIsHLTV(v != 0);
		}
		else if (!V_stricmp(f, "pending"))
		{
			c->m_iPendingTeamNum((uint8)v);
		}
		else if (!V_stricmp(f, "team"))
		{
			c->ChangeTeam(v);
		}
		else if (!V_stricmp(f, "connected"))
		{
			c->m_iConnected((PlayerConnectedState)v);
		}
		META_CONPRINTF("kz_invisible_dbg: hltv=%d team=%d pending=%d connected=%d\n", c->m_bIsHLTV(), c->GetTeam(), c->m_iPendingTeamNum(),
					   (int)c->m_iConnected());
		return;
	}
	META_CONPRINTF("kz_invisible_dbg: not_on_server\n");
}

void KZInvisibleService::FilterTransmit(KZPlayer *target, CBitVec<16384> *edicts)
{
	bool modeOn = kz_invisible_mode.Get() == 2 && HasOnlineInvisibles();
	bool anyForced = false;
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		anyForced |= s_forceHideIndex[i];
	}
	if (!modeOn && !anyForced)
	{
		return;
	}
	for (i32 i = 1; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *p = g_pKZPlayerManager->ToPlayer((u32)i);
		if (!p || p == target)
		{
			continue;
		}
		CCSPlayerController *c = p->GetController();
		if (!c)
		{
			continue;
		}
		bool hide = s_forceHideIndex[i];
		// Не только наблюдающего, но и только что зашедшего (команда NONE): клиент, однажды
		// получивший контроллер, держит строку в TAB — прятать надо с первого снапшота.
		if (!hide && modeOn && p->invisibleService && p->invisibleService->invisible && c->GetTeam() <= CS_TEAM_SPECTATOR)
		{
			hide = !target->invisibleService || !target->invisibleService->invisible;
		}
		if (!hide)
		{
			continue;
		}
		// Пешку без контроллера отдавать нельзя (клиент падает, см. kz_quiet) — режем обе.
		if (CBasePlayerPawn *pawn = p->GetPlayerPawn())
		{
			edicts->Clear(pawn->entindex());
		}
		if (CBasePlayerPawn *obs = c->GetObserverPawn())
		{
			edicts->Clear(obs->entindex());
		}
		edicts->Clear(c->entindex());
	}
}

CON_COMMAND_F(kz_invisible_hide_index, "kz_invisible_hide_index <entindex 1-64> <0|1>: опыт — прятать игрока от всех", FCVAR_NONE)
{
	if (DenyIfPlayer(context, "kz_invisible_hide_index") || args.ArgC() < 3)
	{
		return;
	}
	i32 idx = atoi(args.Arg(1));
	if (idx < 1 || idx > MAXPLAYERS)
	{
		return;
	}
	s_forceHideIndex[idx] = atoi(args.Arg(2)) != 0;
	META_CONPRINTF("kz_invisible_hide_index: %d=%d\n", idx, s_forceHideIndex[idx]);
}
