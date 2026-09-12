/*
 * cyb_awr_info.cpp — команда `!awr` (`kz_awr`). Смысл, ключ и почему резолвов два — в
 * заголовке cyb_awr_info.h, повторять здесь нечего.
 *
 * Всё состояние команды живёт В ЗАХВАТАХ колбэков, а не в глобалях: на один `!awr` приходится
 * до трёх асинхронных шагов (резолв awr → резолв wr → ник из БД), и между ними игрок успевает
 * уйти, сменить курс и режим или пережить смену карты. Поэтому наружу из шага в шаг едут
 * CPlayerUserId (не указатель: он мог протухнуть) и уже ГОТОВЫЕ строки шапки, снятые в момент
 * команды, — иначе ответ описывал бы ключ, по которому его никто не спрашивал.
 *
 * Единственная глобаль — отсечка частоты по слоту: команда ходит в api, и держать её без
 * ограничения нельзя. Она вне игрового такта, только главный поток.
 */

#include "cyb_awr_info.h"

#include "cs2kz.h"
#include "kz/kz.h"
#include "kz/db/kz_db.h"
#include "kz/language/kz_language.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/replays/cyb_replay_download.h"
#include "kz/timer/kz_timer.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include <string>
#include <vector>

// Пауза между `!awr` одного игрока. Команда — это два запроса в api и возможный запрос в БД;
// без отсечки спам в чат превращается в спам по сети. Три секунды: ответ обычно приходит
// быстрее, то есть нормальному пользователю отсечка не мешает.
#define KZ_AWR_COOLDOWN_SEC 3.0

namespace
{
	using CybAwrInfo::Outcome;

	// Момент, раньше которого `!awr` этому слоту не отвечаем. Индекс — слот, а не userID:
	// userID у каждого подключения свой и в массив не ложится. Реконнект на тот же слот
	// наследует остаток паузы — три секунды, цена вопроса нулевая.
	f64 g_awrNextAllowed[MAXPLAYERS + 1] {};

	// Шапка ответа, снятая в момент команды. Строки, а не указатели на дескрипторы: курс мог
	// исчезнуть вместе со сменой карты, пока летел ответ.
	struct AwrHeader
	{
		std::string map;
		std::string course;
		std::string mode;
	};

	// Часы движка на смене карты ОБНУЛЯЮТСЯ (известная грабля форка с persistent-состоянием),
	// поэтому отметку «из будущего» трактуем как рестарт часов и паузу снимаем. Иначе после
	// смены карты команда молчала бы у всех, кто успел позвать её на прошлой.
	bool AwrCooldownPassed(KZPlayer *player)
	{
		const i32 slot = player->GetPlayerSlot().Get();
		if (slot < 0 || slot > MAXPLAYERS)
		{
			return true;
		}
		const f64 now = g_pKZUtils->GetServerGlobals() ? g_pKZUtils->GetServerGlobals()->realtime : 0.0;
		if (now < g_awrNextAllowed[slot] - KZ_AWR_COOLDOWN_SEC)
		{
			g_awrNextAllowed[slot] = 0.0;
		}
		if (now < g_awrNextAllowed[slot])
		{
			return false;
		}
		g_awrNextAllowed[slot] = now + KZ_AWR_COOLDOWN_SEC;
		return true;
	}

	// Шапка строится по ТОМУ ЖЕ ключу, который уйдёт в резолв: курс разбирается ОБРАТНО из
	// cyber-номера (GetCourseByCyberNumber), а не берётся у таймера напрямую. Номер — это и есть
	// то, по чему api ищет запись; если курса с таким номером на карте нет (ключ 0 без курсов),
	// показываем сам номер, а не выдумываем имя.
	AwrHeader BuildHeader(KZPlayer *player)
	{
		AwrHeader header;
		header.map = g_pKZUtils->GetCurrentMapName().Get();

		const i32 courseNumber = KZ::course::GetCyberCourseNumber(player->timerService->GetCourse());
		const KZCourseDescriptor *course = KZ::course::GetCourseByCyberNumber(courseNumber);
		const CUtlString courseName = course ? course->GetName() : CUtlString();
		header.course = (courseName.Get() && courseName.Get()[0]) ? courseName.Get() : std::to_string(courseNumber);

		auto modeInfo = KZ::mode::GetModeInfo(player->modeService);
		header.mode = std::string(modeInfo.shortModeName.Get(), modeInfo.shortModeName.Length());
		return header;
	}

	void PrintAwrTime(KZPlayer *player, const AwrHeader &header, u64 awrMs, const char *holderName)
	{
		// Точность как у !pb (до миллисекунд): AWR сравнивают с личным временем, и десятых тут
		// мало. Сам !rpinfo печатает AWR грубее намеренно — там строка про кадр, не про рекорд.
		CUtlString time = utils::FormatTime((f64)awrMs / 1000.0);
		player->languageService->PrintChat(true, false, "AWR Header", header.map.c_str(), header.course.c_str(), header.mode.c_str());
		player->languageService->PrintChat(true, false, "AWR Time", time.Get(), holderName);
	}

	// Ник держателя. Сначала смотрим, кто на сервере (ответ мгновенный и всегда актуальнее
	// таблицы), потом — общая Players. Ни то, ни другое не сработало — печатаем SteamID64:
	// «время без автора» хуже, чем время с числом вместо имени.
	void PrintWithHolderName(CPlayerUserId userID, const AwrHeader &header, u64 awrMs, u64 steamId64)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return; // спросивший ушёл — печатать некому
		}

		KZPlayer *holder = g_pKZPlayerManager->SteamIdToPlayer(steamId64);
		if (holder && holder->IsInGame())
		{
			PrintAwrTime(player, header, awrMs, holder->GetName());
			return;
		}

		const std::string steamIdText = std::to_string(steamId64);
		if (!KZDatabaseService::IsReady())
		{
			PrintAwrTime(player, header, awrMs, steamIdText.c_str());
			return;
		}

		auto onSuccess = [userID, header, awrMs, steamIdText](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *asker = g_pKZPlayerManager->ToPlayer(userID);
			if (!asker)
			{
				return;
			}
			// Пустой вектор запросов — теоретический случай (транзакция одна), но обращение по
			// индексу в него было бы UB, а цена проверки нулевая.
			ISQLResult *result = (!queries.empty() && queries[0]) ? queries[0]->GetResultSet() : nullptr;
			const char *alias = nullptr;
			if (result && result->GetRowCount() > 0 && result->FetchRow())
			{
				alias = result->GetString(0);
			}
			PrintAwrTime(asker, header, awrMs, (alias && alias[0]) ? alias : steamIdText.c_str());
		};
		auto onFailure = [userID, header, awrMs, steamIdText](std::string, int)
		{
			KZPlayer *asker = g_pKZPlayerManager->ToPlayer(userID);
			if (asker)
			{
				// БД отказала — это не повод проглотить ответ: время у нас есть, автор будет
				// числом. Саму ошибку запроса печатает слой БД.
				PrintAwrTime(asker, header, awrMs, steamIdText.c_str());
			}
		};
		KZDatabaseService::FindAliasBySteamID64(steamId64, onSuccess, onFailure);
	}

	void PrintOutcome(CPlayerUserId userID, const AwrHeader &header, Outcome outcome, u64 awrMs, u64 steamId64)
	{
		if (outcome == Outcome::Found && steamId64 != 0)
		{
			PrintWithHolderName(userID, header, awrMs, steamId64);
			return;
		}

		KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
		if (!player)
		{
			return;
		}
		switch (outcome)
		{
			case Outcome::Found:
				// Время есть, а держателя в ответе нет — контракт api такого не допускает
				// (steamId64 обязателен), поэтому молча подставлять «кто-то» нельзя: показываем
				// отказ, а строку в лог уже написал резолв.
				player->languageService->PrintChat(true, false, "AWR - Request Failed");
				break;
			case Outcome::NotComputed:
				player->languageService->PrintChat(true, false, "AWR - Not Computed");
				break;
			case Outcome::NoRecord:
				player->languageService->PrintChat(true, false, "AWR - No Replays");
				break;
			case Outcome::Unavailable:
				player->languageService->PrintChat(true, false, "AWR - Request Failed");
				break;
		}
	}
} // namespace

SCMD(kz_awr, SCFL_RECORD | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	if (!AwrCooldownPassed(player))
	{
		// Молча: отсечка защищает сеть, а не воспитывает игрока, и своя фраза на неё
		// превратила бы спам командой в спам ответами.
		return MRES_SUPERCEDE;
	}

	const AwrHeader header = BuildHeader(player);

	CybReplayDownload::RequestInfo(
		player, CybReplayDownload::Kind::AWR, 0,
		[header](CPlayerUserId userID, CybReplayDownload::Info awrInfo)
		{
			if (awrInfo.status == -1)
			{
				KZPlayer *asker = g_pKZPlayerManager->ToPlayer(userID);
				if (asker)
				{
					asker->languageService->PrintChat(true, false, "AWR - Unavailable Here");
				}
				return;
			}
			if (awrInfo.status != 404)
			{
				// Один резолв всё решил: либо нашли, либо наша сторона не смогла спросить.
				//
				// Отдельно отбиваем «2xx, но тело нечитаемо»: времени в таком ответе нет, а
				// Classify по нулевому awrMs сказал бы «ещё не посчитан» — то есть выдал бы наш
				// отказ за состояние платформы. Сам отказ уже в логе (info_resolve_failed).
				const bool trustworthy = awrInfo.status < 200 || awrInfo.status >= 300 || awrInfo.bodyUsable;
				const CybAwrInfo::Outcome outcome =
					trustworthy ? CybAwrInfo::Classify(awrInfo.status, awrInfo.awrMs > 0, 0) : CybAwrInfo::Outcome::Unavailable;
				PrintOutcome(userID, header, outcome, awrInfo.awrMs, awrInfo.steamId64);
				return;
			}

			// 404 по awr: уточняем вторым резолвом, есть ли на ключе вообще файлы.
			KZPlayer *asker = g_pKZPlayerManager->ToPlayer(userID);
			if (!asker)
			{
				return; // спросивший ушёл — второй запрос в api не нужен никому
			}
			CybReplayDownload::RequestInfo(asker, CybReplayDownload::Kind::WR, 0,
										   [header](CPlayerUserId uid, CybReplayDownload::Info wrInfo)
										   {
											   // Смотрим ТОЛЬКО статус: вопрос к этому запросу
											   // один — «есть ли на ключе хоть один файл». Тело
											   // (держатель, awrMs) относится к чужой записи и
											   // на ответ игроку не влияет.
											   //
											   // Второй резолв мог не состояться по построению
											   // (-1) — тогда «записей нет» утверждать нечем,
											   // и Classify отдаст Unavailable.
											   PrintOutcome(uid, header, CybAwrInfo::Classify(404, false, wrInfo.status), 0, 0);
										   });
		});
	return MRES_SUPERCEDE;
}
