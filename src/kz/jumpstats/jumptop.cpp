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

		// Битый UTF-8 (имя карты) не должен уронить сервер: исключения выключены, throw = abort.
		return d.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
	}

	void PlaySoundToAll(const char *sound)
	{
		for (i32 i = 0; i <= MAXPLAYERS; i++)
		{
			KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
			if (!other || !other->IsInGame() || other->IsFakeClient())
			{
				continue;
			}
			if (!other->optionService->GetPreferenceBool("jsReporting", true))
			{
				continue;
			}
			utils::PlaySoundToClient(other->GetPlayerSlot(), sound, other->optionService->GetPreferenceFloat("jsVolume", 0.75f));
		}
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

	void QualifyReplay(const std::string &uuid)
	{
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
	void SubmitPB(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock, i32 block, Jump *jump, const std::string &replayUuid,
				  const std::string &details)
	{
		ISQLConnection *db = KZDatabaseService::GetDatabaseConnection();
		std::string map = db->Escape(g_pKZUtils->GetCurrentMapName().Get());
		std::string server = db->Escape(ServerID());
		std::string cleanDetails = db->Escape(details.c_str());
		u64 steamID64 = player->GetSteamId64();
		f64 distance = jump->GetDistance();

		std::vector<char> insert(cleanDetails.size() + map.size() + 1024);
		V_snprintf(insert.data(), (int)insert.size(), sql_jumptop_insert, steamID64, modeID, jumpType, isBlock ? 1 : 0, block, distance,
				   jump->GetStrafeCount(), (f64)jump->GetSync(), (f64)jump->GetTakeoffSpeed(), (f64)jump->GetMaxSpeed(), (f64)jump->airtime,
				   (f64)jump->GetMaxHeight(), (f64)jump->GetOffset(), map.c_str(), server.c_str(), replayUuid.c_str(), cleanDetails.c_str());

		char rank[2048];
		V_snprintf(rank, sizeof(rank), sql_jumptop_rank, modeID, jumpType, isBlock ? 1 : 0, block, block, distance, steamID64, modeID, jumpType,
				   isBlock ? 1 : 0);

		Transaction txn;
		txn.queries.push_back(insert.data());
		txn.queries.push_back(rank);

		CPlayerUserId userID = player->GetClient()->GetUserID();
		std::string name = player->GetName();
		// clang-format off
		db->ExecuteTransaction(
			txn,
			[userID, name, steamID64, modeID, jumpType, isBlock, block, distance, replayUuid](std::vector<ISQLQuery *> queries)
			{
				u32 id = queries[0]->GetInsertId();
				i32 place = 0;
				i32 total = 0;
				ISQLResult *result = queries[1]->GetResultSet();
				if (result && result->FetchRow())
				{
					place = result->GetInt(0) + 1;
					total = result->GetInt(1);
				}
				KZ_LOG_INFO(LogChannel::DB, "[cyb] jumptop_pb id=%u steam_id=%llu mode=%d type=%d block=%d dist=%.4f place=%d total=%d\n", id,
							steamID64, modeID, jumpType, isBlock ? block : 0, distance, place, total);

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
				if (player)
				{
					std::string label = JumpLabel(player->languageService->GetLanguage(), jumpType, isBlock, block, distance);
					player->languageService->PrintChat(true, false, "Jumptop - New PB", label.c_str(), mode.c_str(), place, total);
				}
				if (place == 1)
				{
					for (i32 i = 0; i <= MAXPLAYERS; i++)
					{
						KZPlayer *other = g_pKZPlayerManager->ToPlayer(i);
						if (!other || !other->IsInGame() || other->IsFakeClient())
						{
							continue;
						}
						std::string label = JumpLabel(other->languageService->GetLanguage(), jumpType, isBlock, block, distance);
						other->languageService->PrintChat(true, false, "Jumptop - New Server Record", name.c_str(), label.c_str(), mode.c_str());
					}
					PlaySoundToAll(distanceTierSounds[DistanceTier_Wrecker]);
				}
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
			u64 steamID64 = (u64)r->GetInt64(1);
			std::string alias = r->GetString(2) ? r->GetString(2) : "";
			i32 mode = r->GetInt(3);
			i32 type = r->GetInt(4);
			bool isBlock = r->GetInt(5) != 0;
			i32 block = r->GetInt(6);
			f64 distance = ResultDouble(r, 7);
			std::string replay = r->GetString(17) ? r->GetString(17) : "";
			std::string label = JumpLabel(player->languageService->GetLanguage(), type, isBlock, block, distance);
			std::string modeName = ModeShortName(mode);

			char line[1024];
			V_snprintf(line, sizeof(line),
					   "[jumptop #%d] %s %s | %s (%llu) | strafes %d | sync %.1f%% | pre %.2f | max %.2f | air %.3f | height %.2f | offset %.2f"
					   " | map %s | server %s | replay %s | %s%s\n",
					   id, label.c_str(), modeName.c_str(), alias.c_str(), steamID64, r->GetInt(8), ResultDouble(r, 9) * 100.0,
					   ResultDouble(r, 10), ResultDouble(r, 11), ResultDouble(r, 12), ResultDouble(r, 13), ResultDouble(r, 14),
					   r->GetString(15) ? r->GetString(15) : "", r->GetString(16) ? r->GetString(16) : "", replay.c_str(),
					   r->GetString(20) ? r->GetString(20) : "", r->GetInt(19) ? " | REMOVED" : "");
			utils::PrintConsole(player->GetController(), "%s", line);
			// Полная статистика (JSON по стрейфам) — кусками: строка консоли ограничена.
			const char *details = r->GetString(18) ? r->GetString(18) : "";
			std::string all = details;
			for (size_t off = 0; off < all.size(); off += 900)
			{
				utils::PrintConsole(player->GetController(), "%s", all.substr(off, 900).c_str());
			}
			utils::PrintConsole(player->GetController(), "\n");

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
 * Меню !jumptop: [режим] + 14 пунктов (7 типов × дистанция/блоки) → топ-20 → выбор строки =
 * статистика в консоль и реплей ботом (как в GOKZ).
 */
namespace
{
	MenuHandle g_rootMenu[MAXPLAYERS + 1] = {};
	MenuHandle g_topMenu[MAXPLAYERS + 1] = {};
	i32 g_menuMode[MAXPLAYERS + 1] = {};

	void ResetMenu(MenuHandle *slotMenu)
	{
		if (*slotMenu != kInvalidMenuHandle)
		{
			g_pMenus->DestroyMenu(*slotMenu);
			*slotMenu = kInvalidMenuHandle;
		}
	}

	void OpenRootMenu(KZPlayer *player, i32 modeID);

	void OnTopMenuSelect(MenuHandle menu, int slot, int item)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		const char *info = g_pMenus->GetItemInfo(menu, item);
		if (!player || !info || !info[0])
		{
			return;
		}
		PrintJumpInfo(player, V_StringToInt32(info, 0), true);
	}

	void ShowTop(KZPlayer *player, i32 modeID, i32 jumpType, bool isBlock)
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
			[userID, modeID, jumpType, isBlock](std::vector<ISQLQuery *> queries)
			{
				KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
				if (!player)
				{
					return;
				}
				const char *lang = player->languageService->GetLanguage();
				std::string mode = ModeShortName(modeID);
				std::string kind = KZLanguageService::PrepareMessageWithLang(lang, isBlock ? "Jumptop - Kind Block" : "Jumptop - Kind Distance");
				std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Top Title", jumpTypeShortStr[jumpType], kind.c_str(), mode.c_str());

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
					char text[256];
					if (isBlock)
					{
						V_snprintf(text, sizeof(text), "#%d  %d (%.4f)  %s", place, block, distance, alias.c_str());
					}
					else
					{
						V_snprintf(text, sizeof(text), "#%d  %.4f  %s", place, distance, alias.c_str());
					}
					char info[16];
					V_snprintf(info, sizeof(info), "%d", id);
					rows.emplace_back(text, info);
					utils::PrintConsole(player->GetController(),
										"%s | %llu | strafes %d | sync %.1f%% | pre %.2f | max %.2f | air %.3f | %s | id %d\n", text,
										(u64)r->GetInt64(1), r->GetInt(5), ResultDouble(r, 6) * 100.0, ResultDouble(r, 7), ResultDouble(r, 8),
										ResultDouble(r, 9), r->GetString(11) ? r->GetString(11) : "", id);
				}
				if (rows.empty())
				{
					player->languageService->PrintChat(true, false, "Jumptop - Empty", jumpTypeShortStr[jumpType], kind.c_str(), mode.c_str());
					return;
				}
				int slot = player->GetPlayerSlot().Get();
				if (g_pMenus == nullptr || slot < 0 || slot > MAXPLAYERS)
				{
					player->languageService->PrintChat(true, false, "Jumptop - See Console");
					return;
				}
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
				g_pMenus->SetCloseOnSelect(m, true);
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

	i32 NextModeID(i32 modeID)
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
		for (i32 step = 1; step <= count; step++)
		{
			i32 id = KZ::mode::GetModeInfo(CUtlString(jumptopModes[(current + step + count) % count])).databaseID;
			if (id >= 0)
			{
				return id;
			}
		}
		return modeID;
	}

	void OnRootMenuSelect(MenuHandle menu, int slot, int item)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		const char *info = g_pMenus->GetItemInfo(menu, item);
		if (!player || !info || !info[0] || slot < 0 || slot > MAXPLAYERS)
		{
			return;
		}
		if (V_strcmp(info, "mode") == 0)
		{
			OpenRootMenu(player, NextModeID(g_menuMode[slot]));
			return;
		}
		i32 code = V_StringToInt32(info, -1);
		if (code < 0)
		{
			return;
		}
		ShowTop(player, g_menuMode[slot], code / 2, (code % 2) == 1);
	}

	void OpenRootMenu(KZPlayer *player, i32 modeID)
	{
		int slot = player->GetPlayerSlot().Get();
		if (slot < 0 || slot > MAXPLAYERS)
		{
			return;
		}
		ResetMenu(&g_rootMenu[slot]);
		g_menuMode[slot] = modeID;
		const char *lang = player->languageService->GetLanguage();
		std::string mode = ModeShortName(modeID);
		std::string title = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Menu Title", mode.c_str());
		MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title.c_str(), &OnRootMenuSelect);
		if (m == kInvalidMenuHandle)
		{
			return;
		}
		std::string modeItem = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Menu Mode", mode.c_str());
		g_pMenus->AddItem(m, modeItem.c_str(), "mode", false);
		std::string blockWord = KZLanguageService::PrepareMessageWithLang(lang, "Jumptop - Kind Block");
		for (i32 type = JumpType_LongJump; type <= JumpType_Jumpbug; type++)
		{
			char info[8];
			V_snprintf(info, sizeof(info), "%d", type * 2);
			g_pMenus->AddItem(m, jumpTypeStr[type], info, false);
			char blockText[64];
			V_snprintf(blockText, sizeof(blockText), "%s — %s", jumpTypeShortStr[type], blockWord.c_str());
			V_snprintf(info, sizeof(info), "%d", type * 2 + 1);
			g_pMenus->AddItem(m, blockText, info, false);
		}
		g_pMenus->SetCloseOnSelect(m, true);
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
					utils::PrintConsole(player->GetController(), "[jumptop #%d] %s %s | strafes %d | sync %.1f%% | pre %.2f | max %.2f | air %.3f\n",
										r->GetInt(0), label.c_str(), mode.c_str(), r->GetInt(5), ResultDouble(r, 6) * 100.0, ResultDouble(r, 7),
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
	OpenRootMenu(player, modeID);
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
	if (args.ArgC() != 2)
	{
		META_CONPRINTF("Usage: kz_jumptop_remove <id>\n");
		return;
	}
	SetJumpRemoved(V_StringToInt32(args.Arg(1), 0), true);
}

CON_COMMAND_F(kz_jumptop_restore, "Вернуть снятый прыжок в джамптоп по ID.", FCVAR_NONE)
{
	if (args.ArgC() != 2)
	{
		META_CONPRINTF("Usage: kz_jumptop_restore <id>\n");
		return;
	}
	SetJumpRemoved(V_StringToInt32(args.Arg(1), 0), false);
}
