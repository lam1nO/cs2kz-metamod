#pragma once
#include "../kz.h"

// Режим невидимки (слежка за читерами), скоуп v2: скрытие из TAB (controller в
// CheckTransmit, только пока невидимка — спектатор) и из !specs, тихий вход/выход
// (анонсы подавлены). Модель/звуки НЕ скрываем — невидимка обязан быть спектатором,
// живым он виден как обычный игрок. Видят его только он сам и другие невидимки.
// Источник списка: csgo/cfg/cyb_invisible.json ({"steamids": ["7656119...", ...]}),
// живое управление — ConCommand'ы kz_invisible_reload/add/remove (только сервер/RCON).
class KZInvisibleService : public KZBaseService
{
	using KZBaseService::KZBaseService;

	// steamid64 эпохи OnClientConnect (xuid): известен с первой секунды, до аутентификации.
	u64 steamId64 {};
	bool invisible {};

public:
	virtual void Reset() override;

	void OnPlayerConnect(u64 steamID64);
	// Чат-индикация невидимке. Эпоха ClientActive: язык/чат на более ранних эпохах
	// могут не доехать (QueryCvarValue асинхронный) — паттерн kz_player.cpp OnPlayerActive.
	void OnPlayerActive();
	// Пересчитать флаг по текущему списку; при смене статуса у игрока в игре — сказать ему
	// в чат. Возвращает true, если игрок В ИГРЕ стал видимым (вызывающему нужен один
	// сетевой full update зрителям). Счётчик онлайна пересобирает RefreshAllPlayers.
	bool RefreshFlag();

	bool IsInvisible() const
	{
		return this->invisible;
	}

	// Быстрый гейт горячего пути (CheckTransmit): есть ли невидимки онлайн.
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
