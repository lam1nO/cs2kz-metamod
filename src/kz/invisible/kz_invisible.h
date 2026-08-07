#pragma once
#include "../kz.h"

// Режим невидимки (слежка за читерами), скоуп v3: пока невидимка наблюдает, его
// контроллер держится в CS_TEAM_NONE вместо CS_TEAM_SPECTATOR — клиент группирует
// скорборд по командам, и у команды NONE группы нет, поэтому строка из TAB пропадает.
// Плюс скрытие из !specs и тихий вход/выход (анонсы подавлены). Модель/звуки НЕ
// скрываем — невидимка обязан быть наблюдателем, живым он виден всем как обычный игрок.
// Видят его только он сам и другие невидимки.
// Прежний подход (чистка бита КОНТРОЛЛЕРА в CheckTransmit) снят как неработающий:
// CheckTransmit умеет только «не присылать апдейт» и не заставляет клиент забыть уже
// созданного игрока, а до входа в наблюдатели контроллер всегда успевает доехать.
// Источник списка: csgo/cfg/cyb_invisible.json ({"steamids": ["7656119...", ...]}),
// живое управление — ConCommand'ы kz_invisible_reload/add/remove (только сервер/RCON).
class KZInvisibleService : public KZBaseService
{
	using KZBaseService::KZBaseService;

	// steamid64 эпохи OnClientConnect (xuid): известен с первой секунды, до аутентификации.
	u64 steamId64 {};
	bool invisible {};
	// Игрок ушёл в наблюдатели и мы держим его в CS_TEAM_NONE. Нужен, чтобы отличить
	// «сам попросился в игру» (снимаем) от «команду сменил кто-то извне» (возвращаем).
	bool observing {};
	// Одноразовое подавление хука смены команды: перевод СПЕК→NONE делаем мы сами, и для
	// таймера/prac это не второй уход в наблюдатели — второй OnPausePost сдвинул бы
	// lastPauseTime и кулдаун паузы внутри рана.
	bool suppressTeamEvent {};
	// Не чаще одного возврата в наблюдатели за этот интервал: если движок (mp_force_pick_time)
	// продолжит выдёргивать игрока, мы не уйдём с ним в покадровую драку.
	f32 nextRestoreTime {};
	// Возвраты, идущие ПОДРЯД (серия рвётся паузой, см. KZ_INVISIBLE_RESTORE_STREAK).
	// Считаем именно серию, а не сумму за сессию: ожидаемый источник форсов —
	// периодический mp_force_pick_time, и счётчик без окна выдохся бы ровно на нём,
	// молча сняв скрытность с игрока, который считает себя спрятанным.
	i32 restoreAttempts {};
	f32 lastRestoreTime {};

	// Перевести контроллер в команду team, не поднимая событие ухода в наблюдатели.
	// Цель наблюдения переживает переход: слепок снимается до ChangeTeam и возвращается
	// после, а если observer pawn пересоздаётся не синхронно — отложенным на кадр таймером.
	void SetObserverTeam(int team);

public:
	virtual void Reset() override;

	void OnPlayerConnect(u64 steamID64);
	// Чат-индикация невидимке. Эпоха ClientActive: язык/чат на более ранних эпохах
	// могут не доехать (QueryCvarValue асинхронный) — паттерн kz_player.cpp OnPlayerActive.
	void OnPlayerActive();
	// Пересчитать флаг по текущему списку; при смене статуса у игрока в игре — сказать ему
	// в чат и привести команду наблюдателя к новому статусу. Счётчик онлайна пересобирает
	// RefreshAllPlayers.
	void RefreshFlag();

	bool IsInvisible() const
	{
		return this->invisible;
	}

	// Мы увели игрока в наблюдатели и держим его в CS_TEAM_NONE. Отличать от «просто
	// сидит в NONE»: только что зашедший игрок тоже в NONE, но вход в наблюдатели
	// (пауза таймера, гашение худа) для него ещё не отрабатывал.
	bool IsObserving() const
	{
		return this->observing;
	}

	// Спрятать наблюдающего невидимку: CS_TEAM_SPECTATOR → CS_TEAM_NONE. Идемпотентно.
	// Зовётся из сторожа OnGameFrame (следующим кадром после ухода в наблюдатели) и из
	// RefreshFlag (игрока внесли в список, пока он уже наблюдал).
	void EnforceObserverTeam();

	// Игрок сам уходит в живую команду — наблюдение окончено, сторож его больше не
	// возвращает. Зовётся ЯВНО из обёрток (JoinTeam, !goto), а не выводится из хука
	// ChangeTeam: заход в CT/T идёт через движковый SwitchTeam, и полагаться на то, что
	// он поднимет наш хук, нельзя — иначе сторож утащил бы игрока обратно в наблюдатели.
	void OnObserveEnd()
	{
		this->observing = false;
		this->restoreAttempts = 0;
	}

	// Движковый хук смены команды. Возвращает true, если событие нужно проглотить.
	bool OnChangeTeamPost(i32 team);
	// Сторож: держит невидимок-наблюдателей в CS_TEAM_NONE и возвращает выдернутых.
	static void OnGameFrame();

	// Быстрый гейт горячих путей: есть ли невидимки онлайн.
	static bool HasOnlineInvisibles();

	static bool IsInvisible(KZPlayer *player);
	// Скрывать ли subject от viewer: subject невидим, а viewer — нет (и это не сам subject).
	static bool ShouldHideFrom(KZPlayer *subject, KZPlayer *viewer);
	static bool IsInvisibleSteamId(u64 steamID64);

	// Загрузка списка на плагин-лоаде.
	static void Init();
	// Late load: ResetPlayers() из AllPluginsLoaded обнуляет флаги, выставленные Init(), —
	// повторно применяем список к живым игрокам (и пересобираем счётчик онлайна).
	static void OnAllPluginsLoaded();
	// Перечитка на map start (node-agent мог обновить файл между картами).
	static void OnActivateServer();
	// source — для лога (plugin_load/map_start/rcon_reload). Отсутствующий файл = пустой список;
	// битый файл = текущий список не меняем (warn с reason).
	static void LoadFromFile(const char *source);
};
