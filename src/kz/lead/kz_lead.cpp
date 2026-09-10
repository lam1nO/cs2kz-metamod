#include "kz_lead.h"

#include "cs2kz.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/option/kz_option.h"
#include "kz/replays/awr_cut.h"
#include "kz/replays/data.h"
#include "kz/replays/playback.h"

#include "sdk/entity/cparticlesystem.h"
#include "entitykeyvalues.h"

#include "utils/logging.h"
#include "utils/simplecmds.h"
#include "utils/utils.h"

#include <algorithm>
#include <cfloat>
#include <string>
#include <thread>
#include <utility>

// Окно вокруг игрока: 1 с назад / 6 с вперёд по ЭФФЕКТИВНЫМ серверным тикам (64/с).
#define KZ_LEAD_BACK_TICKS   64
#define KZ_LEAD_AHEAD_TICKS  384
// Пересчёт окна — раз в 32 тика (два раза в секунду): создание сущностей дороже, чем
// точность отставания луча на полшага.
#define KZ_LEAD_UPDATE_TICKS 32
// Дальше этого от ближайшей вершины считаем, что игрок сошёл с маршрута, и ищем заново
// по всему пути (телепорт мимо хука, спавн, падение в другую часть карты).
#define KZ_LEAD_RESEARCH_DIST 300.0f
// Допуск упрощения пути (юниты). Меньше — лишние сущности на прямых, больше — срезанные углы.
#define KZ_LEAD_RDP_TOLERANCE 2.0f
// Верхняя граница потолка отрезков: защита от опечатки в конфиге (лимит сущностей движка).
#define KZ_LEAD_MAX_SEGMENTS_CAP 512
// Какая доля потолка отрезков может уйти на хвост ПОЗАДИ ближайшей вершины (1/4).
// Остальное резервируется под подсказку впереди.
#define KZ_LEAD_BACK_BUDGET_DIV 4

#define KZ_LEAD_PARTICLE "particles/ui/annotation/ui_annotation_line_segment.vpcf"
// Свой targetname обязателен: по нему снятие отличает НАШИ отрезки от чужих энтити с
// переиспользованным индексом (хендл может разрешиться в постороннюю сущность). Через
// classname их не различить — NameMatches сверяет m_name, а не класс. Идиома от
// KZ::zones::RemoveBoxEdges.
#define KZ_LEAD_TARGETNAME "cyb_lead_seg"

using namespace KZ::replaysystem;

namespace
{
	// Копия CreateMeasureBeam (kz_measure.cpp): тот же примитив, свой цвет на отрезок.
	CEntityHandle CreateLeadSegment(const Vector &start, const Vector &end, const Color &color)
	{
		CParticleSystem *line = utils::CreateEntityByName<CParticleSystem>("info_particle_system");
		if (!line)
		{
			return CEntityHandle();
		}
		CEntityKeyValues *pKeyValues = new CEntityKeyValues();
		pKeyValues->SetString("effect_name", KZ_LEAD_PARTICLE);
		pKeyValues->SetString("targetname", KZ_LEAD_TARGETNAME);
		pKeyValues->SetVector("origin", start);
		pKeyValues->SetInt("tint_cp", 16);
		pKeyValues->SetColor("tint_cp_color", color);
		pKeyValues->SetInt("data_cp", 1);
		pKeyValues->SetVector("data_cp_value", end);
		pKeyValues->SetBool("start_active", true);
		line->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
		line->DispatchSpawn(pKeyValues);
		return line->GetRefEHandle();
	}

	// Снятие одного отрезка. Хендл сам по себе ничего не гарантирует: индекс энтити
	// переиспользуется, и без сверки targetname мы могли бы снести чужую сущность.
	void RemoveLeadSegment(const CEntityHandle &handle)
	{
		CEntityInstance *inst = GameEntitySystem() ? GameEntitySystem()->GetEntityInstance(handle) : nullptr;
		if (inst && inst->m_pEntity && inst->m_pEntity->NameMatches(KZ_LEAD_TARGETNAME))
		{
			g_pKZUtils->RemoveEntity(inst);
		}
	}

	// Цвет отрезка берётся у НАЧАЛЬНОЙ вершины: отрезок показывает, чем игрок был занят,
	// когда из неё уходил.
	Color SegmentColor(bool onGround)
	{
		return onGround ? Color(80, 220, 120, 255) : Color(90, 170, 255, 255);
	}

	f32 PointSegmentDistSqr(const Vector &p, const Vector &a, const Vector &b)
	{
		Vector ab = b - a;
		const f32 len2 = ab.LengthSqr();
		if (len2 < 1e-6f)
		{
			return (p - a).LengthSqr();
		}
		f32 t = (p - a).Dot(ab) / len2;
		t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
		const Vector proj = a + ab * t;
		return (p - proj).LengthSqr();
	}

	// Рамер–Дуглас–Пекер на срезе [first, last] списка вершин. Явный стек, не рекурсия:
	// живой интервал — это десятки тысяч кадров, и на почти-прямом пути глубина рекурсии
	// была бы линейной по их числу (переполнение стека рабочего потока).
	void SimplifyRange(const std::vector<KZLeadService::Vertex> &src, u32 first, u32 last, f32 tolSqr, std::vector<bool> &keep)
	{
		if (last <= first + 1)
		{
			return;
		}
		std::vector<std::pair<u32, u32>> stack;
		stack.push_back({first, last});
		while (!stack.empty())
		{
			const std::pair<u32, u32> range = stack.back();
			stack.pop_back();
			if (range.second <= range.first + 1)
			{
				continue;
			}
			u32 worst = range.first;
			f32 worstDist = -1.0f;
			for (u32 i = range.first + 1; i < range.second; i++)
			{
				const f32 d = PointSegmentDistSqr(src[i].pos, src[range.first].pos, src[range.second].pos);
				if (d > worstDist)
				{
					worstDist = d;
					worst = i;
				}
			}
			if (worstDist <= tolSqr)
			{
				continue;
			}
			keep[worst] = true;
			stack.push_back({range.first, worst});
			stack.push_back({worst, range.second});
		}
	}

	// Рабочий поток: файл с диска → живые кадры рана → упрощённый путь.
	//
	// Файл читается ЗДЕСЬ, а не на главном потоке: реплей — до 32 МБ, и чтение в тике
	// колбэка докачки было бы хитчем. Тем же путём идёт штатный загрузчик реплеев
	// (data::LoadReplayAsync читает файл на своём потоке); utils::ReadBufferFromFile —
	// обычный stdio, движковый файловый интерфейс не трогает.
	//
	// Всё тело в try/catch: разбор аллоцирует по размерам ИЗ ФАЙЛА (compression.cpp:417
	// `new char[uncompressedSize]`, :458 `resize(elementCount)`), и на битом файле прилетит
	// bad_alloc/length_error. Необработанное исключение в detached-потоке — std::terminate,
	// то есть падение сервера из-за одного мусорного реплея (та же защита, что у воркера
	// бэкфилла, cyb_awr_backfill.cpp).
	void BuildPathWorker(std::string filePath, std::shared_ptr<KZLeadService::PendingLoad> pending)
	{
		std::vector<KZLeadService::Vertex> out;
		const char *failReason = nullptr;
		const char *cutWarn = nullptr;

		try
		{
			std::vector<char> bytes;
			if (!utils::ReadBufferFromFile(filePath.c_str(), bytes) || bytes.empty())
			{
				failReason = "read_failed";
			}
			else
			{
				data::CutSource src = data::LoadCutSourceFromMemory(bytes.data(), bytes.size());
				if (!src.valid || src.ticks.empty())
				{
					failReason = "parse_failed";
				}
				else
				{
					const u32 count = (u32)src.ticks.size();
					const RpEvent *events = src.events.empty() ? nullptr : src.events.data();
					const u32 numEvents = (u32)src.events.size();

					// Окно САМОГО рана: в файле есть ~5 с предзаписи и хвост после финиша, и
					// без этой границы луч уводил бы новичка по дороге к старту и за финиш.
					// Окна нет (оборванный ран, разные курсы пары) — берём файл целиком, как
					// прежде: подсказка с лишними хвостами полезнее отсутствия подсказки.
					u32 runStart = 0, runEnd = count - 1;
					u32 wStart = 0, wEnd = 0;
					i32 runCourseId = -1;
					// Возврат читаем именно как bool: RunWindowFromEvents умеет вернуть false
					// УЖЕ записав вырожденное окно (outStart >= outEnd), и брать его нельзя.
					if (playback::RunWindowFromEvents(src.ticks.data(), count, events, numEvents, wStart, wEnd, runCourseId)
						&& wStart < wEnd && wEnd < count)
					{
						runStart = wStart;
						runEnd = wEnd;
					}

					const u64 timeMs =
						(src.header.has_run() && src.header.run().time() > 0.0f) ? (u64)(src.header.run().time() * 1000.0 + 0.5) : 0;
					awr::CutResult cut = playback::ComputeCutFor(src.ticks.data(), count, events, numEvents, timeMs);
					std::vector<awr::Interval> live;
					if (cut.ok)
					{
						live = awr::LiveIntervals(cut.dead, count);
					}
					else
					{
						// Разрез не сошёлся — рисуем путь целиком: луч без вырезки всё равно
						// показывает маршрут, а отказ оставил бы новичка вообще без подсказки.
						cutWarn = cut.reason;
						live.push_back({0, count - 1});
					}

					// Эффективный тик: мёртвые интервалы времени не занимают, поэтому окно
					// «1 с назад / 6 с вперёд» непрерывно переходит через стык разреза.
					u32 effBase = 0;
					std::vector<KZLeadService::Vertex> raw;
					for (const awr::Interval &full : live)
					{
						// Пересечение живого интервала с окном рана. LiveIntervals — дополнение
						// мёртвых по ВСЕМУ файлу, поэтому первый и последний интервалы всегда
						// вылезают за ран.
						if (full.to < runStart || full.from > runEnd)
						{
							continue;
						}
						// Без std::min/max: у awr::Interval поля uint32_t, у окна — u32 форка,
						// и вывод шаблона на разных платформах мог бы не сойтись.
						const u32 ivFrom = full.from > runStart ? full.from : runStart;
						const u32 ivTo = full.to < runEnd ? full.to : runEnd;
						awr::Interval iv {ivFrom, ivTo};
						if (iv.from >= count || iv.to >= count || iv.from > iv.to)
						{
							continue;
						}
						const u32 firstVertex = (u32)raw.size();
						const u32 baseTick = src.ticks[iv.from].serverTick;
						for (u32 i = iv.from; i <= iv.to; i++)
						{
							KZLeadService::Vertex v;
							v.pos = src.ticks[i].post.origin;
							v.onGround = (src.ticks[i].post.entityFlags & FL_ONGROUND) != 0;
							v.tickIdx = effBase + (src.ticks[i].serverTick - baseTick);
							raw.push_back(v);
						}
						effBase += src.ticks[iv.to].serverTick - baseTick + 1;

						// Упрощение — ПО КАЖДОМУ живому интервалу отдельно: сшивать соседние через
						// вырезанную петлю нельзя, там нет пути.
						const u32 lastVertex = (u32)raw.size() - 1;
						std::vector<bool> keep(raw.size(), false);
						keep[firstVertex] = true;
						keep[lastVertex] = true;
						// Вершины смены onGround обязательны: на них меняется цвет отрезка, и
						// упрощение не имеет права стирать отрыв и приземление.
						for (u32 i = firstVertex + 1; i <= lastVertex; i++)
						{
							if (raw[i].onGround != raw[i - 1].onGround)
							{
								keep[i] = true;
								keep[i - 1] = true;
							}
						}
						// Между обязательными вершинами — свой прогон РДП (так они и «прибиты»).
						u32 anchor = firstVertex;
						for (u32 i = firstVertex + 1; i <= lastVertex; i++)
						{
							if (!keep[i])
							{
								continue;
							}
							SimplifyRange(raw, anchor, i, KZ_LEAD_RDP_TOLERANCE * KZ_LEAD_RDP_TOLERANCE, keep);
							anchor = i;
						}
						for (u32 i = firstVertex; i <= lastVertex; i++)
						{
							if (keep[i])
							{
								out.push_back(raw[i]);
							}
						}
					}

					if (out.size() < 2)
					{
						failReason = "path_too_short";
						out.clear();
					}
				}
			}
		}
		catch (...)
		{
			// Битый файл: отдаём отказ, игрок увидит «нет реплея», сервер жив.
			out.clear();
			failReason = "parse_failed";
			cutWarn = nullptr;
		}

		{
			std::lock_guard<std::mutex> lock(pending->mu);
			pending->path = std::move(out);
			pending->failReason = failReason;
			pending->cutWarn = cutWarn;
		}
		pending->done = true;
	}
} // namespace

void KZLeadService::Reset()
{
	// Дисконнект и late load: мир жив, сущности обязаны уйти.
	this->ResetState(false);
}

void KZLeadService::OnMapChanged()
{
	// Граница строго `i < MAXPLAYERS`: ToPlayer(CPlayerSlot) внутри берёт slot.Get() + 1 по
	// массиву players[MAXPLAYERS + 1]. Подробный разбор границы и второго оверлоада —
	// в KZ::zones::ResetEditors, откуда взят этот обход.
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->leadService)
		{
			// keepEntities: мир прошлой карты уже разобран, RemoveEntity по его хендлам
			// пошёл бы в никуда (или в чужую энтити нового мира).
			player->leadService->ResetState(true);
		}
	}
}

void KZLeadService::ResetState(bool keepEntities)
{
	this->ClearSegments(keepEntities);
	this->path.clear();
	this->path.shrink_to_fit();
	this->enabled = false;
	this->loading = false;
	// Незавершённая загрузка протухает: её результат отбросит проверка поколения.
	this->generation++;
	this->pending.reset();
	this->windowFrom = 0;
	this->windowTo = 0;
	this->nearest = 0;
	this->resync = true;
	this->ticksSinceUpdate = 0;
	this->modeName[0] = '\0';
}

void KZLeadService::ClearSegments(bool keepEntities)
{
	if (!keepEntities)
	{
		for (const CEntityHandle &handle : this->segments)
		{
			RemoveLeadSegment(handle);
		}
	}
	this->segments.clear();
	this->ownedSorted.clear();
}

void KZLeadService::RebuildOwnedIndex()
{
	this->ownedSorted = this->segments;
	std::sort(this->ownedSorted.begin(), this->ownedSorted.end(), [](const CEntityHandle &a, const CEntityHandle &b) { return a < b; });
}

bool KZLeadService::OwnsParticle(const CEntityHandle &handle) const
{
	// Горячий путь CheckTransmit: на каждую помеченную частицу, на каждого получателя.
	// Отсюда двоичный поиск (до 64 отрезков × десятки частиц × число игроков линейным
	// сканом — десятки тысяч сравнений за тик).
	return std::binary_search(this->ownedSorted.begin(), this->ownedSorted.end(), handle,
							  [](const CEntityHandle &a, const CEntityHandle &b) { return a < b; });
}

void KZLeadService::Disable(const char *reason)
{
	if (!this->enabled && !this->loading && !this->pending)
	{
		// `!lead off` при выключенном луче: «выключен» было бы неправдой.
		this->player->languageService->PrintChat(true, false, "Lead - Not Enabled");
		return;
	}
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] disable reason=%s steam_id=%llu\n", reason, this->player->GetSteamId64());
	this->Reset();
	// Все пути Disable — от игрока (!lead off, повторный !lead, смена режима), поэтому
	// подтверждение в чат безусловно.
	this->player->languageService->PrintChat(true, false, "Lead - Disabled");
}

void KZLeadService::Toggle(CybReplayDownload::Kind kind)
{
	if (this->enabled)
	{
		this->Disable("toggle");
		return;
	}
	if (this->loading || this->pending)
	{
		// Загрузка уже идёт (сеть или разбор) — второй запрос ничего не ускорит, но завёл бы
		// ещё один резолв, ещё одну докачку и ещё один поток.
		this->player->languageService->PrintChat(true, false, "Lead - Loading");
		return;
	}

	this->loading = true;
	const u32 gen = ++this->generation;
	this->player->languageService->PrintChat(true, false, "Lead - Loading");
	CybReplayDownload::RequestFile(this->player, kind, this->player->GetSteamId64(),
								   [gen](CPlayerUserId userID, std::string filePath)
								   {
									   KZPlayer *player = g_pKZPlayerManager->ToPlayer(userID);
									   if (!player || !player->leadService)
									   {
										   return;
									   }
									   player->leadService->OnFileReady(gen, std::move(filePath));
								   });
}

void KZLeadService::OnFileReady(u32 gen, std::string &&filePath)
{
	if (gen != this->generation)
	{
		// Луч успели выключить и/или запросить заново: флаг loading принадлежит уже НЕ
		// этому запросу, снимать его нельзя.
		return;
	}
	if (filePath.empty())
	{
		this->loading = false;
		this->player->languageService->PrintChat(true, false, "Lead - No Replay");
		return;
	}
	this->pending = std::make_shared<PendingLoad>();
	this->pending->generation = gen;
	// Дальше гейтом служит сам pending — сетевая фаза кончилась.
	this->loading = false;
	std::thread(BuildPathWorker, std::move(filePath), this->pending).detach();
}

void KZLeadService::PollPending()
{
	if (!this->pending->done)
	{
		return;
	}
	std::shared_ptr<PendingLoad> load = this->pending;
	this->pending.reset();
	if (load->generation != this->generation)
	{
		return;
	}

	std::vector<Vertex> loaded;
	const char *failReason = nullptr;
	const char *cutWarn = nullptr;
	{
		std::lock_guard<std::mutex> lock(load->mu);
		loaded = std::move(load->path);
		failReason = load->failReason;
		cutWarn = load->cutWarn;
	}

	if (failReason)
	{
		KZ_LOG_WARN(LogChannel::Replays, "[lead] lead_load_failed reason=%s steam_id=%llu\n", failReason, this->player->GetSteamId64());
		this->player->languageService->PrintChat(true, false, "Lead - No Replay");
		return;
	}
	if (cutWarn && cutWarn[0])
	{
		KZ_LOG_WARN(LogChannel::Replays, "[lead] lead_load_failed reason=cut_failed detail=%s steam_id=%llu\n", cutWarn,
					this->player->GetSteamId64());
	}
	this->OnPathLoaded(std::move(loaded));
}

void KZLeadService::OnPathLoaded(std::vector<Vertex> &&newPath)
{
	this->ClearSegments(false);
	this->path = std::move(newPath);
	this->enabled = true;
	this->resync = true;
	this->nearest = 0;
	this->windowFrom = 0;
	this->windowTo = 0;
	// Окно строится на ближайшем же тике, а не через полсекунды.
	this->ticksSinceUpdate = KZ_LEAD_UPDATE_TICKS;
	const char *mode = this->player->modeService ? this->player->modeService->GetModeName() : "";
	V_strncpy(this->modeName, mode ? mode : "", sizeof(this->modeName));
	this->player->languageService->PrintChat(true, false, "Lead - Enabled", (int)this->path.size());
}

void KZLeadService::OnPhysicsSimulatePost()
{
	if (this->pending)
	{
		this->PollPending();
	}
	if (!this->enabled || this->path.empty())
	{
		return;
	}
	if (++this->ticksSinceUpdate < KZ_LEAD_UPDATE_TICKS)
	{
		return;
	}
	this->ticksSinceUpdate = 0;

	// Путь режим-зависим: в другом режиме он врал бы про траекторию. Отдельного хука на
	// смену режима у игрока нет, поэтому сверяем имя здесь — раз в 32 тика и без аллокаций
	// (GetModeName отдаёт литерал сервиса, а не собранную строку).
	const char *mode = this->player->modeService ? this->player->modeService->GetModeName() : "";
	if (!mode || !KZ_STREQI(this->modeName, mode))
	{
		this->Disable("mode_changed");
		return;
	}

	if (!this->player->GetPlayerPawn())
	{
		// Нет пешки (мёртв, спектейт) — окно не двигаем, но и не снимаем: вернётся сам.
		return;
	}
	this->UpdateWindow();
}

void KZLeadService::UpdateNearest(const Vector &origin)
{
	const u32 count = (u32)this->path.size();
	u32 bestIdx = this->nearest < count ? this->nearest : 0;
	f32 best = FLT_MAX;

	if (!this->resync)
	{
		// Вперёд от прошлой ближайшей до конца окна: игрок идёт по маршруту, полный скан
		// пути (десятки тысяч вершин) каждые полсекунды не нужен.
		const u32 to = this->windowTo > this->nearest ? this->windowTo : this->nearest;
		for (u32 i = bestIdx; i <= to && i < count; i++)
		{
			const f32 d = (this->path[i].pos - origin).LengthSqr();
			if (d < best)
			{
				best = d;
				bestIdx = i;
			}
		}
	}

	if (this->resync || best > KZ_LEAD_RESEARCH_DIST * KZ_LEAD_RESEARCH_DIST)
	{
		// Сошёл с маршрута (или только включил / телепортировался) — ищем по всему пути.
		best = FLT_MAX;
		for (u32 i = 0; i < count; i++)
		{
			const f32 d = (this->path[i].pos - origin).LengthSqr();
			if (d < best)
			{
				best = d;
				bestIdx = i;
			}
		}
	}

	this->nearest = bestIdx;
	this->resync = false;
}

void KZLeadService::UpdateWindow()
{
	Vector origin = vec3_invalid;
	this->player->GetOrigin(&origin);
	if (origin == vec3_invalid)
	{
		return;
	}

	this->UpdateNearest(origin);

	const u32 count = (u32)this->path.size();
	const u32 anchorTick = this->path[this->nearest].tickIdx;
	u32 from = this->nearest;
	while (from > 0 && anchorTick - this->path[from - 1].tickIdx <= KZ_LEAD_BACK_TICKS)
	{
		from--;
	}
	u32 to = this->nearest;
	while (to + 1 < count && this->path[to + 1].tickIdx - anchorTick <= KZ_LEAD_AHEAD_TICKS)
	{
		to++;
	}

	i64 configured = KZOptionService::GetOptionInt("cybLeadMaxSegments", 64);
	if (configured < 1)
	{
		configured = 1;
	}
	if (configured > KZ_LEAD_MAX_SEGMENTS_CAP)
	{
		configured = KZ_LEAD_MAX_SEGMENTS_CAP;
	}
	const u32 maxSegments = (u32)configured;
	if (to - from > maxSegments)
	{
		// Потолок отрезков. Смысл луча — подсказка ВПЕРЁД, поэтому хвосту позади ближайшей
		// вершины оставляем не больше четверти бюджета: плотная петля пути (стояние,
		// слайд) даёт на секунду назад больше вершин, чем весь потолок, и без этого
		// резерва передний конец окна оказался бы пуст — луч выродился бы в след под
		// ногами. Три четверти бюджета остаются впереди; режется дальний передний конец.
		const u32 backBudget = maxSegments / KZ_LEAD_BACK_BUDGET_DIV;
		if (this->nearest - from > backBudget)
		{
			from = this->nearest - backBudget;
		}
		// Кламп по КОНЦУ пути обязателен: у финиша `from + maxSegments` уходит за последнюю
		// вершину, и ApplyWindow читал бы path[newFrom + i + 1] за концом вектора.
		to = (std::min)(from + maxSegments, count - 1);
		if (from > to)
		{
			from = to;
		}
	}

	// Инвариант окна: ApplyWindow читает path[to] (и path[newFrom + i + 1] до него), поэтому
	// обе границы обязаны лежать В пути. Держим его ЗДЕСЬ, одной проверкой, а не выводим из
	// двух циклов и клампа выше: цена — два сравнения дважды в секунду, а цена ошибки —
	// чтение за концом вектора.
	if (to >= count)
	{
		to = count - 1;
	}
	if (from > to)
	{
		from = to;
	}
	this->ApplyWindow(from, to);
}

void KZLeadService::ApplyWindow(u32 newFrom, u32 newTo)
{
	const u32 want = newTo - newFrom;
	if (newFrom == this->windowFrom && newTo == this->windowTo && this->segments.size() == want)
	{
		return;
	}

	// Пересечение со старым окном есть только при движении вперёд (общий хотя бы один
	// отрезок). Иначе — снять всё и построить заново: считать хвост дешевле, чем
	// поддерживать вставку с обоих концов.
	// Инкрементальный путь требует, чтобы число сущностей ровно соответствовало прошлому
	// окну: иначе индекс отрезка разъедется с индексом вершины и луч пойдёт не по пути
	// (сущность могла не создаться — лимит энтити).
	const bool aligned = this->segments.size() == (size_t)(this->windowTo - this->windowFrom);
	const bool overlap = aligned && !this->segments.empty() && newFrom >= this->windowFrom && newFrom < this->windowTo;
	if (!overlap)
	{
		this->ClearSegments(false);
	}
	else
	{
		const u32 drop = newFrom - this->windowFrom;
		if (drop > 0)
		{
			for (u32 i = 0; i < drop && i < this->segments.size(); i++)
			{
				RemoveLeadSegment(this->segments[i]);
			}
			this->segments.erase(this->segments.begin(), this->segments.begin() + (std::min)((size_t)drop, this->segments.size()));
		}
		while (this->segments.size() > want)
		{
			RemoveLeadSegment(this->segments.back());
			this->segments.pop_back();
		}
	}

	for (u32 i = (u32)this->segments.size(); i < want; i++)
	{
		const Vertex &a = this->path[newFrom + i];
		const Vertex &b = this->path[newFrom + i + 1];
		this->segments.push_back(CreateLeadSegment(a.pos, b.pos, SegmentColor(a.onGround)));
	}

	this->windowFrom = newFrom;
	this->windowTo = newTo;
	this->RebuildOwnedIndex();
}

SCMD(kz_lead, SCFL_REPLAY | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player)
	{
		return MRES_SUPERCEDE;
	}
	const char *arg = args->ArgC() >= 2 ? args->Arg(1) : "awr";
	if (KZ_STREQI(arg, "off"))
	{
		player->leadService->Disable("command");
		return MRES_SUPERCEDE;
	}
	CybReplayDownload::Kind kind = CybReplayDownload::Kind::AWR;
	if (KZ_STREQI(arg, "wr"))
	{
		kind = CybReplayDownload::Kind::WR;
	}
	else if (KZ_STREQI(arg, "pb"))
	{
		kind = CybReplayDownload::Kind::PB;
	}
	player->leadService->Toggle(kind);
	return MRES_SUPERCEDE;
}
