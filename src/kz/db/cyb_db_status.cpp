// [cyb] НАШ файл, апстрима здесь нет. См. cyb_db_status.h.
#include "cyb_db_status.h"
#include "kz/db/kz_db.h"
#include "kz/option/kz_option.h"
#include "utils/utils.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include <atomic>
#include <chrono>

#include "tier0/memdbgon.h"

using namespace KZ::Database;

// Состояние глобальное, а не per-player, СОЗНАТЕЛЬНО: это состояние процесса (одна БД на
// инстанс). В хуках движения не участвует — пишется при настройке БД и в колбэках зонда,
// читается только из rcon-команды.
static_global u32 s_appliedMigrations = 0;
static_global u32 s_knownMigrations = 0;
static_global bool s_migrationCheckRan = false;

// Зонд живой связи с базой. Зачем он вообще нужен: KZDatabaseService::IsReady() — это
// localDBConnected, который выставляется ОДИН раз в onSuccess после сверки миграций и не
// сбрасывается нигде, включая Cleanup(). То есть упавшая уже ПОСЛЕ старта MySQL даёт ровно
// симптом инцидента 09-10.09.2026 (раны не сохраняются), а IsReady() при этом продолжает
// отвечать true. Инвариант, построенный на нём, светил бы ok на мёртвой базе.
// Поэтому команда печатает результат ПОСЛЕДНЕГО зонда (SELECT 1) и тут же запускает
// следующий: инвариант опрашивает раз в 5 минут, значит он всегда видит состояние не
// старше своего прошлого прогона, а не «когда-то в прошлом подключились».
// Колбэки sql_mm приходят не из этого вызова — флаги атомарные.
#define KZ_DB_PROBE_STUCK_SECONDS 60.0

enum class ProbeState : int
{
	None = 0, // зонда ещё не было (первый прогон после старта)
	Ok,
	Fail,
};

static_global std::atomic<int> s_probeState {(int)ProbeState::None};
static_global std::atomic<bool> s_probeInFlight {false};
// Секунды от старта процесса (steady_clock): часы движка обнуляются на смене карты, и
// «возраст» по ним врал бы ровно так же, как врали persistent-таймеры форка.
static_global std::atomic<double> s_probeResultAt {0.0};
static_global std::atomic<double> s_probeStartedAt {0.0};

static f64 NowSeconds()
{
	static_persist const auto origin = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count();
}

void KZ::cyb::db::NoteMigrationCheck(u32 applied, u32 known)
{
	s_appliedMigrations = applied;
	s_knownMigrations = known;
	s_migrationCheckRan = true;
}

// Запустить зонд, если предыдущий уже завершился. Стоимость — один SELECT 1 раз в 5 минут
// на инстанс (частота опроса инвариантом), фоновым потоком sql_mm.
static void StartProbe()
{
	bool expected = false;
	if (!s_probeInFlight.compare_exchange_strong(expected, true))
	{
		return; // предыдущий зонд ещё не вернулся — это само по себе сигнал, см. ниже
	}
	s_probeStartedAt = NowSeconds();
	ISQLConnection *connection = KZDatabaseService::GetDatabaseConnection();
	if (!connection)
	{
		// Соединения нет вовсе (нет конфига, не подключились, порвали после отказа
		// миграций) — это честный отрицательный результат, а не «зонд не удался».
		s_probeState = (int)ProbeState::Fail;
		s_probeResultAt = NowSeconds();
		s_probeInFlight = false;
		return;
	}
	Transaction txn;
	txn.queries.push_back("SELECT 1");
	connection->ExecuteTransaction(
		txn,
		[](std::vector<ISQLQuery *> queries)
		{
			s_probeState = (int)ProbeState::Ok;
			s_probeResultAt = NowSeconds();
			s_probeInFlight = false;
		},
		[](std::string error, int failIndex)
		{
			s_probeState = (int)ProbeState::Fail;
			s_probeResultAt = NowSeconds();
			s_probeInFlight = false;
			KZ_LOG_WARN(LogChannel::DB, "[cyb] db_probe_failed reason=query_failed index=%i error=%s\n", failIndex, error.c_str());
		});
}

// Драйвер берём из КОНФИГА, а не из KZDatabaseService::GetDatabaseType(): тип зануляется в
// SQLite (None = -1 и не присваивается нигде), а при отсутствующей секции `db`
// SetupDatabase уходит в ранний return, не трогая тип вовсе — то есть «конфига БД нет»
// печаталось бы как driver=sqlite, и оператора отправляли бы чинить MySQL вместо профиля.
static const char *ConfiguredDriver()
{
	KeyValues *config = KZOptionService::GetOptionKV("db");
	if (!config)
	{
		return "none";
	}
	const char *driver = config->GetString("driver", "");
	if (!driver || !driver[0])
	{
		return "unset";
	}
	if (!V_stricmp(driver, "mysql"))
	{
		return "mysql";
	}
	if (!V_stricmp(driver, "sqlite"))
	{
		return "sqlite";
	}
	// Чужое значение печатаем одним словом: строка разбирается по парам ключ=значение, и
	// пробел из конфига сломал бы разбор у читателя.
	return "other";
}

CON_COMMAND_F(kz_db_status, "Print the local database state of the plugin (machine readable, used by the kz_db_ready invariant).", FCVAR_NONE)
{
	// Игрокам не отдаём, как и прочие настоящие ConCommand форка (канон — kz_invisible.cpp).
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::DB, "[cyb] cmd_denied cmd=kz_db_status reason=not_server slot=%d\n", context.GetPlayerSlot().Get());
		return;
	}
	f64 now = NowSeconds();
	ProbeState state = (ProbeState)s_probeState.load();
	const char *probe = "none";
	bool live = false;
	f64 age = 0.0;
	if (s_probeInFlight.load() && now - s_probeStartedAt.load() > KZ_DB_PROBE_STUCK_SECONDS)
	{
		// Зонд ушёл и не вернулся: соединение висит. Снаружи это неотличимо от здорового
		// инстанса, а раны в это время не сохраняются — отдельное состояние, не "fail".
		probe = "stuck";
		age = now - s_probeStartedAt.load();
	}
	else if (state == ProbeState::Ok)
	{
		probe = "ok";
		live = true;
		age = now - s_probeResultAt.load();
	}
	else if (state == ProbeState::Fail)
	{
		probe = "fail";
		age = now - s_probeResultAt.load();
	}
	// ready — ТЕКУЩАЯ связь (последний зонд прошёл), setup — исторический IsReady()
	// (настройка БД однажды удалась). Разводить обязательно: инцидент как раз про то, что
	// второе остаётся true, когда первого уже нет.
	// checked=0 значит, что до сверки миграций дело не дошло вовсе; applied/known тогда нули,
	// а не «миграций ноль». ahead>0 — на базе побывала сборка новее этой.
	u32 ahead = s_appliedMigrations > s_knownMigrations ? s_appliedMigrations - s_knownMigrations : 0;
	Msg("[cyb] db_status ready=%d setup=%d checked=%d driver=%s applied=%u known=%u ahead=%u probe=%s probe_age=%d\n",
		(live && KZDatabaseService::IsReady()) ? 1 : 0, KZDatabaseService::IsReady() ? 1 : 0, s_migrationCheckRan ? 1 : 0,
		ConfiguredDriver(), s_appliedMigrations, s_knownMigrations, ahead, probe, (int)age);
	// Зонд для СЛЕДУЮЩЕГО опроса запускаем после печати: команда обязана отвечать сразу,
	// а не ждать базу (её отказ — это таймаут rcon, то есть unknown вместо нарушения).
	StartProbe();
}
