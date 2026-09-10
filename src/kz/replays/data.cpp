#include "cs2kz.h"
#include "data.h"
#include "filesystem.h"
#include "utils/utils.h"
#include "utils/uuid.h"
#include "compression.h"
#include <thread>
#include <mutex>
#include <atomic>

CConVar<bool> kz_replay_playback_skins_enable("kz_replay_playback_skins_enable", FCVAR_NONE, "Enables applying player skins during replay playback.",
											  true);
CConVar<CUtlString> kz_replay_bot_default_crosshair("kz_replay_bot_default_crosshair", FCVAR_NONE,
													"Static crosshair code for the replay bot when the replay has none.", "");

static KZ::replaysystem::data::ReplayPlayback g_currentReplay = {};
static KZ::replaysystem::data::AsyncLoadStatus g_loadStatus = {};
static std::thread g_loadThread;
static std::atomic<bool> g_cancelLoad {false};

namespace KZ::replaysystem::data
{
	void FreeReplayData(ReplayPlayback *replay)
	{
		if (replay->tickData)
		{
			delete[] replay->tickData;
			replay->tickData = nullptr;
		}
		if (replay->subtickData)
		{
			delete[] replay->subtickData;
			replay->subtickData = nullptr;
		}
		if (replay->weapons)
		{
			delete[] replay->weapons;
			replay->weapons = nullptr;
		}
		if (replay->weaponIndices)
		{
			delete[] replay->weaponIndices;
			replay->weaponIndices = nullptr;
		}
		if (replay->jumps)
		{
			delete[] replay->jumps;
			replay->jumps = nullptr;
		}
		if (replay->events)
		{
			delete[] replay->events;
			replay->events = nullptr;
		}
		// AWR-разрез принадлежит реплею (ReplayPlayback копируется по значению, владелец —
		// текущий g_currentReplay), поэтому освобождается здесь же, вместе с кадрами.
		if (replay->awrDead)
		{
			delete replay->awrDead;
		}
		*replay = {};
	}

	void ResetReplayState(ReplayPlayback *replay)
	{
		// Reset all tracking indices
		replay->currentJump = 0;
		replay->currentEvent = 0;

		// Reset checkpoint/teleport counters
		replay->currentCpIndex = -1;
		replay->currentCheckpoint = 0;
		replay->currentTeleport = 0;

		// Reset timer state
		replay->courseName[0] = '\0';
		replay->startTime = 0.0f;
		replay->paused = false;
		replay->pausedTime = 0.0f;
		replay->accumulatedPauseTime = 0.0f;
		replay->pauseStartTime = 0.0f;
		replay->endTime = 0.0f;
		replay->stopTick = 0;
		replay->lastSplitTime = 0.0f;
		replay->lastCPZTime = 0.0f;
		replay->lastStageTime = 0.0f;

		// Reset replay pause state
		replay->replayPaused = false;

		// Позиция плейхеда внутри кадра — часть состояния воспроизведения, обнуляется
		// вместе с ним. Саму скорость (playbackSpeed) здесь НЕ трогаем: сик не должен
		// сбрасывать выбранную зрителем скорость; она задаётся на старте реплея.
		replay->tickFraction = 0.0f;
	}

	ReplayPlayback *GetCurrentReplay()
	{
		return &g_currentReplay;
	}

	bool IsReplayValid()
	{
		return g_currentReplay.valid;
	}

	bool IsReplayPlaying()
	{
		return g_currentReplay.playingReplay;
	}

	void SetCurrentTick(u32 tick)
	{
		g_currentReplay.currentTick = tick;
	}

	u32 GetCurrentTick()
	{
		return g_currentReplay.currentTick;
	}

	u32 GetTickCount()
	{
		return g_currentReplay.tickCount;
	}

	i32 GetCurrentCpIndex()
	{
		return g_currentReplay.currentCpIndex;
	}

	i32 GetCheckpointCount()
	{
		return g_currentReplay.currentCheckpoint;
	}

	i32 GetTeleportCount()
	{
		return g_currentReplay.currentTeleport;
	}

	bool IsAwrMode()
	{
		return g_currentReplay.awrMode;
	}

	f32 GetReplayTime()
	{
		if (g_currentReplay.startTime == 0.0f)
		{
			return 0.0f;
		}
		if (g_currentReplay.paused)
		{
			// На записанной паузе время заморожено (TIMER_PAUSE в events.cpp).
			return g_currentReplay.pausedTime;
		}
		// Активное время ВСЕГДА исключает всю накопленную паузу.
		return g_pKZUtils->GetServerGlobals()->curtime - g_currentReplay.startTime - g_currentReplay.accumulatedPauseTime;
	}

	f32 GetEndTime()
	{
		if (g_currentReplay.stopTick == 0)
		{
			return 0.0f;
		}
		if (g_currentReplay.stopTick + 192 < g_currentReplay.currentTick) // 3 seconds
		{
			return 0.0f;
		}
		return g_currentReplay.endTime;
	}

	bool GetPaused()
	{
		return g_currentReplay.paused;
	}

	static_function void UpdateProgress(const char *cursor, const char *dataStart, size_t totalSize, std::atomic<f32> &progress)
	{
		if (totalSize > 0)
		{
			size_t bytesRead = static_cast<size_t>(cursor - dataStart);
			progress = static_cast<f32>(bytesRead) / static_cast<f32>(totalSize);
			KZ_LOG_DEBUG(LogChannel::Replays, "Replay load progress: %zu bytes, %.2f%%\n", bytesRead, progress.load() * 100.0f);
		}
	}

	// Шапка файла: u32 длины + protobuf + проверка версии. Общий первый шаг обоих
	// разборов (полного плейбека и CutSource) — раскладка не имеет права разъехаться.
	// cursor сдвигается на начало секции тиков.
	static_function bool ParseHeaderPrefixed(const char *&cursor, const char *end, ReplayHeader &header)
	{
		if (cursor + (ptrdiff_t)sizeof(u32) > end)
		{
			return false;
		}
		u32 headerSize = 0;
		memcpy(&headerSize, cursor, sizeof(headerSize));
		cursor += sizeof(headerSize);

		if (headerSize == 0 || headerSize > 5 * 1024 * 1024) // sanity limit 5MB
		{
			return false;
		}
		if (cursor + (ptrdiff_t)headerSize > end)
		{
			return false;
		}

		std::string serialized(cursor, cursor + headerSize);
		cursor += headerSize;

		if (!header.ParseFromString(serialized))
		{
			return false;
		}
		return header.version() >= 1 && header.version() <= KZ_REPLAY_VERSION;
	}

	// Пропуск zstd-секции без распаковки: 12-байтная шапка CompressedSectionHeader
	// (compression.h) плюс сжатые байты. Нужен CutSource, чтобы перескочить оружие и
	// джампстаты и добраться до событий.
	//
	// Потолка на `uncompressedSize` здесь НЕТ намеренно: секция не распаковывается, ни одной
	// аллокации по этому числу не делается, и байтовая граница тут защищала бы не от чего.
	// Единственное, что важно, — `compressedSize` в пределах буфера: он двигает курсор, и
	// мусорное значение увело бы разбор событий за конец файла (проверка ниже).
	static_function bool SkipCompressedSection(const char *&cursor, const char *end)
	{
		using SectionHeader = KZ::replaysystem::compression::CompressedSectionHeader;
		if (cursor + (ptrdiff_t)sizeof(SectionHeader) > end)
		{
			return false;
		}
		SectionHeader header;
		memcpy(&header, cursor, sizeof(header));
		cursor += sizeof(header);
		if (cursor + (ptrdiff_t)header.compressedSize > end)
		{
			return false;
		}
		cursor += header.compressedSize;
		return true;
	}

	// Потолок РАСПАКОВАННЫХ БАЙТ секции по умолчанию — 256 МиБ. Применяется к тикам и
	// событиям; у сабтиков свой (см. вызов PeekSectionHeader ниже). Ориентир: самый длинный
	// мыслимый ран (2 часа) это ~460 800 кадров при sizeof(TickData) = 264 Б, то есть ~116 МиБ
	// сырых тиков (дельта-буфер плюс 8 Б флагов на кадр — ~120 МиБ), а лимит файла на
	// аплоаде — 32 МБ сжатых. 256 МиБ — двукратный запас к этому и одновременно тормоз на
	// суммарный запрос памяти: у тиков в пике живут ОБА буфера сразу
	// (`new char[uncompressedSize]` в compression.cpp:417 плюс `resize(elementCount)` на :489).
	static constexpr u64 KZ_CUT_MAX_SECTION_BYTES = 256ull * 1024ull * 1024ull;

	// Максимум на ЧИСЛО КАДРОВ (elementCount секций тиков и сабтиков) — 1 000 000, это ~4.3
	// часа при 64 тик/с, двукратный запас к самому длинному мыслимому рану.
	//
	// Почему отдельной границей, а не байтами: массив сабтиков стоит 868 Б на кадр
	// (`MAX_SUBTICK_MOVES = 36`, замерено), и у ЛЕГИТИМНОГО двухчасового рана это 381 МБ —
	// байтовый потолок 256 МиБ отверг бы такой файл начиная примерно с 84 минут. Отказ здесь
	// необратим: воркер бэкфилла отправляет на него `awrMs: null`, и файл больше никогда не
	// вернётся в бэклог. Поэтому длину рана ограничиваем длиной рана, а не байтами; смысл
	// проверки — отсечь абсурд из мусорной шапки (там elementCount доходит до 4 млрд, то есть
	// до терабайтов запроса), а не выгадать десятки мегабайт на легальных файлах.
	// Для тиков 1 млн кадров это ~252 МиБ сырых данных, то есть та же величина, что и
	// байтовый потолок выше, — границы согласованы и ни одна не «срабатывает первой» на
	// легальном файле.
	static constexpr u64 KZ_CUT_MAX_TICKS = 1000000ull;

	// Подглядеть шапку секции, НЕ сдвигая курсор, и проверить общие границы: шапка целиком
	// лежит в буфере, сжатые байты не выходят за его конец, распакованный размер не больше
	// `maxUncompressedBytes`. false — файл битый.
	//
	// Потолок распакованных байт задаёт ВЫЗЫВАЮЩИЙ, а не функция: у секций разная цена
	// элемента, и общий потолок обязательно оказался бы ложным отказом для одной из них
	// (сабтики — 868 Б на кадр, у легитимного двухчасового рана это 381 МБ). Отказ здесь
	// необратим — воркер бэкфилла отправляет на него `awrMs: null`.
	static_function bool PeekSectionHeader(const char *cursor, const char *end, u64 maxUncompressedBytes,
										   KZ::replaysystem::compression::CompressedSectionHeader &out)
	{
		using SectionHeader = KZ::replaysystem::compression::CompressedSectionHeader;
		if (!cursor || cursor > end || (size_t)(end - cursor) < sizeof(SectionHeader))
		{
			return false;
		}
		memcpy(&out, cursor, sizeof(out));
		const size_t avail = (size_t)(end - cursor) - sizeof(SectionHeader);
		if ((u64)out.compressedSize > (u64)avail)
		{
			return false;
		}
		if ((u64)out.uncompressedSize > maxUncompressedBytes)
		{
			return false;
		}
		return true;
	}

	CutSource LoadCutSourceFromMemory(const char *data, size_t size)
	{
		CutSource out;
		if (!data || size == 0)
		{
			return out;
		}

		const char *cursor = data;
		const char *end = data + size;

		if (!ParseHeaderPrefixed(cursor, end, out.header))
		{
			return out;
		}

		// ПРЕ-ВАЛИДАЦИЯ секций до всякой распаковки. Это единственная защита от битого файла
		// в данном тракте: форк собирается с `-fno-exceptions` (AMBuildScript), то есть
		// bad_alloc/length_error поймать нечем в принципе, а распаковщик аллоцирует ровно по
		// числам ИЗ ФАЙЛА. Поэтому абсурдные размеры отсекаются здесь, а не в аллокаторе.
		// Все проверки — на подглядывании шапки без сдвига курсора, сам разбор ниже не
		// меняется.
		using SectionHeader = KZ::replaysystem::compression::CompressedSectionHeader;
		SectionHeader tickHeader {}, subtickHeader {}, eventHeader {};
		const char *probe = cursor;

		// Секция тиков. compression.cpp:417 делает `new char[uncompressedSize]` (границу
		// держит PeekSectionHeader), compression.cpp:489 — `resize(elementCount)` уже
		// массивом TickData, поэтому ограничиваем И число кадров (KZ_CUT_MAX_TICKS).
		if (!PeekSectionHeader(probe, end, KZ_CUT_MAX_SECTION_BYTES, tickHeader) || (u64)tickHeader.elementCount > KZ_CUT_MAX_TICKS)
		{
			return out;
		}
		probe += sizeof(SectionHeader) + tickHeader.compressedSize;

		// Секция сабтиков идёт сразу за тиками (её читает та же ReadTickDataCompressed).
		// compression.cpp:458 делает `resize(elementCount)`, а следом Decompress пишет в этот
		// же буфер до `uncompressedSize` байт. Проверяем ВЕРХНЮЮ ГРАНИЦУ, а не равенство:
		// `uncompressedSize` МЕНЬШЕ ёмкости вектора — законная ситуация (секции не
		// версионируются отдельно от файла, и zstd просто запишет меньше байт, чем ёмкость,
		// записи за границу тут нет), а вот БОЛЬШЕ — уже перезапись за концом вектора.
		// Равенство отвергало бы легитимные файлы, а отказ необратим: воркер бэкфилла на него
		// отправляет `awrMs: null`, и файл больше никогда не вернётся в бэклог.
		// Для v<4 раскладка другая (`oldEntrySize = uncompressedSize / elementCount`): там
		// нужна делимость и НЕнулевой elementCount — на нуле апстримный читатель делит на
		// ноль. Отказ от древнего файла без сабтиков дешевле, чем деление на ноль.
		// Байтовый потолок сабтиков — РОВНО та верхняя граница, которую уже даёт проверка
		// числа кадров (`KZ_CUT_MAX_TICKS * sizeof(SubtickData)` ≈ 828 МиБ). Меньше делать
		// нельзя: 256 МиБ отвергали бы легитимный ран длиннее ~84 минут (868 Б на кадр).
		// Больше — бессмысленно: такой файл всё равно не пройдёт проверку elementCount.
		if (!PeekSectionHeader(probe, end, KZ_CUT_MAX_TICKS * (u64)sizeof(SubtickData), subtickHeader)
			|| (u64)subtickHeader.elementCount > KZ_CUT_MAX_TICKS)
		{
			return out;
		}
		if (out.header.version() >= 4)
		{
			if ((u64)subtickHeader.uncompressedSize > (u64)subtickHeader.elementCount * sizeof(SubtickData))
			{
				return out;
			}
		}
		else if (subtickHeader.elementCount == 0 || subtickHeader.uncompressedSize % subtickHeader.elementCount != 0)
		{
			return out;
		}

		// Сабтики читаются той же функцией, что и тики, и здесь не нужны — вектор живёт
		// до конца разбора и уходит вместе с ним.
		std::vector<SubtickData> subticks;
		if (!KZ::replaysystem::compression::ReadTickDataCompressed(cursor, end, out.ticks, subticks, out.header.version()))
		{
			return out;
		}

		// Оружие и джампстаты — мимо: разрезу и !lead они не нужны, а распаковка стоила бы
		// памяти на рабочем потоке. Отсюда и единственная проверка у них — что `compressedSize`
		// не уводит курсор за конец буфера (см. SkipCompressedSection).
		if (!SkipCompressedSection(cursor, end) || !SkipCompressedSection(cursor, end))
		{
			return out;
		}

		// Секция событий: compression.cpp:716 делает `resize(elementCount)` массивом RpEvent,
		// а Decompress следом пишет туда до `uncompressedSize` байт. Как и у сабтиков —
		// ВЕРХНЯЯ ГРАНИЦА, не равенство: меньший `uncompressedSize` законен, больший означает
		// запись за концом вектора.
		//
		// Число элементов здесь ограничиваем БАЙТОВЫМ потолком (а не KZ_CUT_MAX_TICKS, как у
		// кадров): событий в ране единицы тысяч (сплиты, ТП, смены стиля), и 256 МиБ — это
		// миллионы записей, то есть на легальный файл граница не влияет вообще. Без неё
		// мусорный elementCount (до 4 млрд) ушёл бы прямо в resize.
		if (!PeekSectionHeader(cursor, end, KZ_CUT_MAX_SECTION_BYTES, eventHeader)
			|| (u64)eventHeader.elementCount * sizeof(RpEvent) > KZ_CUT_MAX_SECTION_BYTES
			|| (u64)eventHeader.uncompressedSize > (u64)eventHeader.elementCount * sizeof(RpEvent))
		{
			return out;
		}

		if (!KZ::replaysystem::compression::ReadEventsCompressed(cursor, end, out.events))
		{
			return out;
		}

		out.valid = true;
		return out;
	}

	// Parses replay data from an in-memory byte array.
	static_function ReplayPlayback LoadReplayFromMemory(const char *data, size_t size, UUID_t uuid, std::atomic<f32> &progress,
														std::atomic<bool> &shouldCancel)
	{
		ReplayPlayback result = {};
		progress = 0.0f;

		const char *cursor = data;
		const char *end = data + size;

		// Read length-prefixed protobuf header
		if (shouldCancel)
		{
			return result;
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "Loading replay protobuf header...\n");

		// Шапка и проверка версии — общий помощник (см. ParseHeaderPrefixed).
		if (!ParseHeaderPrefixed(cursor, end, result.header))
		{
			return result;
		}

		UpdateProgress(cursor, data, size, progress);

		// Load tick data
		if (shouldCancel)
		{
			return result;
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "Loading compressed tick data...\n");

		std::vector<TickData> tickDataVec;
		std::vector<SubtickData> subtickDataVec;

		if (!KZ::replaysystem::compression::ReadTickDataCompressed(cursor, end, tickDataVec, subtickDataVec, result.header.version()))
		{
			return result;
		}

		result.tickCount = tickDataVec.size();
		tickDataVec.shrink_to_fit();
		subtickDataVec.shrink_to_fit();
		result.tickData = tickDataVec.data();
		result.subtickData = subtickDataVec.data();
		new (&tickDataVec) std::vector<TickData>();
		new (&subtickDataVec) std::vector<SubtickData>();

		UpdateProgress(cursor, data, size, progress);

		// Load weapon data
		if (shouldCancel)
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			return {};
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "Loading weapons...\n");

		std::vector<std::pair<i32, EconInfo>> weaponTableVec;

		if (!KZ::replaysystem::compression::ReadWeaponsCompressed(cursor, end, weaponTableVec))
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			return {};
		}

		result.weaponTableSize = weaponTableVec.size();
		assert(result.weaponTableSize > 0);
		result.weaponIndices = new i32[result.weaponTableSize];
		result.weapons = new EconInfo[result.weaponTableSize];
		for (size_t i = 0; i < weaponTableVec.size(); i++)
		{
			result.weaponIndices[i] = weaponTableVec[i].first;
			result.weapons[i] = weaponTableVec[i].second;
		}

		UpdateProgress(cursor, data, size, progress);

		// Load jump stats
		if (shouldCancel)
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			delete[] result.weaponIndices;
			delete[] result.weapons;
			return {};
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "Loading compressed jump stats...\n");

		std::vector<RpJumpStats> jumpsVec;

		if (!KZ::replaysystem::compression::ReadJumpsCompressed(cursor, end, jumpsVec, result.header.version()))
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			delete[] result.weaponIndices;
			delete[] result.weapons;
			return {};
		}

		result.numJumps = jumpsVec.size();
		if (result.numJumps > 0)
		{
			result.jumps = new RpJumpStats[result.numJumps];
			for (size_t i = 0; i < result.numJumps; i++)
			{
				result.jumps[i] = std::move(jumpsVec[i]);
			}
		}
		else
		{
			result.jumps = nullptr;
		}

		UpdateProgress(cursor, data, size, progress);

		// Load events
		if (shouldCancel)
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			delete[] result.weaponIndices;
			delete[] result.weapons;
			delete[] result.jumps;
			return {};
		}
		KZ_LOG_DEBUG(LogChannel::Replays, "Loading compressed events...\n");

		std::vector<RpEvent> eventsVec;

		if (!KZ::replaysystem::compression::ReadEventsCompressed(cursor, end, eventsVec))
		{
			delete[] result.tickData;
			delete[] result.subtickData;
			delete[] result.weaponIndices;
			delete[] result.weapons;
			delete[] result.jumps;
			return {};
		}

		result.numEvents = eventsVec.size();
		eventsVec.shrink_to_fit();
		result.events = eventsVec.data();
		new (&eventsVec) std::vector<RpEvent>();

		UpdateProgress(cursor, data, size, progress);

		result.uuid = uuid;
		result.valid = true;
		progress = 1.0f;
		return result;
	}

	// File-based entry point: reads the entire file into memory then parses.
	static_function ReplayPlayback LoadReplayWithProgress(const char *path, std::atomic<f32> &progress, std::atomic<bool> &shouldCancel)
	{
		ReplayPlayback result = {};

		FileHandle_t file = g_pFullFileSystem->Open(path, "rb");
		if (!file)
		{
			return result;
		}

		size_t fileSize = g_pFullFileSystem->Size(file);
		std::vector<char> fileData(fileSize);
		if (g_pFullFileSystem->Read(fileData.data(), (int)fileSize, file) != (int)fileSize)
		{
			g_pFullFileSystem->Close(file);
			return result;
		}
		g_pFullFileSystem->Close(file);

		UUID_t uuid = {};
		if (!UUID_t::FromString(CUtlString(path).GetBaseFilename().StripExtension().Get(), &uuid))
		{
			return result;
		}

		return LoadReplayFromMemory(fileData.data(), fileSize, uuid, progress, shouldCancel);
	}

	// Memory-based entry point: parse directly from a provided buffer.
	static_function ReplayPlayback LoadReplayWithProgress(const char *data, size_t size, UUID_t uuid, std::atomic<f32> &progress,
														  std::atomic<bool> &shouldCancel)
	{
		return LoadReplayFromMemory(data, size, uuid, progress, shouldCancel);
	}

	static_function void PrepareAsyncLoad(LoadSuccessCallback onSuccess, LoadFailureCallback onFailure)
	{
		CancelAsyncLoad();
		g_loadStatus.state = LoadingState::Loading;
		g_loadStatus.progress = 0.0f;
		g_cancelLoad = false;
		{
			std::lock_guard<std::mutex> lock(g_loadStatus.callbackMutex);
			g_loadStatus.successCallback = onSuccess;
			g_loadStatus.failureCallback = onFailure;
		}
		{
			std::lock_guard<std::mutex> lock(g_loadStatus.errorMutex);
			g_loadStatus.errorMessage.clear();
		}
		{
			std::lock_guard<std::mutex> lock(g_loadStatus.replayMutex);
			FreeReplayData(&g_loadStatus.completedReplay);
			g_loadStatus.completedReplay = {};
		}
	}

	static_function void HandleAsyncResult(ReplayPlayback &result)
	{
		if (g_cancelLoad)
		{
			// Load was cancelled, cleanup and exit
			FreeReplayData(&result);
			g_loadStatus.state = LoadingState::Failed;
			{
				std::lock_guard<std::mutex> lock(g_loadStatus.errorMutex);
				g_loadStatus.errorMessage = "Replay - Load Cancelled";
			}
			return;
		}

		if (!result.valid)
		{
			g_loadStatus.state = LoadingState::Failed;
			{
				std::lock_guard<std::mutex> lock(g_loadStatus.errorMutex);
				g_loadStatus.errorMessage = "Replay - Invalid File Format";
			}
			return;
		}

		// Success - store result for main thread to process
		{
			std::lock_guard<std::mutex> lock(g_loadStatus.replayMutex);
			g_loadStatus.completedReplay = result;
		}
		g_loadStatus.state = LoadingState::Completed;
	}

	void LoadReplayAsync(std::string path, LoadSuccessCallback onSuccess, LoadFailureCallback onFailure)
	{
		PrepareAsyncLoad(onSuccess, onFailure);
		g_loadThread = std::thread(
			[path = std::move(path)]()
			{
				ReplayPlayback result = LoadReplayWithProgress(path.c_str(), g_loadStatus.progress, g_cancelLoad);
				HandleAsyncResult(result);
			});
		g_loadThread.detach();
	}

	void LoadReplayMemoryAsync(std::vector<char> data, UUID_t uuid, LoadSuccessCallback onSuccess, LoadFailureCallback onFailure)
	{
		PrepareAsyncLoad(onSuccess, onFailure);
		g_loadThread = std::thread(
			[data = std::move(data), uuid]() mutable
			{
				ReplayPlayback result = LoadReplayWithProgress(data.data(), data.size(), uuid, g_loadStatus.progress, g_cancelLoad);
				HandleAsyncResult(result);
			});
		g_loadThread.detach();
	}

	AsyncLoadStatus *GetLoadStatus()
	{
		return &g_loadStatus;
	}

	bool IsLoading()
	{
		return g_loadStatus.state == LoadingState::Loading;
	}

	void CancelAsyncLoad()
	{
		if (IsLoading())
		{
			g_cancelLoad = true;
		}
	}

	void ProcessAsyncLoadCompletion()
	{
		LoadingState currentState = g_loadStatus.state.load();

		if (currentState == LoadingState::Completed)
		{
			// Handle successful load
			ReplayPlayback completedReplay;
			LoadSuccessCallback successCallback;

			// Get the completed replay and callback
			{
				std::lock_guard<std::mutex> replayLock(g_loadStatus.replayMutex);
				std::lock_guard<std::mutex> callbackLock(g_loadStatus.callbackMutex);

				completedReplay = g_loadStatus.completedReplay;
				successCallback = g_loadStatus.successCallback;

				// Clear the completed replay from the status struct
				g_loadStatus.completedReplay = {};
			}

			// Process the completion on the main thread
			if (completedReplay.valid)
			{
				// Store the replay
				FreeReplayData(&g_currentReplay);
				g_currentReplay = completedReplay;

				// Call the success callback if it exists
				if (successCallback)
				{
					successCallback();
				}
			}

			// Reset state to idle
			g_loadStatus.state = LoadingState::Idle;
		}
		else if (currentState == LoadingState::Failed)
		{
			// Handle failed load
			std::string errorMessage;
			LoadFailureCallback failureCallback;

			// Get the error message and callback
			{
				std::lock_guard<std::mutex> errorLock(g_loadStatus.errorMutex);
				std::lock_guard<std::mutex> callbackLock(g_loadStatus.callbackMutex);

				errorMessage = g_loadStatus.errorMessage;
				failureCallback = g_loadStatus.failureCallback;
			}

			// Call the failure callback if it exists
			if (failureCallback)
			{
				failureCallback(errorMessage.c_str());
			}

			// Reset state to idle
			g_loadStatus.state = LoadingState::Idle;
		}
	}

} // namespace KZ::replaysystem::data
