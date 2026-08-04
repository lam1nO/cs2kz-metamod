#pragma once
#include "../kz.h"

// Режим невидимки (слежка за читерами): игрок из списка не передаётся другим клиентам
// (pawn/оружие в CheckTransmit), его звуки глушатся (PostEvent), он не виден в
// спектатор-списках и не анонсируется в чате при входе/выходе. Видят его только он сам
// и другие невидимки. Controller и скорборд не трогаем (v1).
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
	void OnPlayerFullyConnect();
	// Пересчитать флаг по текущему списку; при смене статуса у игрока в игре — сказать ему в чат.
	void RefreshFlag();

	bool IsInvisible() const
	{
		return this->invisible;
	}

	static bool IsInvisible(KZPlayer *player);
	// Скрывать ли subject от viewer: subject невидим, а viewer — нет (и это не сам subject).
	static bool ShouldHideFrom(KZPlayer *subject, KZPlayer *viewer);
	static bool IsInvisibleSteamId(u64 steamID64);

	// Загрузка списка на плагин-лоаде.
	static void Init();
	// Перечитка на map start (node-agent мог обновить файл между картами).
	static void OnActivateServer();
	// source — для лога (plugin_load/map_start/rcon_reload). Отсутствующий файл = пустой список;
	// битый файл = текущий список не меняем (warn с reason).
	static void LoadFromFile(const char *source);

	// Выкинуть из маски получателей всех, от кого невидимка-эмиттер должен быть скрыт
	// (паттерн FilterQuietClients, kz_quiet.cpp).
	static void FilterReceivers(const uint64 *clients, u32 emitterPlayerIndex);

	// Движковый выбор цели обзёрвера (цикл кнопками, авто-attach) не знает про
	// transmit-гейт: in-eye на непередаваемый pawn — крашеопасный класс (прецедент —
	// исключение спектируемой цели в KZQuietService::ShouldHideIndex). Каждый тик
	// снимаем обычных зрителей с невидимок: на следующую валидную цель или во free roam.
	static void OnGameFrame();
};
