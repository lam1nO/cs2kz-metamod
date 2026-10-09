#include "jumptop.h"

#include "cs2kz.h"

#include "kz/kz.h"
#include "kz/db/kz_db.h"
#include "kz/db/queries/jumptop.h"
#include "kz/mode/kz_mode.h"
#include "kz/language/kz_language.h"
#include "kz/anticheat/kz_anticheat.h"
#include "kz/option/kz_option.h"
#include "kz/recording/kz_recording.h"
#include "kz/replays/kz_replay.h"
#include "kz/replays/commands.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"
#include "utils/json.h"
#include "utils/tables.h"

#include "filesystem.h"
#include "vendor/sql_mm/src/public/sql_mm.h"
#include <vendor/mm-cs2menus/src/public/ics2menus.h>
#include <ixwebsocket/IXBase64.h>

#include <cstdlib>
#include <deque>
#include <string>
#include <unordered_map>

using namespace KZ::Database;

extern ICS2Menus *g_pMenus;

// Порядок режимов в меню и в разборе аргумента !jumptop <режим>.
static_global const char *const jumptopModes[] = {"CKZ", "KZT", "VNL"};

/*
 * Кэш личных рекордов игрока (по слоту). Грузится на OnClientSetup; пока не загружен, прыжки
 * в топ не идут вовсе: без кэша каждый прыжок считался бы PB и улетал бы в базу.
 */
namespace
{
	struct Best
	{
		i32 block;
		f64 distance;
	};

	struct PlayerCache
	{
		u64 steamID64 {};
		bool loaded {};
		std::unordered_map<u32, Best> bests;
	};

	PlayerCache g_cache[MAXPLAYERS + 1];

	u32 CacheKey(i32 modeID, i32 jumpType, bool isBlock)
	{
		return ((u32)modeID << 16) | ((u32)jumpType << 8) | (isBlock ? 1u : 0u);
	}

	PlayerCache *GetCache(KZPlayer *player)
	{
		i32 slot = player->GetPlayerSlot().Get();
		if (slot < 0 || slot > MAXPLAYERS)
		{
			return nullptr;
		}
		PlayerCache *cache = &g_cache[slot];
		if (!cache->loaded || cache->steamID64 != player->GetSteamId64())
		{
			return nullptr;
		}
		return cache;
	}

	bool IsMySQL()
	{
		return KZDatabaseService::GetDatabaseType() == DatabaseType::MySQL;
	}

	f64 ResultDouble(ISQLResult *result, u32 column)
	{
		const char *str = result->GetString(column);
		return str ? atof(str) : 0.0;
	}

	const char *ServerID()
	{
		// Тот же id инстанса, что агент кладёт в контейнер (X-Server-Id у cyber-presence).
		const char *id = getenv("CS2_INSTANCE_ID");
		return (id && id[0]) ? id : "unknown";
	}

	std::string ModeShortName(i32 modeID)
	{
		auto info = KZ::mode::GetModeInfoFromDatabaseID(modeID);
		return info.databaseID >= 0 ? info.shortModeName.Get() : "?";
	}

	// «LJ 265.1234» или «LJ 260 блок (265.1234)» — общая подпись прыжка для чата и меню.
	std::string JumpLabel(const char *lang, i32 jumpType, bool isBlock, i32 block, f64 distance)
	{
		const char *type = (jumpType >= 0 && jumpType < JUMPTYPE_COUNT) ? jumpTypeShortStr[jumpType] : "?";
		char buf[128];
		if (isBlock)
		{
			std::string blockWord = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Block Word");
			V_snprintf(buf, sizeof(buf), "%s %d %s (%.4f)", type, block, blockWord.c_str(), distance);
		}
		else
		{
			V_snprintf(buf, sizeof(buf), "%s %.4f", type, distance);
		}
		return buf;
	}

	bool UsedTurnbinds(Jump *jump)
	{
		FOR_EACH_VEC(jump->strafes, i)
		{
			Strafe &strafe = jump->strafes[i];
			FOR_EACH_VEC(strafe.aaCalls, j)
			{
				u64 *buttons = strafe.aaCalls[j].buttons;
				if (CInButtonState::IsButtonPressed(buttons, IN_TURNLEFT, true) || CInButtonState::IsButtonPressed(buttons, IN_TURNRIGHT, true))
				{
					return true;
				}
			}
		}
		return false;
	}

	nlohmann::json VecJson(const Vector &v)
	{
		return nlohmann::json::array({v.x, v.y, v.z});
	}

	// Полная статистика прыжка для разбора на АХК. Всё, что видно в консольном отчёте, плюс
	// стрейфы поштучно. Посубтиковые данные (вызовы AirAccelerate, кнопки) — в реплее прыжка.
	std::string BuildDetails(KZPlayer *player, Jump *jump, const char *replayUuid)
	{
		nlohmann::json d;
		d["v"] = 1;
		d["plugin"] = g_KZPlugin.GetVersion();
		d["map"] = g_pKZUtils->GetCurrentMapName().Get();
		d["server"] = ServerID();
		d["replay"] = replayUuid;
		d["mode"] = KZ::mode::GetModeInfo(player->modeService).shortModeName.Get();
		d["jumpType"] = jump->GetJumpType();
		d["originalJumpType"] = jump->originalJumpType;
		d["distance"] = jump->GetDistance();
		d["block"] = jump->GetBlock();
		d["miss"] = jump->GetMiss();
		d["edge"] = jump->GetEdge(false);
		d["landingEdge"] = jump->GetEdge(true);
		d["offset"] = jump->GetOffset();
		d["height"] = jump->GetMaxHeight();
		d["pre"] = jump->GetTakeoffSpeed();
		d["max"] = jump->GetMaxSpeed();
		d["airtime"] = jump->airtime;
		d["strafes"] = jump->GetStrafeCount();
		d["sync"] = jump->GetSync();
		d["badAngles"] = jump->GetBadAngles();
		d["overlap"] = jump->GetOverlap();
		d["deadAir"] = jump->GetDeadAir();
		d["width"] = jump->GetWidth();
		d["gainEff"] = jump->GetGainEfficiency();
		d["airPath"] = jump->GetAirPath();
		d["deviation"] = jump->GetDeviation();
		d["releaseTicks"] = jump->GetReleaseInTick();
		d["duckEnd"] = jump->GetDuckTime(true);
		d["duck"] = jump->GetDuckTime(false);
		d["hitHead"] = jump->DidHitHead();
		d["touchDuration"] = jump->touchDuration;
		d["takeoffOrigin"] = VecJson(jump->takeoffOrigin);
		d["adjustedTakeoffOrigin"] = VecJson(jump->adjustedTakeoffOrigin);
		d["takeoffVelocity"] = VecJson(jump->takeoffVelocity);
		d["landingOrigin"] = VecJson(jump->landingOrigin);
		d["adjustedLandingOrigin"] = VecJson(jump->adjustedLandingOrigin);
		d["serverTick"] = jump->serverTick;

		nlohmann::json strafes = nlohmann::json::array();
		FOR_EACH_VEC(jump->strafes, i)
		{
			Strafe &s = jump->strafes[i];
			nlohmann::json js;
			js["duration"] = s.GetStrafeDuration();
			js["sync"] = s.GetSync();
			js["syncDuration"] = s.GetSyncDuration();
			js["badAngles"] = s.GetBadAngleDuration();
			js["overlap"] = s.GetOverlapDuration();
			js["deadAir"] = s.GetDeadAirDuration();
			js["width"] = s.GetWidth();
			js["gain"] = s.GetGain();
			js["loss"] = s.GetLoss();
			js["maxGain"] = s.GetMaxGain();
			js["externalGain"] = s.GetGain(true);
			js["externalLoss"] = s.GetLoss(true);
			js["maxSpeed"] = s.GetStrafeMaxSpeed();
			js["aaCalls"] = s.aaCalls.Count();
			if (s.arStats.available)
			{
				js["arMax"] = s.arStats.max;
				js["arMedian"] = s.arStats.median;
				js["arAverage"] = s.arStats.average;
			}
			strafes.push_back(js);
		}
		d["strafeList"] = strafes;
		// График клавиш и мыши — тот же, что в обычном отчёте в консоли (для разбора на АХК).
		std::string strafeLeft, strafeRight, mouseLeft, mouseRight;
		if (jump->BuildConsoleStrafeMouseGraph(strafeLeft, strafeRight, mouseLeft, mouseRight))
		{
			d["graph"] = {{"strafeLeft", strafeLeft}, {"strafeRight", strafeRight}, {"mouseLeft", mouseLeft}, {"mouseRight", mouseRight}};
		}

		// Битый UTF-8 (имя карты) не должен уронить сервер: исключения выключены, throw = abort.
		return d.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	void InsertReplayRow(const std::string &uuidStr, const std::vector<char> &buffer)
	{
		if (!KZDatabaseService::IsReady())
		{
			KZ_LOG_WARN(LogChannel::DB, "[cyb] jumptop_replay_store_failed reason=db_not_ready replay=%s\n", uuidStr.c_str());
			return;
		}
		std::string encoded = macaron::Base64::Encode(std::string(buffer.begin(), buffer.end()));
		std::string query = IsMySQL() ? mysql_jumptop_replay_insert : sqlite_jumptop_replay_insert;
		// Шаблон с двумя %s: base64 и UUID в кавычках не нуждаются в экранировании, а буфер в
		// сотни КБ через V_snprintf не гоняем — собираем строку подстановкой.
		size_t first = query.find("%s");
		query.replace(first, 2, uuidStr);
		size_t second = query.find("%s", first + uuidStr.size());
		query.replace(second, 2, encoded);

		Transaction txn;
		txn.queries.push_back(query);
		size_t bytes = buffer.size();
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[uuidStr, bytes](std::vector<ISQLQuery *> queries)
			{
				KZ_LOG_INFO(LogChannel::DB, "[cyb] jumptop_replay_stored replay=%s bytes=%zu\n", uuidStr.c_str(), bytes);
			},
			[uuidStr](std::string error, int failIndex)
			{
				KZ_LOG_ERROR(LogChannel::DB, "[cyb] jumptop_replay_store_failed reason=db replay=%s error=%s\n", uuidStr.c_str(), error.c_str());
			});
		// clang-format on
	}

	/*
	 * Гейт реплея: в базу уходит только реплей прыжка, попавшего в топ. Вердикт (место из базы) и
	 * буфер (рекордер останавливается через 2 с после прыжка) приходят в любом порядке — кто
	 * второй, тот и пишет. Обе очереди ограничены: прыжок вне топа оставляет буфер, который
	 * никто не заберёт, а вердикт без буфера бывает, если рекордер потерян (выход до записи).
	 */
	constexpr size_t maxPendingReplays = 16;
	std::deque<std::pair<std::string, std::vector<char>>> g_pendingReplays;
	std::deque<std::string> g_qualifiedReplays;
	// UUID прыжков, удалённых из админки (kz_jumptop_forget): буфер их реплея может прийти уже ПОСЛЕ
	// удаления (рекордер останавливается через 2 с после прыжка) — тогда его не пишем. Ограничено.
	std::deque<std::string> g_forgottenReplays;

	bool IsForgotten(const std::string &uuid)
	{
		for (auto &f : g_forgottenReplays)
		{
			if (f == uuid)
			{
				return true;
			}
		}
		return false;
	}

	void QualifyReplay(const std::string &uuid)
	{
		if (IsForgotten(uuid))
		{
			return;
		}
		for (auto it = g_pendingReplays.begin(); it != g_pendingReplays.end(); ++it)
		{
			if (it->first == uuid)
			{
				InsertReplayRow(uuid, it->second);
				g_pendingReplays.erase(it);
				return;
			}
		}
		for (auto &q : g_qualifiedReplays)
		{
			if (q == uuid)
			{
				return;
			}
		}
		g_qualifiedReplays.push_back(uuid);
		while (g_qualifiedReplays.size() > maxPendingReplays)
		{
			g_qualifiedReplays.pop_front();
		}
	}

	void OfferReplayBuffer(const std::string &uuid, const std::vector<char> &buffer)
	{
		if (IsForgotten(uuid))
		{
			return;
		}
		for (auto it = g_qualifiedReplays.begin(); it != g_qualifiedReplays.end(); ++it)
		{
			if (*it == uuid)
			{
				g_qualifiedReplays.erase(it);
				InsertReplayRow(uuid, buffer);
				return;
			}
		}
		g_pendingReplays.emplace_back(uuid, buffer);
		while (g_pendingReplays.size() > maxPendingReplays)
		{
			g_pendingReplays.pop_front();
		}
	}

	// Вставка строки PB и сразу место в топе — одной транзакцией.
	// Место в топе и объявление — по базе ПОСЛЕ коммита вставки. Не в той же транзакции: там
	// чтение шло бы по снимку, и две почти одновременные вставки на разных серверах (284 и 283)
	// не видели бы друг друга — WR засчитался бы обоим. После коммита второй сервер видит
	// чужую строку. Кэш «WR на момент захода» не используется вовсе — рекорд спрашивается у
	// общей базы на каждом PB.
	void AnnouncePB(CPlayerUserId userID, const std::string &name, u64 steamID64, i32 modeID, i32 jumpType, bool isBlock, i32 block,
					f64 distance, const std::string &replayUuid, const std::string &mapName, u32 id)
	{
		if (!KZDatabaseService::IsReady())
		{
			return;
		}
		char rank[2048];
		V_snprintf(rank, sizeof(rank), sql_jumptop_rank, modeID, jumpType, isBlock ? 1 : 0, block, block, distance, distance, id, steamID64, modeID, jumpType,
				   isBlock ? 1 : 0);
		Transaction txn;
		txn.queries.push_back(rank);
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[userID, name, steamID64, modeID, jumpType, isBlock, block, distance, replayUuid, mapName, id](std::vector<ISQLQuery *> queries)
			{
				i32 place = 0;
				i32 total = 0;
				ISQLResult *result = queries[0]->GetResultSet();
				if (result && result->FetchRow())
				{
					place = result->GetInt(0) + 1;
					total = result->GetInt(1);
				}
				KZ_LOG_INFO(LogChannel::DB, "[cyb] jumptop_pb id=%u steam_id=%llu mode=%d type=%d block=%d dist=%.4f map=%s place=%d total=%d\n", id,
							steamID64, modeID, jumpType, isBlock ? block : 0, distance, mapName.c_str(), place, total);

				// Реплей в базу — только прыжку из топа: PB новичка обновляется каждые несколько прыжков,
				// и хранить реплей каждого значило бы бесконечно растить общую MySQL.
				if (place >= 1 && place <= KZ::jumptop::topCount)
				{
					QualifyReplay(replayUuid);
				}

				std::string mode = ModeShortName(modeID);
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				// Античит мог забанить за этот же прыжок, пока шёл запрос: рекорд не объявляем
				// (из топа строку уберёт фильтр по Bans).
				if (player && player->anticheatService->isBanned)
				{
					return;
				}
				// Строка PB — как в GOKZ. Самому игроку всегда; остальным — если у них включён преф
				// jsBroadcastPB (дефолт on, «Джампстаты» и «Игра → Сообщения» в !options). На WR остальным
				// уходит только строка WR ниже, без дубля PB.
				for (i32 i = 0; i <= MAXPLAYERS; i++)
				{
					KZPlayer *target = g_pKZPlayerManager->ToPlayer(i);
					if (!target || !target->IsInGame() || target->IsFakeClient() || target->IsCSTV())
					{
						continue;
					}
					bool self = target == player;
					if (!self && (place == 1 || !target->optionService->GetPreferenceBool("jsBroadcastPB", true)))
					{
						continue;
					}
					if (isBlock)
					{
						target->languageService->PrintChat(true, false, "Jumptop - New Block PB", name.c_str(), block, jumpTypeStr[jumpType], distance,
														   mode.c_str());
					}
					else
					{
						target->languageService->PrintChat(true, false, "Jumptop - New PB", name.c_str(), jumpTypeStr[jumpType], distance, mode.c_str());
					}
				}
				if (place != 1)
				{
					return;
				}
				// Текст как у WR рана (submission.cpp): «поставил новый WORLD RECORD». Без звука —
				// решение пользователя 08.10: рекорд прыжка объявляется только текстом.
				for (i32 i = 0; i <= MAXPLAYERS; i++)
				{
					KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
					if (!other || !other->IsInGame() || other->IsFakeClient() || other->IsCSTV())
					{
						continue;
					}
					std::string label = JumpLabel(other->languageService->GetLanguage(), jumpType, isBlock, block, distance);
					other->languageService->PrintChat(true, false, "Jumptop - New World Record", name.c_str(), label.c_str(), mapName.c_str(),
													  mode.c_str());
				}
			},
			[steamID64, jumpType](std::string error, int failIndex)
			{
				KZ_LOG_ERROR(LogChannel::DB, "[cyb] jumptop_rank_failed reason=db steam_id=%llu type=%d error=%s\n", steamID64, jumpType,
							 error.c_str());
			});
		// clang-format on
	}

	void SubmitPB(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock, i32 block, Jump *jump, const std::string &replayUuid,
				  const std::string &details)
	{
		ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
		std::string mapName = g_pKZUtils->GetCurrentMapName().Get();
		std::string map = db->Escape(mapName.c_str());
		std::string server = db->Escape(ServerID());
		std::string cleanDetails = db->Escape(details.c_str());
		u64 steamID64 = player->GetSteamId64();
		f64 distance = jump->GetDistance();

		std::vector<char> insert(cleanDetails.size() + map.size() + 1024);
		V_snprintf(insert.data(), (int)insert.size(), sql_jumptop_insert, steamID64, modeID, jumpType, isBlock ? 1 : 0, block, distance,
				   jump->GetStrafeCount(), (f64)jump->GetSync(), (f64)jump->GetTakeoffSpeed(), (f64)jump->GetMaxSpeed(), (f64)jump->airtime,
				   (f64)jump->GetMaxHeight(), (f64)jump->GetOffset(), map.c_str(), server.c_str(), replayUuid.c_str(), cleanDetails.c_str());

		Transaction txn;
		txn.queries.push_back(insert.data());

		CPlayerUserId userID = player->GetClient()->GetUserID();
		std::string name = player->GetName();
		// clang-format off
		db->ExecuteTransaction(
			txn,
			[userID, name, steamID64, modeID, jumpType, isBlock, block, distance, replayUuid, mapName](std::vector<ISQLQuery *> queries)
			{
				AnnouncePB(userID, name, steamID64, modeID, jumpType, isBlock, block, distance, replayUuid, mapName, queries[0]->GetInsertId());
			},
			[steamID64, jumpType](std::string error, int failIndex)
			{
				KZ_LOG_ERROR(LogChannel::DB, "[cyb] jumptop_pb_failed reason=db steam_id=%llu type=%d index=%d error=%s\n", steamID64, jumpType,
							 failIndex, error.c_str());
			});
		// clang-format on
	}

	void LoadPBs(Player *basePlayer, u64 steamID64)
	{
		KZPlayer *player = g_pKZPlayerManager->ToKZPlayer(basePlayer);
		if (!player || player->IsFakeClient() || player->IsCSTV() || !KZDatabaseService::IsReady())
		{
			return;
		}
		i32 slot = player->GetPlayerSlot().Get();
		if (slot < 0 || slot > MAXPLAYERS)
		{
			return;
		}
		g_cache[slot] = PlayerCache();

		char query[512];
		V_snprintf(query, sizeof(query), sql_jumptop_getpbs, steamID64);
		Transaction txn;
		txn.queries.push_back(query);

		CPlayerUserId userID = player->GetClient()->GetUserID();
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[userID, steamID64](std::vector<ISQLQuery *> queries)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (!player || player->GetSteamId64() != steamID64)
				{
					return;
				}
				i32 slot = player->GetPlayerSlot().Get();
				if (slot < 0 || slot > MAXPLAYERS)
				{
					return;
				}
				PlayerCache &cache = g_cache[slot];
				cache = PlayerCache();
				ISQLResult *result = queries[0]->GetResultSet();
				while (result && result->FetchRow())
				{
					i32 mode = result->GetInt(0);
					i32 type = result->GetInt(1);
					bool isBlock = result->GetInt(2) != 0;
					i32 block = result->GetInt(3);
					f64 distance = ResultDouble(result, 4);
					u32 key = CacheKey(mode, type, isBlock);
					auto it = cache.bests.find(key);
					if (it == cache.bests.end() || KZ::jumptop::IsBetter(isBlock, block, distance, it->second.block, it->second.distance))
					{
						cache.bests[key] = {block, distance};
					}
				}
				cache.steamID64 = steamID64;
				cache.loaded = true;
			},
			[steamID64](std::string error, int failIndex)
			{
				KZ_LOG_ERROR(LogChannel::DB, "[cyb] jumptop_pbs_load_failed steam_id=%llu error=%s\n", steamID64, error.c_str());
			});
		// clang-format on
	}

	class : public KZDatabaseServiceEventListener
	{
	public:
		virtual void OnClientSetup(Player *player, u64 steamID64, bool isBanned) override
		{
			LoadPBs(player, steamID64);
		}
	} databaseEventListener;
} // namespace

// Пороги jumptop_rules.h — копии констант форка (заголовок без SDK ради host-теста).
static_assert(KZ::jumptop::rules::typeLongJump == JumpType_LongJump);
static_assert(KZ::jumptop::rules::typeLadderJump == JumpType_LadderJump);
static_assert(KZ::jumptop::rules::typeJumpbug == JumpType_Jumpbug);
static_assert(KZ::jumptop::rules::minDistance == (float)JS_MIN_BLOCK_DISTANCE);
static_assert(KZ::jumptop::rules::minLadderDistance == (float)JS_MIN_LAJ_BLOCK_DISTANCE);
static_assert(KZ::jumptop::rules::offsetEpsilon == JS_EPSILON);

void KZ::jumptop::Init()
{
	KZDatabaseService::RegisterEventListener(&databaseEventListener);
}

void KZ::jumptop::OnJumpFinish(KZPlayer *player, Jump *jump)
{
	if (!player || player->IsFakeClient() || player->IsCSTV() || !KZDatabaseService::IsReady())
	{
		return;
	}
	if (!jump->IsValid() || jump->IsFailstat())
	{
		return;
	}
	i32 jumpType = jump->GetJumpType();
	f32 distance = jump->GetDistance();
	if (!PassesDistanceRules(jumpType, distance, jump->GetOffset()))
	{
		return;
	}
	// Стили меняют физику — их прыжки в общий топ не идут (у GOKZ топ без стилей).
	if (player->styleServices.Count() > 0 || player->anticheatService->isBanned)
	{
		return;
	}
	// Бинды поворота на нашем флоте разрешены (kz_allow_turnbinds), но в топ прыжки с ними не
	// идут: в GOKZ +left/+right инвалидирует прыжок, а cl_yawspeed даёт идеальный поворот.
	if (UsedTurnbinds(jump))
	{
		return;
	}
	i32 modeID = KZ::mode::GetModeInfo(player->modeService).databaseID;
	if (modeID < 0)
	{
		return;
	}
	PlayerCache *cache = GetCache(player);
	if (!cache)
	{
		return;
	}

	i32 block = (i32)jump->GetBlock();
	bool categories[2] = {true, block > 0};
	bool isPB[2] = {};
	for (i32 c = 0; c < 2; c++)
	{
		if (!categories[c])
		{
			continue;
		}
		bool isBlock = c == 1;
		i32 rowBlock = isBlock ? block : 0;
		u32 key = CacheKey(modeID, jumpType, isBlock);
		auto it = cache->bests.find(key);
		if (it == cache->bests.end() || IsBetter(isBlock, rowBlock, distance, it->second.block, it->second.distance))
		{
			isPB[c] = true;
			cache->bests[key] = {rowBlock, distance};
		}
	}
	if (!isPB[0] && !isPB[1])
	{
		return;
	}

	// Реплей прыжка: рекордер этого прыжка (создан KZRecordingService::OnJumpFinish только для
	// wrecker-тира) заводим/помечаем, и по остановке его буфер уйдёт в StoreReplay.
	UUID_t replayUuid = player->recordingService->GetLastJumpUUID();
	player->recordingService->MarkJumpForJumptop(replayUuid, jump);
	std::string uuidStr = replayUuid.ToString();
	std::string details = BuildDetails(player, jump, uuidStr.c_str());

	for (i32 c = 0; c < 2; c++)
	{
		if (isPB[c])
		{
			SubmitPB(player, modeID, jumpType, c == 1, c == 1 ? block : 0, jump, uuidStr, details);
		}
	}
}

void KZ::jumptop::StoreReplay(const UUID_t &uuid, const std::vector<char> &buffer)
{
	std::string uuidStr = uuid.ToString();
	// Прыжок уже удалён из админки (kz_jumptop_forget пришёл раньше буфера) — ни файла, ни базы.
	if (IsForgotten(uuidStr))
	{
		return;
	}
	// Локальная копия — чтобы !jumptop на этом же инстансе играл без похода в базу.
	char path[512];
	V_snprintf(path, sizeof(path), KZ_REPLAY_PATH "/%s.replay", uuidStr.c_str());
	utils::WriteBufferToFile(path, buffer);
	OfferReplayBuffer(uuidStr, buffer);
}

/*
 * Реплей прыжка: локальный файл (свой инстанс) либо base64 из базы → downloads/ → штатный плеер.
 */
static_function void PlayJumpReplay(KZPlayer *player, const std::string &uuid)
{
	UUID_t parsed;
	if (!UUID_t::FromString(uuid.c_str(), &parsed))
	{
		player->languageService->PrintChat(true, false, "Jumptop - No Replay");
		return;
	}
	std::string clean = parsed.ToString();
	char local[512];
	V_snprintf(local, sizeof(local), KZ_REPLAY_PATH "/%s.replay", clean.c_str());
	char downloaded[512];
	V_snprintf(downloaded, sizeof(downloaded), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", clean.c_str());
	if (g_pFullFileSystem->FileExists(local) || g_pFullFileSystem->FileExists(downloaded))
	{
		KZ::replaysystem::commands::LoadReplay(player, clean.c_str());
		return;
	}
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
		return;
	}

	char query[256];
	V_snprintf(query, sizeof(query), sql_jumptop_replay_fetch, clean.c_str());
	Transaction txn;
	txn.queries.push_back(query);
	CPlayerUserId userID = player->GetClient()->GetUserID();
	// clang-format off
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
		txn,
		[userID, clean](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (!player)
			{
				return;
			}
			ISQLResult *result = queries[0]->GetResultSet();
			if (!result || !result->FetchRow())
			{
				player->languageService->PrintChat(true, false, "Jumptop - No Replay");
				return;
			}
			size_t len = 0;
			const char *data = result->GetString(0, &len);
			std::string decoded;
			if (!data || !macaron::Base64::Decode(std::string(data, len), decoded).empty() || decoded.empty())
			{
				KZ_LOG_WARN(LogChannel::DB, "[cyb] jumptop_replay_bad_data replay=%s\n", clean.c_str());
				player->languageService->PrintChat(true, false, "Jumptop - No Replay");
				return;
			}
			char path[512];
			V_snprintf(path, sizeof(path), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", clean.c_str());
			if (!utils::WriteBufferToFile(path, std::vector<char>(decoded.begin(), decoded.end())))
			{
				player->languageService->PrintChat(true, false, "Jumptop - No Replay");
				return;
			}
			KZ::replaysystem::commands::LoadReplay(player, clean.c_str());
		},
		[userID](std::string error, int failIndex)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (player)
			{
				player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
			}
		});
	// clang-format on
}

/*
 * Полная статистика прыжка из базы — в формате обычного отчёта о прыжке в консоли
 * (KZJumpstatsService::PrintJumpToConsole): сводка, две строки деталей, таблица стрейфов, график.
 * Сверху строка джамптопа (номер, карта, сервер, дата), снизу SteamID и UUID реплея.
 */
struct StoredJump
{
	i32 id;
	u64 steamID64;
	std::string alias;
	i32 mode;
	i32 type;
	bool isBlock;
	i32 block;
	f64 distance;
	std::string map;
	std::string server;
	std::string replay;
	std::string created;
	bool removed;
	std::string details;
};

static_function f64 JsonNum(const nlohmann::json &j, const char *key, f64 def = 0.0)
{
	auto it = j.find(key);
	return (it != j.end() && it->is_number()) ? it->get<f64>() : def;
}

static_function std::string JsonStr(const nlohmann::json &j, const char *key)
{
	auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : "";
}

// Как Jump::GetReleaseString(false): отпускание W относительно отрыва, в тиках.
static_function std::string ReleaseString(f64 ticks)
{
	if (ticks < -20)
	{
		return "";
	}
	if (ticks > 10)
	{
		return " | ✗ W";
	}
	if (ticks == 0)
	{
		return " | ✓ W";
	}
	char buf[64];
	V_snprintf(buf, sizeof(buf), " | %s%.1f W", ticks > 0 ? "+" : "", ticks);
	return buf;
}

static_global const char *storedColumnKeys[] = {"#.",
												"Sync",
												"Gain",
												"",
												"Loss",
												"",
												"Max",
												"Air Time",
												"Bad Angles (Short)",
												"Overlap (Short)",
												"Dead Air (Short)",
												"Width (Short)",
												"Average Gain (Short)",
												"Gain Efficiency (Short)",
												"Angle Ratio"};

static_function void PrintStoredJumpReport(KZPlayer *target, const StoredJump &s)
{
	const char *lang = target->languageService->GetLanguage();
	const char *typeShort = (s.type >= 0 && s.type < JUMPTYPE_COUNT) ? jumpTypeShortStr[s.type] : "?";
	const char *typeLong = (s.type >= 0 && s.type < JUMPTYPE_COUNT) ? jumpTypeStr[s.type] : "?";
	std::string modeName = ModeShortName(s.mode);

	char head[512];
	char blockPart[32] = "";
	if (s.isBlock)
	{
		V_snprintf(blockPart, sizeof(blockPart), " · %d block", s.block);
	}
	V_snprintf(head, sizeof(head), "\n[Jumptop #%d] %s%s · %s · %s · %s · %s%s\n", s.id, typeShort, blockPart, modeName.c_str(), s.map.c_str(),
			   s.server.c_str(), s.created.c_str(), s.removed ? " · REMOVED" : "");
	utils::PrintConsole(target->GetController(), "%s", head);

	nlohmann::json d = nlohmann::json::parse(s.details, nullptr, false);
	if (d.is_discarded() || !d.is_object())
	{
		d = nlohmann::json::object();
	}

	target->languageService->PrintConsole(false, false, "Jumpstats Report - Console Summary", s.alias.c_str(), s.distance, typeLong, "");

	std::string modeStyle = JsonStr(d, "mode");
	if (modeStyle.empty())
	{
		modeStyle = modeName;
	}
	std::string blockStr, edgeStr, landingEdgeStr, missStr;
	if (JsonNum(d, "edge", -1.0) >= 0.0)
	{
		edgeStr = KZLanguageService::PrepareMessageWithLang(lang, "Jumpstat Report - Console Segment - Edge", JsonNum(d, "edge"));
	}
	if (JsonNum(d, "landingEdge") > 0.0)
	{
		landingEdgeStr = KZLanguageService::PrepareMessageWithLang(lang, "Jumpstat Report - Console Segment - Landing Edge", JsonNum(d, "landingEdge"));
	}
	if (JsonNum(d, "block") > 0.0)
	{
		blockStr = KZLanguageService::PrepareMessageWithLang(lang, "Jumpstat Report - Console Segment - Block", JsonNum(d, "block"));
	}
	if (JsonNum(d, "miss") > 0.0)
	{
		missStr = KZLanguageService::PrepareMessageWithLang(lang, "Jumpstat Report - Console Segment - Miss", JsonNum(d, "miss"));
	}
	std::string releaseStr;
	if ((s.type == JumpType_LongJump || s.type == JumpType_LadderJump || s.type == JumpType_WeirdJump) && d.contains("releaseTicks"))
	{
		releaseStr = ReleaseString(JsonNum(d, "releaseTicks"));
	}
	i32 strafeCount = (i32)JsonNum(d, "strafes");
	// clang-format off
	target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Details 1",
		modeStyle.c_str(),
		blockStr.c_str(),
		edgeStr.c_str(),
		landingEdgeStr.c_str(),
		missStr.c_str(),
		strafeCount,
		KZLanguageService::PrepareMessageWithLang(lang, strafeCount > 1 ? "Strafes" : "Strafe").c_str(),
		JsonNum(d, "sync") * 100.0,
		JsonNum(d, "pre"),
		JsonNum(d, "max"),
		JsonNum(d, "badAngles") * 100.0,
		JsonNum(d, "overlap") * 100.0,
		JsonNum(d, "deadAir") * 100.0,
		JsonNum(d, "height"),
		releaseStr.c_str()
	);
	target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Details 2",
		JsonNum(d, "gainEff") * 100.0,
		JsonNum(d, "airPath"),
		JsonNum(d, "deviation"),
		JsonNum(d, "width"),
		JsonNum(d, "airtime"),
		JsonNum(d, "offset"),
		JsonNum(d, "duckEnd"),
		JsonNum(d, "duck")
	);
	// clang-format on

	auto list = d.find("strafeList");
	if (list != d.end() && list->is_array() && !list->empty())
	{
		CUtlString headers[KZ_ARRAYSIZE(storedColumnKeys)];
		for (u32 i = 0; i < KZ_ARRAYSIZE(storedColumnKeys); i++)
		{
			headers[i] = target->languageService->PrepareMessage(storedColumnKeys[i]).c_str();
		}
		utils::Table<KZ_ARRAYSIZE(storedColumnKeys)> table("", headers);
		u32 row = 0;
		for (const nlohmann::json &st : *list)
		{
			if (!st.is_object())
			{
				continue;
			}
			f64 duration = JsonNum(st, "duration");
			f64 gain = JsonNum(st, "gain");
			f64 maxGain = JsonNum(st, "maxGain");
			char num[8], sync[16], gainS[16], extGain[16], loss[16], extLoss[16], maxS[16], dur[16];
			char ba[16], ol[16], da[16], width[16], avgGain[16], gainEff[16], ar[32];
			V_snprintf(num, sizeof(num), "%u.", row + 1);
			V_snprintf(sync, sizeof(sync), "%.0f%%%%", JsonNum(st, "sync") * 100.0);
			V_snprintf(gainS, sizeof(gainS), "%.2f", gain);
			V_snprintf(extGain, sizeof(extGain), "(+%.2f)", fabs(JsonNum(st, "externalGain")));
			V_snprintf(loss, sizeof(loss), "-%.2f", fabs(JsonNum(st, "loss")));
			V_snprintf(extLoss, sizeof(extLoss), "(-%.2f)", fabs(JsonNum(st, "externalLoss")));
			V_snprintf(maxS, sizeof(maxS), "%.2f", JsonNum(st, "maxSpeed"));
			V_snprintf(dur, sizeof(dur), "%.3f", duration);
			V_snprintf(ba, sizeof(ba), "%.1f", JsonNum(st, "badAngles") * ENGINE_FIXED_TICK_RATE);
			V_snprintf(ol, sizeof(ol), "%.1f", JsonNum(st, "overlap") * ENGINE_FIXED_TICK_RATE);
			V_snprintf(da, sizeof(da), "%.1f", JsonNum(st, "deadAir") * ENGINE_FIXED_TICK_RATE);
			V_snprintf(width, sizeof(width), "%.1f", fabs(JsonNum(st, "width")));
			V_snprintf(avgGain, sizeof(avgGain), "%.2f", duration > 0.0 ? gain / duration * ENGINE_FIXED_TICK_INTERVAL : 0.0);
			V_snprintf(gainEff, sizeof(gainEff), "%.0f%%%%", maxGain > 0.0 ? gain / maxGain * 100.0 : 0.0);
			if (st.contains("arAverage"))
			{
				V_snprintf(ar, sizeof(ar), "%.2f/%.2f/%.2f", JsonNum(st, "arAverage"), JsonNum(st, "arMedian"), JsonNum(st, "arMax"));
			}
			else
			{
				V_snprintf(ar, sizeof(ar), "N/A");
			}
			table.SetRow(row, num, sync, gainS, extGain, loss, extLoss, maxS, dur, ba, ol, da, width, avgGain, gainEff, ar);
			row++;
		}
		target->PrintConsole(false, false, table.GetHeader());
		for (u32 i = 0; i < table.GetNumEntries(); i++)
		{
			target->PrintConsole(false, false, table.GetLine(i));
		}
	}

	// График клавиш и мыши — только у прыжков, записанных с cyb.271 (раньше не сохранялся).
	auto graph = d.find("graph");
	if (graph != d.end() && graph->is_object())
	{
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Strafe Keys");
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Left", JsonStr(*graph, "strafeLeft").c_str());
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Right", JsonStr(*graph, "strafeRight").c_str());
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Mouse Movement");
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Left", JsonStr(*graph, "mouseLeft").c_str());
		target->languageService->PrintConsole(false, false, "Jumpstat Report - Console Graph - Right", JsonStr(*graph, "mouseRight").c_str());
	}

	char tail[256];
	V_snprintf(tail, sizeof(tail), "SteamID %llu · replay %s\n\n", s.steamID64, s.replay.empty() ? "—" : s.replay.c_str());
	utils::PrintConsole(target->GetController(), "%s", tail);
}

/*
 * !jumpinfo <id> — строка топа целиком в консоль (разбор на АХК).
 */
static_function void PrintJumpInfo(KZPlayer *player, i32 id, bool andReplay)
{
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
		return;
	}
	char query[1024];
	V_snprintf(query, sizeof(query), sql_jumptop_getjump, id);
	Transaction txn;
	txn.queries.push_back(query);
	CPlayerUserId userID = player->GetClient()->GetUserID();
	// clang-format off
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
		txn,
		[userID, id, andReplay](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (!player)
			{
				return;
			}
			ISQLResult *r = queries[0]->GetResultSet();
			if (!r || !r->FetchRow())
			{
				player->languageService->PrintChat(true, false, "Jumptop - Jump Not Found", id);
				return;
			}
			StoredJump sj;
			sj.id = id;
			sj.steamID64 = (u64)r->GetInt64(1);
			sj.alias = r->GetString(2) ? r->GetString(2) : "";
			sj.mode = r->GetInt(3);
			sj.type = r->GetInt(4);
			sj.isBlock = r->GetInt(5) != 0;
			sj.block = r->GetInt(6);
			sj.distance = ResultDouble(r, 7);
			sj.map = r->GetString(15) ? r->GetString(15) : "";
			sj.server = r->GetString(16) ? r->GetString(16) : "";
			sj.replay = r->GetString(17) ? r->GetString(17) : "";
			sj.details = r->GetString(18) ? r->GetString(18) : "";
			sj.removed = r->GetInt(19) != 0;
			sj.created = r->GetString(20) ? r->GetString(20) : "";
			PrintStoredJumpReport(player, sj);

			const std::string &alias = sj.alias;
			const std::string &replay = sj.replay;
			std::string label = JumpLabel(player->languageService->GetLanguage(), sj.type, sj.isBlock, sj.block, sj.distance);
			std::string modeName = ModeShortName(sj.mode);
			player->languageService->PrintChat(true, false, "Jumptop - Info Printed", alias.c_str(), label.c_str(), modeName.c_str(), id);
			if (andReplay && !replay.empty())
			{
				// Топ общий для всех карт, а плеер играет реплей только на его карте — не качаем
				// заведомо отбиваемый файл, а говорим, где прыжок.
				std::string jumpMap = r->GetString(15) ? r->GetString(15) : "";
				if (!KZ_STREQI(jumpMap.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
				{
					player->languageService->PrintChat(true, false, "Jumptop - Replay Other Map", jumpMap.c_str());
					return;
				}
				PlayJumpReplay(player, replay);
			}
		},
		[userID](std::string error, int failIndex)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (player)
			{
				player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
			}
		});
	// clang-format on
}

/*
 * Меню !jumptop: корень (режим и вид топа листаются A/D + 7 типов) → топ-20 → карточка прыжка
 * (статистика, реплей ботом, полная статистика в консоль). R в топе и карточке — шаг назад.
 */
namespace
{
	MenuHandle g_rootMenu[MAXPLAYERS + 1] = {};
	MenuHandle g_topMenu[MAXPLAYERS + 1] = {};
	MenuHandle g_cardMenu[MAXPLAYERS + 1] = {};
	i32 g_menuMode[MAXPLAYERS + 1] = {};
	bool g_menuBlock[MAXPLAYERS + 1] = {};
	// Карта прыжка в открытой карточке: реплей играет только на ней, перепроверяем при выборе.
	std::string g_cardMap[MAXPLAYERS + 1];

	// Строки корня: 0 — режим, 1 — вид топа, дальше типы прыжков по порядку.
	constexpr int rootRowMode = 0;
	constexpr int rootRowBoard = 1;
	constexpr int rootRowFirstType = 2;

	void ResetMenu(MenuHandle *slotMenu)
	{
		if (*slotMenu != kInvalidMenuHandle)
		{
			g_pMenus->DestroyMenu(*slotMenu);
			*slotMenu = kInvalidMenuHandle;
		}
	}

	// R в child возвращает в parent. AddSubMenu ставит связь только вместе с пунктом-ссылкой,
	// а топ и карточка строятся асинхронно после выбора — пункт сразу убираем, связь остаётся.
	void LinkParent(MenuHandle parent, MenuHandle child)
	{
		if (parent == kInvalidMenuHandle || child == kInvalidMenuHandle)
		{
			return;
		}
		int item = g_pMenus->AddSubMenu(parent, "", child, "");
		if (item >= 0)
		{
			g_pMenus->RemoveItem(parent, item);
		}
	}

	bool IsSlotValid(int slot)
	{
		return slot >= 0 && slot <= MAXPLAYERS;
	}

	// Ответ базы пришёл, а игрок уже ушёл из меню, откуда ждал (вышел, открыл другое) — не
	// перебиваем. Без родителя (прямой !jumptop lj) показываем всегда.
	bool StillWaitingIn(int slot, MenuHandle parent, MenuHandle current)
	{
		return parent == kInvalidMenuHandle || (parent == current && g_pMenus->GetActiveMenu(slot) == parent);
	}

	// Цвет тира — как у отчёта о прыжке в чате; пороги режима топа, а не режима смотрящего.
	// Цвет не сбрасываем: значение стоит в конце строки, а сброс ({default}) дал бы жёсткий белый
	// и съел бы подсветку курсора у остальной строки.
	std::string TierColored(KZModeService *tiers, i32 jumpType, f64 distance, f64 pre, const char *text)
	{
		DistanceTier tier = tiers ? tiers->GetDistanceTier((JumpType)jumpType, (f32)distance, (f32)pre) : DistanceTier_Meh;
		if (tier == DistanceTier_None)
		{
			tier = DistanceTier_Meh;
		}
		char tagged[128];
		V_snprintf(tagged, sizeof(tagged), "%s%s", distanceTierColors[tier], text);
		char out[128];
		if (!utils::CFormat(out, sizeof(out), tagged))
		{
			return text;
		}
		// CFormat ставит пробел в начало (для чата) — в меню он лишний.
		return out[0] == ' ' ? out + 1 : out;
	}

	// «2026-10-09 12:34:56» → «09.10.2026».
	std::string ShortDate(const char *created)
	{
		if (!created || V_strlen(created) < 10)
		{
			return "";
		}
		char buf[16];
		V_snprintf(buf, sizeof(buf), "%.2s.%.2s.%.4s", created + 8, created + 5, created);
		return buf;
	}

	void OpenRootMenu(KZPlayer *player, i32 modeID, bool isBlock);
	void ShowTop(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock);

	void OnCardMenuSelect(MenuHandle menu, int slot, int item)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		const char *info = g_pMenus->GetItemInfo(menu, item);
		if (!player || !info || !info[0] || !IsSlotValid(slot))
		{
			return;
		}
		std::string action = info;
		if (action.rfind("c:", 0) == 0)
		{
			PrintJumpInfo(player, V_StringToInt32(action.c_str() + 2, 0), false);
		}
		else if (action.rfind("r:", 0) == 0)
		{
			// Карточка могла пережить смену карты, пока висела.
			if (!KZ_STREQI(g_cardMap[slot].c_str(), g_pKZUtils->GetCurrentMapName().Get()))
			{
				player->languageService->PrintChat(true, false, "Jumptop - Replay Other Map", g_cardMap[slot].c_str());
				return;
			}
			g_pMenus->CancelMenu(slot);
			PlayJumpReplay(player, action.substr(2));
		}
	}

	void OpenCard(KZPlayer *player, i32 id, MenuHandle parent)
	{
		if (!KZDatabaseService::IsReady())
		{
			player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
			return;
		}
		char query[1024];
		V_snprintf(query, sizeof(query), sql_jumptop_getjump, id);
		Transaction txn;
		txn.queries.push_back(query);
		CPlayerUserId userID = player->GetClient()->GetUserID();
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[userID, id, parent](std::vector<ISQLQuery *> queries)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (!player || g_pMenus == nullptr)
				{
					return;
				}
				int slot = player->GetPlayerSlot().Get();
				if (!IsSlotValid(slot))
				{
					return;
				}
				if (!StillWaitingIn(slot, parent, g_topMenu[slot]))
				{
					return;
				}
				ISQLResult *r = queries[0]->GetResultSet();
				// Снятый админом, пока висел топ, — как не найденный.
				if (!r || !r->FetchRow() || r->GetInt(19) != 0)
				{
					player->languageService->PrintChat(true, false, "Jumptop - Jump Not Found", id);
					return;
				}
				const char *lang = player->languageService->GetLanguage();
				std::string alias = r->GetString(2) ? r->GetString(2) : "";
				i32 type = r->GetInt(4);
				bool isBlock = r->GetInt(5) != 0;
				i32 block = r->GetInt(6);
				f64 distance = ResultDouble(r, 7);
				std::string map = r->GetString(15) ? r->GetString(15) : "";
				std::string replay = r->GetString(17) ? r->GetString(17) : "";
				const char *typeStr = (type >= 0 && type < JUMPTYPE_COUNT) ? jumpTypeShortStr[type] : "?";

				char title[192];
				if (isBlock)
				{
					std::string blockWord = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Block Word");
					V_snprintf(title, sizeof(title), "%s · %s %d %s (%.2f)", alias.c_str(), typeStr, block, blockWord.c_str(), distance);
				}
				else
				{
					V_snprintf(title, sizeof(title), "%s · %s %.2f", alias.c_str(), typeStr, distance);
				}

				ResetMenu(&g_cardMenu[slot]);
				MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnCardMenuSelect);
				if (m == kInvalidMenuHandle)
				{
					return;
				}
				std::string date = ShortDate(r->GetString(20));
				std::string where = date.empty() ? map : map + " · " + date;
				g_pMenus->AddItem(m, where.c_str(), "", true);
				std::string stats1 = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card Stats", r->GetInt(8), ResultDouble(r, 9) * 100.0);
				g_pMenus->AddItem(m, stats1.c_str(), "", true);
				std::string stats2 = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card Speed", ResultDouble(r, 10), ResultDouble(r, 11));
				g_pMenus->AddItem(m, stats2.c_str(), "", true);

				g_cardMap[slot] = map;
				int startItem = 4;
				if (replay.empty())
				{
					std::string text = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card No Replay");
					g_pMenus->AddItem(m, text.c_str(), "", true);
				}
				else if (!KZ_STREQI(map.c_str(), g_pKZUtils->GetCurrentMapName().Get()))
				{
					std::string text = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card Replay Other Map", map.c_str());
					g_pMenus->AddItem(m, text.c_str(), "", true);
				}
				else
				{
					std::string text = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card Replay");
					std::string info = "r:" + replay;
					startItem = g_pMenus->AddItem(m, text.c_str(), info.c_str(), false);
				}
				std::string consoleText = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Card Console");
				char info[24];
				V_snprintf(info, sizeof(info), "c:%d", id);
				g_pMenus->AddItem(m, consoleText.c_str(), info, false);

				// Первые строки — справка, курсор сразу на действии (реплей, если он доступен).
				g_pMenus->SetStartItem(m, startItem);
				g_pMenus->SetCloseOnSelect(m, false);
				if (parent != kInvalidMenuHandle)
				{
					LinkParent(parent, m);
				}
				g_cardMenu[slot] = m;
				g_pMenus->DisplayMenu(m, slot, 0);
			},
			[userID](std::string error, int failIndex)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (player)
				{
					player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
				}
			});
		// clang-format on
	}

	void OnTopMenuSelect(MenuHandle menu, int slot, int item)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		const char *info = g_pMenus->GetItemInfo(menu, item);
		if (!player || !info || !info[0])
		{
			return;
		}
		// Вернувшись из карточки по R, курсор встанет на тот же прыжок.
		g_pMenus->SetStartItem(menu, item);
		OpenCard(player, V_StringToInt32(info, 0), menu);
	}

	void ShowTop(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock, MenuHandle parent)
	{
		if (!KZDatabaseService::IsReady())
		{
			player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
			return;
		}
		char query[2048];
		V_snprintf(query, sizeof(query), sql_jumptop_gettop, modeID, jumpType, isBlock ? 1 : 0, KZ::jumptop::topCount);
		Transaction txn;
		txn.queries.push_back(query);
		CPlayerUserId userID = player->GetClient()->GetUserID();
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[userID, modeID, jumpType, isBlock, parent](std::vector<ISQLQuery *> queries)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (!player)
				{
					return;
				}
				const char *lang = player->languageService->GetLanguage();
				std::string mode = ModeShortName(modeID);
				std::string kind = KZLanguageService::PrepareMessageWithLang(lang, isBlock ? "Jumptop - Kind Block" : "Jumptop - Kind Distance");
				std::string title = KZLanguageService::PrepareMessageWithLang(lang, isBlock ? "Jumptop - Top Title Block" : "Jumptop - Top Title",
																			  jumpTypeStr[jumpType], mode.c_str());
				std::string blockWord = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Block Word");

				// Пороги тиров — у сервиса режима ТОПА (смотрящий может стоять в другом режиме).
				// Сервис временный: только для GetDistanceTier, без Init — как в смене режима.
				auto modeInfo = KZ::mode::GetModeInfoFromDatabaseID(modeID);
				KZModeService *tiers = modeInfo.factory ? modeInfo.factory(player) : nullptr;

				ISQLResult *r = queries[0]->GetResultSet();
				std::vector<std::pair<std::string, std::string>> rows;
				utils::PrintConsole(player->GetController(), "\n%s\n", title.c_str());
				i32 place = 0;
				while (r && r->FetchRow())
				{
					place++;
					i32 id = r->GetInt(0);
					std::string alias = r->GetString(2) ? r->GetString(2) : "";
					i32 block = r->GetInt(3);
					f64 distance = ResultDouble(r, 4);
					f64 pre = ResultDouble(r, 7);
					const char *map = r->GetString(11) ? r->GetString(11) : "";
					char value[64];
					if (isBlock)
					{
						V_snprintf(value, sizeof(value), "%d %s (%.2f)", block, blockWord.c_str(), distance);
					}
					else
					{
						V_snprintf(value, sizeof(value), "%.2f", distance);
					}
					std::string colored = TierColored(tiers, jumpType, distance, pre, value);
					char text[256];
					V_snprintf(text, sizeof(text), "%d. %s  ·  %s", place, alias.c_str(), colored.c_str());
					char info[16];
					V_snprintf(info, sizeof(info), "%d", id);
					rows.emplace_back(text, info);
					char blockPrefix[32] = "";
					if (isBlock)
					{
						V_snprintf(blockPrefix, sizeof(blockPrefix), "%d %s ", block, blockWord.c_str());
					}
					utils::PrintConsole(player->GetController(),
										"%2d. %s%.4f  %s  | %s | %d Strafes | %.1f%% Sync | %.2f Pre | %.2f Max | %.3f Airtime | !jumpinfo %d\n",
										place, blockPrefix, distance, alias.c_str(), map, r->GetInt(5), ResultDouble(r, 6) * 100.0, pre,
										ResultDouble(r, 8), ResultDouble(r, 9), id);
				}
				delete tiers;
				if (rows.empty())
				{
					player->languageService->PrintChat(true, false, "Jumptop - Empty", jumpTypeShortStr[jumpType], kind.c_str(), mode.c_str());
					return;
				}
				int slot = player->GetPlayerSlot().Get();
				if (g_pMenus == nullptr || !IsSlotValid(slot))
				{
					player->languageService->PrintChat(true, false, "Jumptop - See Console");
					return;
				}
				if (!StillWaitingIn(slot, parent, g_rootMenu[slot]))
				{
					return;
				}
				ResetMenu(&g_cardMenu[slot]);
				ResetMenu(&g_topMenu[slot]);
				MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnTopMenuSelect);
				if (m == kInvalidMenuHandle)
				{
					return;
				}
				for (auto &row : rows)
				{
					g_pMenus->AddItem(m, row.first.c_str(), row.second.c_str(), false);
				}
				g_pMenus->SetCloseOnSelect(m, false);
				if (parent != kInvalidMenuHandle)
				{
					LinkParent(parent, m);
				}
				g_topMenu[slot] = m;
				g_pMenus->DisplayMenu(m, slot, 0);
			},
			[userID](std::string error, int failIndex)
			{
				KZ_LOG_ERROR(LogChannel::DB, "[cyb] jumptop_top_failed error=%s\n", error.c_str());
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (player)
				{
					player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
				}
			});
		// clang-format on
	}

	void ShowTop(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock)
	{
		ShowTop(player, modeID, jumpType, isBlock, kInvalidMenuHandle);
	}

	// Следующий/предыдущий режим джамптопа из загруженных (dir = +1/-1).
	i32 StepModeID(i32 modeID, i32 dir)
	{
		i32 count = KZ_ARRAYSIZE(jumptopModes);
		i32 current = -1;
		for (i32 i = 0; i < count; i++)
		{
			if (KZ::mode::GetModeInfo(CUtlString(jumptopModes[i])).databaseID == modeID)
			{
				current = i;
			}
		}
		if (current < 0)
		{
			current = dir > 0 ? count - 1 : 0;
		}
		for (i32 step = 1; step <= count; step++)
		{
			i32 id = KZ::mode::GetModeInfo(CUtlString(jumptopModes[((current + dir * step) % count + count) % count])).databaseID;
			if (id >= 0)
			{
				return id;
			}
		}
		return modeID;
	}

	std::string RootModeText(const char *lang, i32 modeID)
	{
		return KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Menu Mode", ModeShortName(modeID).c_str());
	}

	std::string RootBoardText(const char *lang, bool isBlock)
	{
		return KZLanguageService::PrepareMessageWithLang(lang, isBlock ? "Jumptop - Menu Board Block" : "Jumptop - Menu Board Distance");
	}

	// A/D на строках режима и вида топа; E на них (и чат-меню, где A/D нет) — шаг вперёд.
	void StepRootRow(MenuHandle menu, int slot, int item, i32 dir)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		if (!player || !IsSlotValid(slot))
		{
			return;
		}
		const char *lang = player->languageService->GetLanguage();
		if (item == rootRowMode)
		{
			g_menuMode[slot] = StepModeID(g_menuMode[slot], dir);
			g_pMenus->SetItemText(menu, item, RootModeText(lang, g_menuMode[slot]).c_str());
		}
		else if (item == rootRowBoard)
		{
			g_menuBlock[slot] = !g_menuBlock[slot];
			g_pMenus->SetItemText(menu, item, RootBoardText(lang, g_menuBlock[slot]).c_str());
		}
	}

	void OnRootMenuAdjust(MenuHandle menu, int slot, int item, float delta, float minValue, float maxValue)
	{
		StepRootRow(menu, slot, item, delta < 0 ? -1 : 1);
	}

	void OnRootMenuSelect(MenuHandle menu, int slot, int item)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		const char *info = g_pMenus->GetItemInfo(menu, item);
		if (!player || !info || !info[0] || !IsSlotValid(slot))
		{
			return;
		}
		if (item == rootRowMode || item == rootRowBoard)
		{
			StepRootRow(menu, slot, item, 1);
			return;
		}
		i32 type = V_StringToInt32(info, -1);
		if (type < 0)
		{
			return;
		}
		// Вернувшись из топа по R, курсор встанет на тот же тип.
		g_pMenus->SetStartItem(menu, item);
		ShowTop(player, g_menuMode[slot], type, g_menuBlock[slot], menu);
	}

	void OpenRootMenu(KZPlayer *player, i32 modeID, bool isBlock)
	{
		int slot = player->GetPlayerSlot().Get();
		if (!IsSlotValid(slot))
		{
			return;
		}
		ResetMenu(&g_cardMenu[slot]);
		ResetMenu(&g_topMenu[slot]);
		ResetMenu(&g_rootMenu[slot]);
		g_menuMode[slot] = modeID;
		g_menuBlock[slot] = isBlock;
		const char *lang = player->languageService->GetLanguage();
		std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Menu Title");
		MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnRootMenuSelect);
		if (m == kInvalidMenuHandle)
		{
			return;
		}
		// Значение строк держим сами (g_menuMode/g_menuBlock), границы движку не нужны.
		g_pMenus->AddAdjustableItem(m, RootModeText(lang, modeID).c_str(), "mode", 1.0f, -1.0f, 1.0f);
		g_pMenus->AddAdjustableItem(m, RootBoardText(lang, isBlock).c_str(), "board", 1.0f, -1.0f, 1.0f);
		for (i32 type = JumpType_LongJump; type <= JumpType_Jumpbug; type++)
		{
			char info[8];
			V_snprintf(info, sizeof(info), "%d", type);
			g_pMenus->AddItem(m, jumpTypeStr[type], info, false);
		}
		g_pMenus->SetAdjustCallback(m, &OnRootMenuAdjust);
		g_pMenus->SetStartItem(m, rootRowFirstType);
		g_pMenus->SetCloseOnSelect(m, false);
		g_rootMenu[slot] = m;
		g_pMenus->DisplayMenu(m, slot, 0);
	}

	i32 ParseJumpType(const char *arg)
	{
		for (i32 type = JumpType_LongJump; type <= JumpType_Jumpbug; type++)
		{
			if (KZ_STREQI(arg, jumpTypeShortStr[type]))
			{
				return type;
			}
		}
		return -1;
	}

	i32 ParseModeID(const char *arg)
	{
		for (const char *mode : jumptopModes)
		{
			if (KZ_STREQI(arg, mode))
			{
				return KZ::mode::GetModeInfo(CUtlString(mode)).databaseID;
			}
		}
		return -1;
	}

	void PrintPlayerBests(KZPlayer *player, u64 steamID64, std::string alias, i32 modeID)
	{
		char query[1024];
		V_snprintf(query, sizeof(query), sql_jumptop_getplayerbests, steamID64, modeID);
		Transaction txn;
		txn.queries.push_back(query);
		CPlayerUserId userID = player->GetClient()->GetUserID();
		// clang-format off
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
			txn,
			[userID, alias, modeID](std::vector<ISQLQuery *> queries)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (!player)
				{
					return;
				}
				const char *lang = player->languageService->GetLanguage();
				std::string mode = ModeShortName(modeID);
				ISQLResult *r = queries[0]->GetResultSet();
				std::string line;
				i32 count = 0;
				while (r && r->FetchRow())
				{
					std::string label = JumpLabel(lang, r->GetInt(1), r->GetInt(2) != 0, r->GetInt(3), ResultDouble(r, 4));
					const char *map = r->GetString(10) ? r->GetString(10) : "";
					utils::PrintConsole(player->GetController(), "[jumptop #%d] %s %s | %s | strafes %d | sync %.1f%% | pre %.2f | max %.2f | air %.3f\n",
										r->GetInt(0), label.c_str(), mode.c_str(), map, r->GetInt(5), ResultDouble(r, 6) * 100.0, ResultDouble(r, 7),
										ResultDouble(r, 8), ResultDouble(r, 9));
					if (!line.empty())
					{
						line += "{grey}, {default}";
					}
					line += label;
					count++;
				}
				if (count == 0)
				{
					player->languageService->PrintChat(true, false, "Jumptop - No PBs", alias.c_str(), mode.c_str());
					return;
				}
				player->languageService->PrintChat(true, false, "Jumptop - PBs", alias.c_str(), mode.c_str(), line.c_str());
			},
			[userID](std::string error, int failIndex)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (player)
				{
					player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
				}
			});
		// clang-format on
	}
} // namespace

// !jumptop [тип] [block] [режим] — без аргументов меню; с типом сразу топ.
SCMD(kz_jumptop, SCFL_JUMPSTATS | SCFL_RECORD | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
		return true;
	}
	i32 modeID = KZ::mode::GetModeInfo(player->modeService).databaseID;
	i32 jumpType = -1;
	bool isBlock = false;
	for (i32 i = 1; i < args->ArgC(); i++)
	{
		const char *arg = args->Arg(i);
		i32 type = ParseJumpType(arg);
		i32 mode = ParseModeID(arg);
		if (type >= 0)
		{
			jumpType = type;
		}
		else if (mode >= 0)
		{
			modeID = mode;
		}
		else if (KZ_STREQI(arg, "block") || KZ_STREQI(arg, "blocks") || KZ_STREQI(arg, "блок") || KZ_STREQI(arg, "блоки"))
		{
			isBlock = true;
		}
	}
	if (modeID < 0)
	{
		player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
		return true;
	}
	if (jumpType >= 0)
	{
		ShowTop(player, modeID, jumpType, isBlock);
		return true;
	}
	if (g_pMenus == nullptr)
	{
		player->languageService->PrintChat(true, false, "Jumptop - Usage");
		return true;
	}
	OpenRootMenu(player, modeID, isBlock);
	return true;
}

SCMD_LINK(kz_jstop, kz_jumptop);
SCMD_LINK(kz_jt, kz_jumptop);

// !jspb [ник] — личные рекорды в текущем режиме (своём или указанном ником).
SCMD(kz_jspb, SCFL_JUMPSTATS | SCFL_RECORD | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	if (!KZDatabaseService::IsReady())
	{
		player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
		return true;
	}
	i32 modeID = KZ::mode::GetModeInfo(player->modeService).databaseID;
	const char *query = args->ArgC() > 1 ? args->ArgS() : "";
	if (!query[0])
	{
		PrintPlayerBests(player, player->GetSteamId64(), player->GetName(), modeID);
		return true;
	}
	CPlayerUserId userID = player->GetClient()->GetUserID();
	// clang-format off
	KZDatabaseService::FindPlayerByAlias(query,
		[userID, modeID](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (!player)
			{
				return;
			}
			ISQLResult *r = queries[0]->GetResultSet();
			if (!r || !r->FetchRow())
			{
				player->languageService->PrintChat(true, false, "Jumptop - Player Not Found");
				return;
			}
			PrintPlayerBests(player, (u64)r->GetInt64(0), r->GetString(1) ? r->GetString(1) : "", modeID);
		},
		[userID](std::string error, int failIndex)
		{
			KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
			if (player)
			{
				player->languageService->PrintChat(true, false, "Jumptop - Database Unavailable");
			}
		});
	// clang-format on
	return true;
}

SCMD_LINK(kz_jsbest, kz_jspb);

// !jumpinfo <id> — полная статистика строки топа в консоль; !jumpreplay <id> — то же + реплей.
SCMD(kz_jumpinfo, SCFL_JUMPSTATS | SCFL_RECORD)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	i32 id = args->ArgC() > 1 ? V_StringToInt32(args->Arg(1), 0) : 0;
	if (id <= 0)
	{
		player->languageService->PrintChat(true, false, "Jumptop - Info Usage");
		return true;
	}
	PrintJumpInfo(player, id, false);
	return true;
}

SCMD(kz_jumpreplay, SCFL_JUMPSTATS | SCFL_REPLAY)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return true;
	}
	i32 id = args->ArgC() > 1 ? V_StringToInt32(args->Arg(1), 0) : 0;
	if (id <= 0)
	{
		player->languageService->PrintChat(true, false, "Jumptop - Info Usage");
		return true;
	}
	PrintJumpInfo(player, id, true);
	return true;
}

// Снятие прыжка с топа (читерский/багнутый) — только с серверной консоли/RCON. Мягкое: строка
// и реплей остаются для разбора. Кэш PB игрока перечитается на его следующем заходе.
static_function void SetJumpRemoved(i32 id, bool removed)
{
	if (!KZDatabaseService::IsReady())
	{
		META_CONPRINTF("[jumptop] database not ready\n");
		return;
	}
	char query[256];
	V_snprintf(query, sizeof(query), sql_jumptop_setremoved, removed ? 1 : 0, id);
	Transaction txn;
	txn.queries.push_back(query);
	// clang-format off
	KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(
		txn,
		[id, removed](std::vector<ISQLQuery *> queries)
		{
			META_CONPRINTF("[jumptop] #%d %s (rows: %u)\n", id, removed ? "removed" : "restored", queries[0]->GetAffectedRows());
			KZ_LOG_INFO(LogChannel::DB, "[cyb] jumptop_set_removed id=%d removed=%d rows=%u\n", id, removed ? 1 : 0, queries[0]->GetAffectedRows());
		},
		[id](std::string error, int failIndex) { META_CONPRINTF("[jumptop] #%d failed: %s\n", id, error.c_str()); });
	// clang-format on
}

CON_COMMAND_F(kz_jumptop_remove, "Снять прыжок с джамптопа по ID (строка и реплей остаются в базе).", FCVAR_NONE)
{
	// Игрокам не отдаём, как и прочие настоящие ConCommand форка (канон — kz_invisible.cpp).
	if (utils::GetController(context.GetPlayerSlot()))
	{
		return;
	}
	if (args.ArgC() != 2)
	{
		META_CONPRINTF("Usage: kz_jumptop_remove <id>\n");
		return;
	}
	SetJumpRemoved(V_StringToInt32(args.Arg(1), 0), true);
}

CON_COMMAND_F(kz_jumptop_restore, "Вернуть снятый прыжок в джамптоп по ID.", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		return;
	}
	if (args.ArgC() != 2)
	{
		META_CONPRINTF("Usage: kz_jumptop_restore <id>\n");
		return;
	}
	SetJumpRemoved(V_StringToInt32(args.Arg(1), 0), false);
}

// Админка удалила прыжок(и) игрока из базы (api, DELETE /admin/v1/kz/jumps) и рассылает это по
// всем kz-серверам. Здесь: перечитать кэш PB игрока, если он на этом сервере (иначе до реконнекта
// ему не засчитывались бы честные прыжки ниже удалённого), удалить локальные файлы реплея и не
// дать позднему буферу реплея доехать в базу. Ответ «kz_jumptop_forget: ok» — подтверждение для api.
CON_COMMAND_F(kz_jumptop_forget, "Забыть удалённые прыжки: kz_jumptop_forget <steamid64> [replayUuid ...]", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		return;
	}
	if (args.ArgC() < 2)
	{
		META_CONPRINTF("Usage: kz_jumptop_forget <steamid64> [replayUuid ...]\n");
		return;
	}
	u64 steamID64 = strtoull(args.Arg(1), nullptr, 10);
	if (steamID64 == 0)
	{
		META_CONPRINTF("kz_jumptop_forget: bad steamid\n");
		return;
	}
	i32 files = 0;
	for (i32 i = 2; i < args.ArgC(); i++)
	{
		UUID_t parsed;
		if (!UUID_t::FromString(args.Arg(i), &parsed))
		{
			continue;
		}
		std::string uuid = parsed.ToString();
		if (!IsForgotten(uuid))
		{
			g_forgottenReplays.push_back(uuid);
			// api шлёт UUID пачками по 10 — 256 с запасом держит недавние удаления.
			while (g_forgottenReplays.size() > 256)
			{
				g_forgottenReplays.pop_front();
			}
		}
		for (auto it = g_pendingReplays.begin(); it != g_pendingReplays.end();)
		{
			it = it->first == uuid ? g_pendingReplays.erase(it) : it + 1;
		}
		for (auto it = g_qualifiedReplays.begin(); it != g_qualifiedReplays.end();)
		{
			it = *it == uuid ? g_qualifiedReplays.erase(it) : it + 1;
		}
		char path[512];
		V_snprintf(path, sizeof(path), KZ_REPLAY_PATH "/%s.replay", uuid.c_str());
		if (g_pFullFileSystem->FileExists(path))
		{
			utils::RemoveFile(path);
			files++;
		}
		V_snprintf(path, sizeof(path), KZ_REPLAY_DOWNLOADS_PATH "/%s.replay", uuid.c_str());
		if (g_pFullFileSystem->FileExists(path))
		{
			utils::RemoveFile(path);
			files++;
		}
	}
	KZPlayer *player = g_pKZPlayerManager->SteamIdToPlayer(steamID64, false);
	bool reloaded = false;
	if (player && !player->IsFakeClient())
	{
		LoadPBs(player, steamID64);
		reloaded = true;
	}
	KZ_LOG_INFO(LogChannel::DB, "[cyb] jumptop_forget steam_id=%llu replays=%d files=%d pb_reloaded=%d\n", steamID64, args.ArgC() - 2, files,
				reloaded ? 1 : 0);
	// Msg, как у kz_db_status: этот вывод возвращается в ответ RCON, по нему api считает подтверждения.
	Msg("kz_jumptop_forget: ok\n");
}
