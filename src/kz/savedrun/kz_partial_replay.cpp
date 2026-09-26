#include "kz_partial_replay.h"

#include "cs2kz.h"
#include "kz/kz.h"
#include "kz_savedrun.h"
#include "kz/global/kz_global.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/option/kz_option.h"
#include "kz/outbox/kz_outbox.h"
#include "kz/recording/kz_recording.h"
#include "kz/replays/compression.h"
#include "kz/replays/cyb_replay_common.h"
#include "kz/replays/kz_replay.h"
#include "kz/timer/kz_timer.h"
#include "utils/async_file_io.h"
#include "utils/http.h"
#include "utils/json.h"
#include "utils/utils.h"
#include "utils/uuid.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <unordered_map>

#define KZ_PARTIAL_REPLAY_PATH KZ_REPLAY_PATH "/partial"

namespace
{
	// Префикс файла куска: 8 байт магии + id куска (UUID строкой, 36 символов), дальше — обычный
	// файл реплея (Recorder::WriteToMemory). Id внутри файла, а не в имени: файл назван ключом
	// SavedRuns (одна перезапись на ключ), а сверка с полем "rp" снапшота держит принадлежность.
	constexpr char kMagic[8] = {'K', 'Z', 'P', 'A', 'R', 'T', '0', '1'};
	constexpr size_t kIdLength = 36;
	constexpr size_t kPrefixLength = sizeof(kMagic) + kIdLength;
	// TTL куска — тот же, что у строки SavedRuns (queries/savedruns.h, PurgeExpiredSavedRuns).
	constexpr i64 kTtlSeconds = 30ll * 24 * 60 * 60;

	// Имя файла/ключ очереди: только [A-Za-z0-9_-] — в ключе бывают стили через запятую, а точка
	// в имени сломала бы разбор имени файла очереди outbox (суффикс .partial.meta).
	std::string Sanitize(const std::string &in)
	{
		std::string out;
		out.reserve(in.size());
		for (char c : in)
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
			out.push_back(ok ? c : '-');
		}
		return out;
	}

	std::string Stem(const KZPartialReplay::Key &key)
	{
		return std::to_string(key.steamId64) + "_" + Sanitize(key.map) + "_" + std::to_string(key.course) + "_" + Sanitize(key.mode) + "_"
			   + (key.styles.empty() ? std::string("none") : Sanitize(key.styles));
	}

	// Путь относительно csgo/ (такой ждут utils::*File и AsyncFileIO).
	std::string LocalPath(const KZPartialReplay::Key &key)
	{
		return std::string(KZ_PARTIAL_REPLAY_PATH "/") + Stem(key) + ".replay";
	}

	std::string AbsPath(const std::string &relPath)
	{
		char abs[1024];
		V_snprintf(abs, sizeof(abs), "%s/csgo/%s", Plat_GetGameDirectory(), relPath.c_str());
		return abs;
	}

	bool LocalFileExists(const std::string &relPath)
	{
		std::error_code ec;
		return std::filesystem::exists(AbsPath(relPath), ec);
	}

	// Кусок уходит в api, только если ключ проходит его валидацию (как у PB-реплеев,
	// cyb_replay_upload.cpp): режим из трёх api-режимов, имя карты [a-z0-9_-], курс smallint,
	// стили [A-Za-z0-9,_-]{0,64}. Остальное живёт только на диске этого сервера.
	bool ApiEligible(const KZPartialReplay::Key &key, std::string &outApiMode)
	{
		const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url || url[0] == '\0')
		{
			return false;
		}
		outApiMode = CybReplayCommon::MapMode(key.mode);
		if (outApiMode.empty() || !CybReplayCommon::IsValidMapName(key.map) || key.course < 0 || key.course > 32767 || key.styles.size() > 64)
		{
			return false;
		}
		for (char c : key.styles)
		{
			const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ',';
			if (!ok)
			{
				return false;
			}
		}
		return true;
	}

	KZOutboxService::PartialMeta MakeMeta(const KZPartialReplay::Key &key, const std::string &apiMode, const char *op, const std::string &partialId)
	{
		KZOutboxService::PartialMeta meta;
		meta.op = op;
		meta.partialId = partialId;
		meta.steamId64 = key.steamId64;
		meta.map = key.map;
		meta.course = key.course;
		meta.mode = apiMode;
		meta.styles = key.styles;
		meta.path = LocalPath(key);
		return meta;
	}

	// Разобранный кусок. Сабтики сразу упакованы так, как их хранит Recorder (счётчики + плоский
	// массив ходов): разбор идёт на рабочем потоке, а главному остаётся только переставить векторы.
	struct Parsed
	{
		bool ok = false;
		const char *reason = "";
		std::string id;
		ReplayHeader header;
		std::vector<TickData> ticks;
		std::vector<u8> subtickCounts;
		std::vector<SubtickData::RpSubtickMove> subtickMoves;
		std::vector<std::pair<i32, EconInfo>> weapons;
		std::vector<RpJumpStats> jumps;
		std::vector<RpEvent> events;
		std::vector<CmdData> cmds;
		std::vector<u8> cmdSubtickCounts;
		std::vector<SubtickData::RpSubtickMove> cmdSubtickMoves;
	};

	void Pack(const std::vector<SubtickData> &in, std::vector<u8> &outCounts, std::vector<SubtickData::RpSubtickMove> &outMoves)
	{
		outCounts.reserve(in.size());
		for (const SubtickData &sd : in)
		{
			const u8 count = (u8)MIN(sd.numSubtickMoves, MAX_SUBTICK_MOVES);
			outCounts.push_back(count);
			for (u8 j = 0; j < count; j++)
			{
				outMoves.push_back(sd.subtickMoves[j]);
			}
		}
	}

	// Разбор файла куска. РАБОЧИЙ поток: ни логов, ни глобального состояния — причина отказа
	// уезжает в Parsed::reason и печатается на главном.
	void ParseContainer(const char *data, size_t size, Parsed &out)
	{
		if (!data || size < kPrefixLength + sizeof(u32))
		{
			out.reason = "short_file";
			return;
		}
		if (memcmp(data, kMagic, sizeof(kMagic)) != 0)
		{
			out.reason = "bad_magic";
			return;
		}
		out.id.assign(data + sizeof(kMagic), kIdLength);

		const char *cursor = data + kPrefixLength;
		const char *end = data + size;

		u32 headerSize = 0;
		memcpy(&headerSize, cursor, sizeof(headerSize));
		cursor += sizeof(headerSize);
		if (headerSize == 0 || headerSize > 5 * 1024 * 1024 || (size_t)(end - cursor) < headerSize)
		{
			out.reason = "bad_header";
			return;
		}
		if (!out.header.ParseFromString(std::string(cursor, cursor + headerSize)))
		{
			out.reason = "bad_header";
			return;
		}
		cursor += headerSize;
		const u32 version = out.header.version();
		if (version < 1 || version > KZ_REPLAY_VERSION)
		{
			out.reason = "unsupported_version";
			return;
		}

		std::vector<SubtickData> subticks;
		std::vector<SubtickData> cmdSubticks;
		// Порядок секций — как у Recorder::WriteToMemory.
		if (!KZ::replaysystem::compression::ReadTickDataCompressed(cursor, end, out.ticks, subticks, version)
			|| !KZ::replaysystem::compression::ReadWeaponsCompressed(cursor, end, out.weapons)
			|| !KZ::replaysystem::compression::ReadJumpsCompressed(cursor, end, out.jumps, version)
			|| !KZ::replaysystem::compression::ReadEventsCompressed(cursor, end, out.events)
			|| !KZ::replaysystem::compression::ReadCmdDataCompressed(cursor, end, out.cmds, cmdSubticks, version))
		{
			out.reason = "bad_sections";
			return;
		}
		if (out.ticks.empty() || subticks.size() != out.ticks.size() || cmdSubticks.size() != out.cmds.size())
		{
			out.reason = "bad_sections";
			return;
		}
		for (size_t i = 1; i < out.ticks.size(); i++)
		{
			if (out.ticks[i].serverTick < out.ticks[i - 1].serverTick)
			{
				out.reason = "ticks_not_monotonic";
				return;
			}
		}
		Pack(subticks, out.subtickCounts, out.subtickMoves);
		std::vector<SubtickData>().swap(subticks);
		Pack(cmdSubticks, out.cmdSubtickCounts, out.cmdSubtickMoves);
		out.ok = true;
	}

	// Чтение целиком без логов (рабочий поток; utils::ReadBufferFromFile пишет WARN на промахе).
	bool ReadWholeFile(const std::string &absPath, std::vector<char> &out)
	{
		FILE *fp = fopen(absPath.c_str(), "rb");
		if (!fp)
		{
			return false;
		}
		bool ok = fseek(fp, 0, SEEK_END) == 0;
		long size = ok ? ftell(fp) : -1;
		ok = ok && size > 0 && fseek(fp, 0, SEEK_SET) == 0;
		if (ok)
		{
			out.resize((size_t)size);
			ok = fread(out.data(), 1, (size_t)size, fp) == (size_t)size;
		}
		fclose(fp);
		return ok;
	}

	// Контекст склейки: всё, что нужно колбэкам после асинхронной загрузки. Игрок за это время
	// мог уйти — поэтому userID + steamId64, а рекордер ищется по uuid (вектор рекордеров
	// перекладывается, указатели на него не живут).
	struct SpliceContext
	{
		explicit SpliceContext(CPlayerUserId id) : userID(id) {}

		CPlayerUserId userID;
		u64 steamId64 {};
		UUID_t recorderUuid;
		KZPartialReplay::Key key;
		std::string partialId;
		f64 restoredTime {};
	};

	KZPlayer *FindPlayer(const SpliceContext &ctx)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(ctx.userID);
		if (!player || player->GetSteamId64() != ctx.steamId64)
		{
			return nullptr;
		}
		return player;
	}

	// Живой рекордер этой склейки (ещё ждёт кусок) либо nullptr: ран могли остановить или
	// закончить, пока кусок ехал.
	RunRecorder *FindPendingRecorder(KZPlayer *player, const UUID_t &uuid)
	{
		for (RunRecorder &rec : player->recordingService->runRecorders)
		{
			if (rec.splicePending && rec.desiredStopTime < 0.0f && rec.uuid == uuid)
			{
				return &rec;
			}
		}
		return nullptr;
	}

	// Отказ склейки: рекордер без начала рана файлом стать не имеет права — снимаем его, ран
	// остаётся без реплея (как было до склейки).
	void Abort(const SpliceContext &ctx, const char *reason)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[cyb] partial_replay_splice_failed steam_id=%llu key=%s partial=%s reason=%s\n",
					(unsigned long long)ctx.steamId64, Stem(ctx.key).c_str(), ctx.partialId.c_str(), reason);
		KZPlayer *player = FindPlayer(ctx);
		if (!player)
		{
			return;
		}
		auto &recorders = player->recordingService->runRecorders;
		for (auto it = recorders.begin(); it != recorders.end(); ++it)
		{
			if (it->splicePending && it->uuid == ctx.recorderUuid)
			{
				recorders.erase(it);
				break;
			}
		}
	}

	void Apply(const SpliceContext &ctx, Parsed &p, const char *source)
	{
		KZPlayer *player = FindPlayer(ctx);
		if (!player)
		{
			return; // ушёл — рекордер умер вместе с ним
		}
		RunRecorder *rec = FindPendingRecorder(player, ctx.recorderUuid);
		if (!rec)
		{
			KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_splice_skipped steam_id=%llu partial=%s reason=run_gone\n",
						(unsigned long long)ctx.steamId64, ctx.partialId.c_str());
			return;
		}
		// Живые кадры уже ушли на диск частями (15 минут ожидания куска) — вставить кусок в начало
		// нечем. Практически недостижимо, но склеивать «через» сброшенные части нельзя.
		if (rec->numFlushedChunks > 0)
		{
			Abort(ctx, "recorder_flushed");
			return;
		}
		const KZCourseDescriptor *course = player->timerService->GetCourse();
		if (!course)
		{
			Abort(ctx, "no_course");
			return;
		}
		// Кусок обязан быть записью ЭТОГО рана: тип, игрок, курс и режим — те же.
		if (p.header.type() != cs2kz::replay::RP_RUN || p.header.player().steamid64() != ctx.steamId64
			|| p.header.run().course_name() != rec->replayHeader.run().course_name()
			|| p.header.run().mode().short_name() != rec->replayHeader.run().mode().short_name())
		{
			Abort(ctx, "header_mismatch");
			return;
		}

		// Старт рана — ПОСЛЕДНИЙ TIMER_START куска (предзапись может нести чужие, см.
		// playback::RunWindowFromEvents). Его id курса переписываем на id этой сессии: окно рана
		// сверяет пару START/END по id, а на другом сервере/после ребилда карты id мог сдвинуться
		// (восстановление ищет курс по cyber-номеру, а не по id).
		i64 startEvent = -1;
		for (i64 i = (i64)p.events.size() - 1; i >= 0; i--)
		{
			const RpEvent &e = p.events[i];
			if (e.type == RPEVENT_TIMER_EVENT && e.data.timer.type == RpEvent::RpEventData::TimerEvent::TIMER_START)
			{
				startEvent = i;
				break;
			}
		}
		if (startEvent < 0)
		{
			Abort(ctx, "no_timer_start");
			return;
		}
		p.events[startEvent].data.timer.index = course->id;

		// Пауза на конце куска: выход из паузы или из prac (там кусок кончается открытой парой
		// PAUSE) — вторую паузу не открываем, её закроет RESUME восстановленного рана.
		bool pausedAtEnd = false;
		for (const RpEvent &e : p.events)
		{
			if (e.type != RPEVENT_TIMER_EVENT)
			{
				continue;
			}
			switch (e.data.timer.type)
			{
				case RpEvent::RpEventData::TimerEvent::TIMER_PAUSE:
					pausedAtEnd = true;
					break;
				case RpEvent::RpEventData::TimerEvent::TIMER_RESUME:
				case RpEvent::RpEventData::TimerEvent::TIMER_START:
				case RpEvent::RpEventData::TimerEvent::TIMER_END:
				case RpEvent::RpEventData::TimerEvent::TIMER_STOP:
					pausedAtEnd = false;
					break;
				default:
					break;
			}
		}

		// Оружие: индексы в кадрах куска — индексы списка ТОЙ сессии; перекладываем в список этой.
		KZRecordingService *rs = player->recordingService;
		std::unordered_map<i32, i32> weaponRemap;
		for (const auto &entry : p.weapons)
		{
			i32 mapped = -1;
			for (i32 i = 0; i < (i32)rs->weapons.size(); i++)
			{
				if (rs->weapons[i] == entry.second)
				{
					mapped = i;
					break;
				}
			}
			if (mapped < 0)
			{
				rs->weapons.push_back(entry.second);
				mapped = (i32)rs->weapons.size() - 1;
			}
			weaponRemap[entry.first] = mapped;
		}
		for (TickData &t : p.ticks)
		{
			if (t.weapon >= 0)
			{
				auto it = weaponRemap.find(t.weapon);
				t.weapon = it != weaponRemap.end() ? it->second : -1;
			}
		}

		// Стык: живые тики ложатся сразу за последним кадром куска.
		p.ticks.back().post.replayFlags.splice = true;
		const u32 spliceTick = p.ticks.back().serverTick + 1;
		rec->tickShiftTo = spliceTick;
		rec->tickShiftActive = true;
		for (TickData &t : rec->tickData)
		{
			t.serverTick = rec->ShiftTick(t.serverTick);
		}
		for (RpEvent &e : rec->rpEvents)
		{
			e.serverTick = rec->ShiftTick(e.serverTick);
		}
		for (RpJumpStats &j : rec->jumps)
		{
			j.overall.serverTick = rec->ShiftTick(j.overall.serverTick);
		}
		for (CmdData &c : rec->cmdData)
		{
			c.serverTick = rec->ShiftTick(c.serverTick);
		}

		// Кусок — в начало, живое — следом. Векторы куска переезжают целиком, копируется только
		// то, что рекордер успел записать с момента восстановления (секунды).
		const size_t partialFrames = p.ticks.size();
		p.ticks.insert(p.ticks.end(), rec->tickData.begin(), rec->tickData.end());
		rec->tickData.swap(p.ticks);
		p.subtickCounts.insert(p.subtickCounts.end(), rec->subtickCounts.begin(), rec->subtickCounts.end());
		rec->subtickCounts.swap(p.subtickCounts);
		p.subtickMoves.insert(p.subtickMoves.end(), rec->subtickMoves.begin(), rec->subtickMoves.end());
		rec->subtickMoves.swap(p.subtickMoves);

		if (!pausedAtEnd)
		{
			// Пауза стыка: восстановленный ран стоит на ForcePause (ApplySnapshot), и её RESUME
			// придёт живым событием, когда игрок продолжит. Время — время рана из снапшота.
			RpEvent pause = {};
			pause.type = RPEVENT_TIMER_EVENT;
			pause.serverTick = spliceTick;
			pause.data.timer.type = RpEvent::RpEventData::TimerEvent::TIMER_PAUSE;
			pause.data.timer.index = -1;
			pause.data.timer.time = (f32)ctx.restoredTime;
			p.events.push_back(pause);
			// ForcePause не поставил паузу (вето листенера) — стык всё равно обязан быть парой,
			// иначе вся запись после него считалась бы паузой.
			if (!player->timerService->GetPaused())
			{
				RpEvent resume = pause;
				resume.serverTick = spliceTick + 1;
				resume.data.timer.type = RpEvent::RpEventData::TimerEvent::TIMER_RESUME;
				p.events.push_back(resume);
			}
		}
		p.events.insert(p.events.end(), rec->rpEvents.begin(), rec->rpEvents.end());
		rec->rpEvents.swap(p.events);
		p.jumps.insert(p.jumps.end(), rec->jumps.begin(), rec->jumps.end());
		rec->jumps.swap(p.jumps);
		p.cmds.insert(p.cmds.end(), rec->cmdData.begin(), rec->cmdData.end());
		rec->cmdData.swap(p.cmds);
		p.cmdSubtickCounts.insert(p.cmdSubtickCounts.end(), rec->cmdSubtickCounts.begin(), rec->cmdSubtickCounts.end());
		rec->cmdSubtickCounts.swap(p.cmdSubtickCounts);
		p.cmdSubtickMoves.insert(p.cmdSubtickMoves.end(), rec->cmdSubtickMoves.begin(), rec->cmdSubtickMoves.end());
		rec->cmdSubtickMoves.swap(p.cmdSubtickMoves);

		rec->totalTicksRecorded += (u32)partialFrames;
		rec->splicePending = false;
		KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_spliced steam_id=%llu key=%s partial=%s source=%s frames=%zu paused_at_end=%d\n",
					(unsigned long long)ctx.steamId64, Stem(ctx.key).c_str(), ctx.partialId.c_str(), source, partialFrames, (int)pausedAtEnd);
	}

	void OnLoaded(const SpliceContext &ctx, Parsed &p, bool fromLocal);

	// Разбор буфера на рабочем потоке и склейка на главном. writeLocal — кусок только что
	// докачан: заодно положить его на диск этого сервера (повторный выход/возврат сюда же).
	void ParseAsync(const SpliceContext &ctx, std::shared_ptr<std::vector<char>> buffer, bool fromLocal, bool writeLocal)
	{
		if (!KZRecordingService::fileWriter)
		{
			Abort(ctx, "no_file_writer");
			return;
		}
		const std::string relPath = LocalPath(ctx.key);
		KZRecordingService::fileWriter->QueueTask(
			[ctx, buffer, fromLocal, writeLocal, relPath]() -> std::function<void()>
			{
				auto parsed = std::make_shared<Parsed>();
				if (!buffer)
				{
					std::vector<char> local;
					if (ReadWholeFile(AbsPath(relPath), local))
					{
						ParseContainer(local.data(), local.size(), *parsed);
					}
					else
					{
						parsed->reason = "no_local_file";
					}
				}
				else
				{
					ParseContainer(buffer->data(), buffer->size(), *parsed);
					if (parsed->ok && writeLocal)
					{
						utils::WriteBufferToFile(relPath.c_str(), *buffer);
					}
				}
				return [ctx, parsed, fromLocal]() { OnLoaded(ctx, *parsed, fromLocal); };
			});
	}

	// Докачка куска из api: GET /replays/v1/partial (сверка id на стороне api) → публичный URL.
	void LoadRemote(const SpliceContext &ctx)
	{
		std::string apiMode;
		if (!ApiEligible(ctx.key, apiMode))
		{
			Abort(ctx, "no_local_file");
			return;
		}
		std::string url = KZOptionService::GetOptionStr("cybEmitUrl", "");
		if (!url.empty() && url.back() == '/')
		{
			url.pop_back();
		}
		HTTP::Request req(HTTP::Method::GET, url + "/replays/v1/partial");
		req.SetQuery("steamId64", std::to_string(ctx.key.steamId64));
		req.SetQuery("map", ctx.key.map);
		req.SetQuery("course", std::to_string(ctx.key.course));
		req.SetQuery("mode", apiMode);
		req.SetQuery("styles", ctx.key.styles);
		req.SetQuery("partialId", ctx.partialId);
		const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
		if (token && token[0] != '\0')
		{
			req.SetHeader("Authorization", std::string("Bearer ") + token);
		}
		// clang-format off
		req.Send(
			[ctx](HTTP::Response resp)
			{
				if (resp.status < 200 || resp.status >= 300)
				{
					Abort(ctx, resp.status == 404 ? "remote_not_found" : "remote_http_error");
					return;
				}
				std::optional<std::string> body = resp.Body();
				Json json(body.value_or(""));
				std::string downloadUrl, remoteId;
				if (!json.IsValid() || !json.Get("url", downloadUrl) || !json.Get("partialId", remoteId) || downloadUrl.empty()
					|| remoteId != ctx.partialId)
				{
					Abort(ctx, "remote_bad_response");
					return;
				}
				HTTP::Request download(HTTP::Method::GET, downloadUrl);
				download.Send(
					[ctx](HTTP::Response file)
					{
						if (file.status < 200 || file.status >= 300)
						{
							Abort(ctx, "download_http_error");
							return;
						}
						std::optional<std::vector<char>> raw = file.RawBody();
						if (!raw.has_value() || raw->empty() || raw->size() >= KZOutboxService::maxReplayBytes)
						{
							Abort(ctx, "download_bad_body");
							return;
						}
						ParseAsync(ctx, std::make_shared<std::vector<char>>(std::move(*raw)), false, true);
					},
					[ctx]() { Abort(ctx, "download_network_error"); });
			},
			[ctx]() { Abort(ctx, "remote_network_error"); });
		// clang-format on
	}

	void OnLoaded(const SpliceContext &ctx, Parsed &p, bool fromLocal)
	{
		if (p.ok && p.id == ctx.partialId)
		{
			Apply(ctx, p, fromLocal ? "local" : "remote");
			return;
		}
		if (fromLocal)
		{
			// Локального куска нет или он чужой (устарел: ран с этим ключом потом бежал на другом
			// сервере) — идём в api.
			if (p.ok)
			{
				KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_local_stale steam_id=%llu partial=%s local=%s\n",
							(unsigned long long)ctx.steamId64, ctx.partialId.c_str(), p.id.c_str());
			}
			LoadRemote(ctx);
			return;
		}
		Abort(ctx, p.ok ? "remote_id_mismatch" : p.reason);
	}
} // namespace

bool KZPartialReplay::MakeKey(KZPlayer *player, i32 courseNumber, Key &out)
{
	if (!player || !player->IsAuthenticated())
	{
		return false;
	}
	bool mapNameOk = false;
	CUtlString mapName = g_pKZUtils->GetCurrentMapName(&mapNameOk);
	if (!mapNameOk || mapName.IsEmpty())
	{
		return false;
	}
	out.steamId64 = player->GetSteamId64();
	out.map = mapName.Get();
	out.course = courseNumber;
	out.mode = KZ::mode::GetModeInfo(player->modeService).shortModeName.Get();
	out.styles = KZSavedRunService::BuildStylesString(player).Get();
	return true;
}

void KZPartialReplay::SaveOnDisconnect(KZPlayer *player, const Key &key, std::string &outPartialId)
{
	outPartialId.clear();
	KZRecordingService *rs = player->recordingService;
	auto it = rs->runRecorders.begin();
	for (; it != rs->runRecorders.end(); ++it)
	{
		if (it->desiredStopTime < 0.0f)
		{
			break;
		}
	}
	if (it == rs->runRecorders.end())
	{
		return; // рекордера нет (карта без записи ранов, ран без реплея) — куска не будет
	}
	if (it->splicePending)
	{
		// Кусок прошлого выхода ещё не вставлен (игрок вышел сразу после восстановления): прежний
		// кусок остаётся в силе — он покрывает ран до прошлого выхода, а с тех пор игрок стоял на
		// паузе восстановления.
		outPartialId = it->splicePartialId;
		rs->runRecorders.erase(it);
		KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_kept steam_id=%llu key=%s partial=%s reason=splice_pending\n",
					(unsigned long long)key.steamId64, Stem(key).c_str(), outPartialId.c_str());
		return;
	}
	if (!KZRecordingService::fileWriter)
	{
		return;
	}

	const std::string partialId = UUID_t().ToString();
	if (partialId.size() != kIdLength)
	{
		return;
	}
	auto rec = std::make_shared<RunRecorder>(std::move(*it));
	rs->runRecorders.erase(it);
	// Весь список оружия сессии: кадры куска могли уйти на диск частями, и отбор «упомянутых в
	// памяти» (CopyWeaponsToRecorder) их бы не увидел. Индексы — индексы этого списка.
	rec->weaponTable.clear();
	for (i32 i = 0; i < (i32)rs->weapons.size(); i++)
	{
		rec->weaponTable.push_back({i, rs->weapons[i]});
	}
	outPartialId = partialId;

	const std::string relPath = LocalPath(key);
	const std::string stem = Stem(key);
	std::string apiMode;
	const bool upload = ApiEligible(key, apiMode);
	KZOutboxService::PartialMeta meta = MakeMeta(key, apiMode, "put", partialId);
	const u64 steamId64 = key.steamId64;

	// Сериализация и запись — на рабочем потоке (длинный ран — десятки МБ): префикс пишется в
	// буфер ДО реплея, лишней копии нет. Аплоад — с главного (HTTP и outbox живут там).
	KZRecordingService::fileWriter->QueueTask(
		[rec, partialId, relPath, stem, upload, meta, steamId64]() -> std::function<void()>
		{
			auto buffer = std::make_shared<std::vector<char>>();
			buffer->insert(buffer->end(), kMagic, kMagic + sizeof(kMagic));
			buffer->insert(buffer->end(), partialId.begin(), partialId.end());
			const size_t frames = rec->totalTicksRecorded;
			const bool serialized = rec->WriteToMemory(*buffer);
			const bool written = serialized && utils::WriteBufferToFile(relPath.c_str(), *buffer);
			return [buffer, partialId, stem, upload, meta, steamId64, frames, written]()
			{
				if (!written)
				{
					KZ_LOG_WARN(LogChannel::Replays, "[cyb] partial_replay_save_failed steam_id=%llu key=%s partial=%s\n",
								(unsigned long long)steamId64, stem.c_str(), partialId.c_str());
					return;
				}
				KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_saved steam_id=%llu key=%s partial=%s frames=%zu bytes=%zu upload=%d\n",
							(unsigned long long)steamId64, stem.c_str(), partialId.c_str(), frames, buffer->size(), (int)upload);
				if (!upload)
				{
					return;
				}
				KZOutboxService::EnqueuePartial(stem, meta);
				KZOutboxService::SendPartial(stem, meta, buffer);
			};
		});
}

void KZPartialReplay::ResumeAfterRestore(KZPlayer *player, const Key &key, const std::string &partialId, f64 restoredTime)
{
	if (partialId.empty() || !KZRecordingService::fileWriter)
	{
		return;
	}
	// Та же политика, что у OnTimerStart: сервер не пишет раны на неглобальных картах — не пишем и
	// восстановленный.
	if (!KZOptionService::GetOptionInt("recordNonGlobalRuns", true) && KZGlobalService::IsCurrentMapConfirmedNotGlobal())
	{
		return;
	}
	if (!player->timerService->GetCourse() || !player->GetPlayerPawn())
	{
		return;
	}
	KZRecordingService *rs = player->recordingService;
	for (const RunRecorder &existing : rs->runRecorders)
	{
		if (existing.desiredStopTime < 0.0f)
		{
			return; // живой рекордер уже есть — ран не наш (не должно случаться: таймер не бежал)
		}
	}

	rs->runRecorders.push_back(RunRecorder(player));
	RunRecorder &rec = rs->runRecorders.back();
	// Предзапись кольцевого буфера (спаун, телепорт восстановления) — не начало рана: начало
	// приедет куском. Шапку (курс/режим/стили/игрок) оставляем.
	rec.tickData.clear();
	rec.subtickCounts.clear();
	rec.subtickMoves.clear();
	rec.rpEvents.clear();
	rec.jumps.clear();
	rec.cmdData.clear();
	rec.cmdSubtickCounts.clear();
	rec.cmdSubtickMoves.clear();
	rec.totalTicksRecorded = 0;
	rec.splicePending = true;
	rec.splicePartialId = partialId;
	rec.tickShiftFrom = (u32)g_pKZUtils->GetServerGlobals()->tickcount;

	SpliceContext ctx(player->GetClient()->GetUserID());
	ctx.steamId64 = player->GetSteamId64();
	ctx.recorderUuid = rec.uuid;
	ctx.key = key;
	ctx.partialId = partialId;
	ctx.restoredTime = restoredTime;
	KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_resume steam_id=%llu key=%s partial=%s local=%d\n", (unsigned long long)ctx.steamId64,
				Stem(key).c_str(), partialId.c_str(), (int)LocalFileExists(LocalPath(key)));
	if (LocalFileExists(LocalPath(key)))
	{
		ParseAsync(ctx, nullptr, true, false);
	}
	else
	{
		LoadRemote(ctx);
	}
}

void KZPartialReplay::Invalidate(const Key &key, const std::string &partialId, const char *reason)
{
	const std::string relPath = LocalPath(key);
	if (LocalFileExists(relPath))
	{
		utils::RemoveFile(relPath.c_str());
	}
	std::string apiMode;
	if (partialId.empty() || !ApiEligible(key, apiMode))
	{
		return;
	}
	KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_invalidate steam_id=%llu key=%s partial=%s reason=%s\n", (unsigned long long)key.steamId64,
				Stem(key).c_str(), partialId.c_str(), reason ? reason : "-");
	const std::string stem = Stem(key);
	KZOutboxService::PartialMeta meta = MakeMeta(key, apiMode, "delete", partialId);
	KZOutboxService::EnqueuePartial(stem, meta);
	KZOutboxService::SendPartial(stem, meta, nullptr);
}

void KZPartialReplay::PurgeExpiredLocal()
{
	namespace fs = std::filesystem;
	std::error_code ec;
	const std::string dir = AbsPath(KZ_PARTIAL_REPLAY_PATH);
	if (!fs::exists(dir, ec))
	{
		return;
	}
	const auto now = fs::file_time_type::clock::now();
	u32 removed = 0;
	for (const auto &entry : fs::directory_iterator(dir, ec))
	{
		std::error_code entryEc;
		if (!entry.is_regular_file(entryEc))
		{
			continue;
		}
		auto mtime = fs::last_write_time(entry.path(), entryEc);
		if (entryEc || now - mtime < std::chrono::seconds(kTtlSeconds))
		{
			continue;
		}
		if (fs::remove(entry.path(), entryEc))
		{
			removed++;
		}
	}
	if (removed > 0)
	{
		KZ_LOG_INFO(LogChannel::Replays, "[cyb] partial_replay_purged count=%u\n", removed);
	}
}
