#include "kz_lead.h"

#include "cs2kz.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
// GetCyberCourseNumber/GetCourse — ключ курса, под который построен путь (см. OnPathLoaded).
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/option/kz_option.h"
#include "kz/timer/kz_timer.h"
#include "kz/replays/awr_cut.h"
#include "kz/replays/data.h"
#include "kz/replays/playback.h"

#include "sdk/entity/cparticlesystem.h"
#include "sdk/entity/cbeam.h"
#include "entitykeyvalues.h"
// Цепочка C++-классов созданной сущности (m_pClassInfo) — по ней ищутся ДОПОЛНИТЕЛЬНЫЕ поля
// луча из рецепта пробника, см. ResolveLeadBeamExtras.
#include "entity2/entityclass.h"
#include "utils/schema.h"

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
// Граница прямого скана ближайшей вершины в режиме БЕЗ ЛУЧА (только процент в худе): вперёд по
// накопленной длине маршрута, юниты. За 32 тика игрок не уходит дальше даже на бхопе; при
// включённом луче граница другая — конец окна луча, см. UpdateNearest.
#define KZ_LEAD_SCAN_UNITS 1024.0f
// Страховка к границе выше: сверхплотная петля (стояние, слайд) может уложить в 1024 юнита
// тысячи вершин, а стоимость скана обязана оставаться ограниченной.
#define KZ_LEAD_SCAN_MAX_VERTS 1024u
// Сколько control point'ов бывает у системы частиц: MAX_PARTICLE_CONTROL_POINTS из
// public/particles/particles.h:137 (движок на выходе за границу бьёт Assert'ом). Своя константа
// здесь, чтобы не тащить в файл тяжёлый particles.h ради одного числа.
#define KZ_LEAD_CP_COUNT 64
// Пауза между МОЛЧАЛИВЫМИ загрузками пути, в проходах дросселированной ветки (32 тика каждый):
// 8 проходов = 4 с. Защита от мигающего ключа и от частых повторов после сетевого отказа.
#define KZ_LEAD_ARM_COOLDOWN_CYCLES 8
// Сколько раз повторить молчаливую загрузку под ОДНИМ ключом, прежде чем считать, что записи
// нет. Только для ПУСТОГО пути (нет файла / сеть): 404 и сетевую ошибку различить нечем. Отказ
// РАЗБОРА скачанного файла детерминирован и защёлкивается сразу — см. OnLoadFailed(retryable).
#define KZ_LEAD_FAIL_RETRIES 2
// Верхняя граница потолка отрезков: защита от опечатки в конфиге (лимит сущностей движка).
#define KZ_LEAD_MAX_SEGMENTS_CAP 512
// Какая доля потолка отрезков может уйти на хвост ПОЗАДИ ближайшей вершины (1/4).
// Остальное резервируется под подсказку впереди.
#define KZ_LEAD_BACK_BUDGET_DIV 4

#define KZ_LEAD_PARTICLE "particles/ui/annotation/ui_annotation_line_segment.vpcf"
// KZ_LEAD_TARGETNAME (имя отрезка), KZ_LEAD_BEAM_CLASSNAME (энтити-класс луча) и
// KZ_LEAD_SEGMENT_TEAM (метка «наша энтити») переехали в kz_lead.h: их читает ещё и фильтр
// передачи (kz_quiet.cpp).
//
// Ширина луча по умолчанию. 2 — значение владельца серверов по итогам живого перебора 11.09.
// Верхняя граница конвара — защита от опечатки: луч шириной в сотни юнитов закрыл бы игроку
// пол-экрана. Нижняя (в LeadBeamWidth) — от нуля и NaN, при которых луч невидим.
#define KZ_LEAD_BEAM_WIDTH_DEFAULT 2.0f
#define KZ_LEAD_BEAM_WIDTH_MIN     0.1f
#define KZ_LEAD_BEAM_WIDTH_MAX     64.0f
// Пауза между одинаковыми жалобами в лог (секунды). Все отказы этого файла сидят в путях,
// которые зовутся до 384 раз за перерисовку и дважды в секунду, поэтому «строка на каждый
// отказ» здесь означала бы залп. Дросселируем по времени, а не «один раз на процесс»: инвариант
// логирования требует reason на КАЖДЫЙ отказ, а защёлка на весь запуск съедала бы квоту первым
// же случаем и молчала бы про все следующие — другой природы и на другой карте.
#define KZ_LEAD_WARN_THROTTLE_SEC 60.0
// Сколько ПЕРЕСБОРОК пути (повторных разборов файла под новый допуск) разрешено вести
// одновременно на весь сервер. У первичных загрузок такого потолка нет и не нужно: их разносит
// сама сеть (резолв + докачка на каждого), а пересборка идёт с диска и стартует у всех от
// ОДНОЙ правки конвара. Каждый разбор держит в памяти файл целиком (до 32 МБ) плюс
// распакованные кадры, поэтому потолок жёсткий; кому не хватило места — подождёт своего
// прохода, метка пересборки с него не снимается.
#define KZ_LEAD_REBUILD_BUDGET 2

using namespace KZ::replaysystem;

// Занятые места бюджета выше. Инкремент — на главном потоке перед стартом потока, декремент —
// САМИМ рабочим потоком в конце разбора: только так место освобождается и тогда, когда игрок
// ушёл, а сервис с его pending давно удалён.
static std::atomic<i32> g_leadRebuildsInFlight {0};

// === Живая настройка вида луча (конвары, а НЕ cybLead* из серверного cfg) ===================
// Серверный cfg (KZOptionService::GetOptionInt/Float) читается ОДИН раз в KZPlugin::Load — по
// RCON он не правится, и перебирать им варианты на канарейке нельзя. Всё, что пользователь
// собирается крутить живьём, объявлено здесь конварами (образец — kz_chat_mode,
// utils/utils_print.cpp): значение применяется к СЛЕДУЮЩЕЙ перерисовке отрезков, а колбэк
// изменения перерисовывает их сразу всем, у кого луч включён.
//
// Дефолты: вид луча остаётся ТЕКУЩИМ (тот же ассет, никаких доп. control point'ов), меняется
// только густота точек — её и просил пользователь.
//
// Колбэк изменения у CConVar ТИПИЗИРОВАН (FnTypedChangeCallback_t<T>), поэтому одного общего
// указателя на всех не бывает: каждый конвар отдаёт свою лямбду без захвата (идиома
// utils/logging.cpp), а тело у них одно — функции ниже.

// Годен ли индекс CP для пробы. Проверяем ОБЕ границы и две занятые позиции: цена ошибки здесь
// не падение, а ложный вывод «этот CP ничего не меняет», — а вся ценность механизма в том, что
// результату перебора можно верить.
//   * сверху KZ_LEAD_CP_COUNT: индекс уезжает в uint8-массив назначений
//     (m_iServerControlPointAssignments, sdk/entity/cparticlesystem.h), поэтому 256 и больше
//     усеклось бы МОЛЧА в другой индекс, а движок и так не знает CP дальше 63;
//   * 255 — сентинел «слот свободен» в том же массиве: проба на нём не зарегистрировалась бы;
//   * 1 и 16 заняты собственными keyvalue отрезка (data_cp = конец, tint_cp = цвет) — запись
//     поверх них ломает геометрию или цвет, а не «толщину».
static_function bool LeadCpIndexUsable(i32 index)
{
	return index >= 0 && index < KZ_LEAD_CP_COUNT && index != 1 && index != 16;
}

// Перерисовать отрезки всем, у кого луч включён: значения читаются при создании сущностей,
// поэтому «применить сейчас» = снять и построить заново по УЖЕ загруженному пути (сам путь не
// пересобирается, сети это не стоит).
static_function void LeadLookChanged()
{
	KZLeadService::RefreshAllSegments("cvar");
}

// Отказ по индексу печатаем ЗДЕСЬ, в колбэке конвара (один раз на правку), а не в
// CreateLeadSegment: тот зовётся на каждый отрезок, и вышел бы залп до 384 строк за тик.
static_function void LeadCpIndexChanged(i32 index)
{
	if (index != -1 && !LeadCpIndexUsable(index))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[lead] cp_index_rejected value=%i reason=%s allowed=0..63_except_1_and_16 note=-1_disables\n", index,
					index < 0 ? "negative"
							  : (index == 255 ? "sentinel_free_slot"
											  : (index >= KZ_LEAD_CP_COUNT ? "out_of_range" : (index == 1 ? "data_cp_taken" : "tint_cp_taken"))));
	}
	LeadLookChanged();
}

// Допуск работает на СБОРКЕ пути (рабочий поток разбора файла), а не на отрисовке, — одной
// перерисовкой отрезков его не применить. Поэтому правка конвара сама помечает живые пути под
// пересборку: файл уже в кэше downloads/, повторный разбор идёт на рабочем потоке, игрок
// ничего не набирает. Прежний совет «!lead off + !lead» был вдобавок неверен: при включённом
// элементе худа «Прогресс» путь держится постоянно, Disable его не освобождает, и повторный
// !lead поднимал луч по ТЕМ ЖЕ вершинам.
static_function void LeadRdpChanged(f32 value)
{
	KZ_LOG_INFO(LogChannel::Replays, "[lead] rdp_tolerance_changed value=%.2f note=live_paths_rebuilt_from_cached_replay\n", value);
	KZLeadService::RebuildAllPaths("cvar_rdp");
}

// Ассет отрезка. Дефолт — стоковая линия-аннотация (тот же примитив у !measure и рёбер зон).
// ВАЖНО: клиент получает только ассеты из манифеста ресурсов (utils/hooks.cpp,
// Hook_BuildGameSessionManifest). Там зарегистрированы РОВНО ДВА пути:
//   particles/ui/annotation/ui_annotation_line_segment.vpcf — линия между двумя точками;
//   particles/ui/hud/ui_map_def_utility_trail.vpcf          — трейл луча игрока (!beam).
// Любой другой путь, выставленный этим конваром, у клиента не прекешируется и, скорее всего,
// не нарисуется вовсе — новый ассет требует правки манифеста и пересборки.
CConVar<CUtlString> cyb_lead_particle("cyb_lead_particle", FCVAR_NONE,
									  "Lead segment particle asset. Only manifest-registered assets render: "
									  "particles/ui/annotation/ui_annotation_line_segment.vpcf (default), "
									  "particles/ui/hud/ui_map_def_utility_trail.vpcf.",
									  CUtlString(KZ_LEAD_PARTICLE),
									  [](CConVar<CUtlString> *, CSplitScreenSlot, const CUtlString *, const CUtlString *) { LeadLookChanged(); });

// Проба control point'ов чужого ассета: индекс и значение. Чем именно управляют CP стоковой
// ui_annotation_line_segment.vpcf — НЕИЗВЕСТНО (ассет скомпилирован, файлов игры на машине
// сборки нет, разобрать его нечем), поэтому «толщина» и «скрыть точки на концах» здесь не
// зашиты, а ищутся живьём: выставить индекс, выставить значение, посмотреть на луч.
// Известно только про два CP, которые уже используются (и идут не через эти поля, а через
// свои keyvalue): data_cp=1 — КОНЕЦ отрезка, tint_cp=16 — цвет.
// -1 — CP не задавать (дефолт: вид не меняется). Слотов серверных CP у сущности всего четыре
// (SetControlPointValue, sdk/entity/cparticlesystem.h), поэтому проб здесь две.
CConVar<i32> cyb_lead_cp1_index("cyb_lead_cp1_index", FCVAR_NONE,
								"Extra server control point index for the lead segment: 0..63 except 1 (data_cp) and 16 (tint_cp); -1 = unused.", -1,
								[](CConVar<i32> *, CSplitScreenSlot, const i32 *newValue, const i32 *)
								{ LeadCpIndexChanged(newValue ? *newValue : -1); });
CConVar<Vector> cyb_lead_cp1_value("cyb_lead_cp1_value", FCVAR_NONE, "Value written to cyb_lead_cp1_index (x y z).", Vector(0.0f, 0.0f, 0.0f),
								   [](CConVar<Vector> *, CSplitScreenSlot, const Vector *, const Vector *) { LeadLookChanged(); });
CConVar<i32> cyb_lead_cp2_index("cyb_lead_cp2_index", FCVAR_NONE,
								"Second extra server control point index: 0..63 except 1 (data_cp) and 16 (tint_cp); -1 = unused.", -1,
								[](CConVar<i32> *, CSplitScreenSlot, const i32 *newValue, const i32 *)
								{ LeadCpIndexChanged(newValue ? *newValue : -1); });
CConVar<Vector> cyb_lead_cp2_value("cyb_lead_cp2_value", FCVAR_NONE, "Value written to cyb_lead_cp2_index (x y z).", Vector(0.0f, 0.0f, 0.0f),
								   [](CConVar<Vector> *, CSplitScreenSlot, const Vector *, const Vector *) { LeadLookChanged(); });

// Живое переопределение потолка отрезков. cybLeadMaxSegments из серверного cfg живьём не
// правится (см. шапку блока), а перебирать густоту без потолка бессмысленно: вдвое более
// частые вершины при прежнем потолке дают луч ВДВОЕ КОРОЧЕ. -1 — брать значение из cfg.
// Замер на канарейке (11.09) показал, что платим мы именно ПОТОЛКОМ, а не допуском: сам допуск
// на тик-тайм не влияет вовсе, а число сущностей ограничено этим числом — см. CYBER.md.
CConVar<i32> cyb_lead_max_segments("cyb_lead_max_segments", FCVAR_NONE,
								   "Override cybLeadMaxSegments from the server cfg (-1 = use cfg value; hard cap 512).", -1,
								   [](CConVar<i32> *, CSplitScreenSlot, const i32 *, const i32 *) { LeadLookChanged(); });

// Допуск упрощения пути (юниты): меньше — вершины ГУЩЕ (и сущностей больше), больше —
// срезанные углы. Дефолт 0.25 — выбран пользователем живым перебором на канарейке 11.09 (по
// картинке: 2.0 → 1.0 → 0.25). На стоимость допуск не влияет: замер 64.00 тика на всех
// значениях от 2 до 0.01, разброс загрузки внутри одного значения больше, чем между ними
// (таблица в CYBER.md). Платим мы потолком отрезков — вместе с этим дефолтом он поднят до 384,
// иначе более густые вершины укоротили бы луч вперёд. Применяется при СБОРКЕ пути, но правка
// конвара сама пересобирает живые пути из кэшированного файла (см. колбэк), то есть действует
// без команд игрока.
CConVar<f32> cyb_lead_rdp("cyb_lead_rdp", FCVAR_NONE,
						  "Path simplification tolerance in units; lower = denser vertices. Live paths are rebuilt from the cached "
						  "replay file when this changes.",
						  0.25f,
						  [](CConVar<f32> *, CSplitScreenSlot, const f32 *newValue, const f32 *) { LeadRdpChanged(newValue ? *newValue : 0.0f); });

// Примитив отрезка. Дефолт — ШТАТНАЯ сущность-луч (`beam`, CBeam): решение владельца серверов
// по итогам живой пробы 11.09. Ноль возвращает прежний путь на info_particle_system —
// дорога назад одной командой, без пересборки, если в бою луч окажется хуже частиц.
// Смешанных окон не бывает: колбэк перерисовывает отрезки, а RefreshSegments обнуляет окно,
// из-за чего ApplyWindow не находит пересечения и снимает ВСЕ прежние сущности разом.
CConVar<bool> cyb_lead_beam_entity("cyb_lead_beam_entity", FCVAR_NONE,
								   "Draw !lead segments with the native beam entity (default) instead of info_particle_system.", true,
								   [](CConVar<bool> *, CSplitScreenSlot, const bool *, const bool *) { LeadLookChanged(); });

// Ширина отрезка-луча в юнитах (m_fWidth/m_fEndWidth). Только для сущности-луча: у частицы
// толщину задаёт сам ассет, и этот конвар на неё не влияет. Применяется к СЛЕДУЮЩЕЙ
// перерисовке, но колбэк перерисовывает отрезки сразу всем, у кого луч включён, — то есть
// правка по RCON видна живьём, как у cyb_lead_particle.
CConVar<f32> cyb_lead_beam_width("cyb_lead_beam_width", FCVAR_NONE,
								 "Lead beam segment width in units (beam entity only): 0.1..64, default 2. Applied to the next segment "
								 "rebuild, which the change callback triggers right away.",
								 KZ_LEAD_BEAM_WIDTH_DEFAULT, [](CConVar<f32> *, CSplitScreenSlot, const f32 *, const f32 *) { LeadLookChanged(); });

namespace
{
	// Копия CreateMeasureBeam (kz_measure.cpp): тот же примитив, свой цвет на отрезок.
	// ПРЕЖНИЙ путь: живёт под cyb_lead_beam_entity 0 как дорога назад (см. конвар).
	CEntityHandle CreateLeadParticleSegment(const Vector &start, const Vector &end, const Color &color)
	{
		CParticleSystem *line = utils::CreateEntityByName<CParticleSystem>("info_particle_system");
		if (!line)
		{
			return CEntityHandle();
		}
		CEntityKeyValues *pKeyValues = new CEntityKeyValues();
		// Ассет — из конвара (живой перебор на канарейке); пустая строка = дефолтный.
		const CUtlString &asset = cyb_lead_particle.Get();
		const char *effect = (asset.Get() && asset.Get()[0]) ? asset.Get() : KZ_LEAD_PARTICLE;
		pKeyValues->SetString("effect_name", effect);
		pKeyValues->SetString("targetname", KZ_LEAD_TARGETNAME);
		pKeyValues->SetVector("origin", start);
		pKeyValues->SetInt("tint_cp", 16);
		pKeyValues->SetColor("tint_cp_color", color);
		pKeyValues->SetInt("data_cp", 1);
		pKeyValues->SetVector("data_cp_value", end);
		pKeyValues->SetBool("start_active", true);
		line->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
		line->DispatchSpawn(pKeyValues);
		// Пробные control point'ы — ПОСЛЕ спавна: SetControlPointValue пишет схему сущности
		// (m_vServerControlPoints + NetworkStateChanged), до DispatchSpawn писать нечего.
		// Индекс -1 (дефолт) не пишем вовсе, чтобы вид по умолчанию не менялся.
		// Отказ («нет свободных серверных CP») сюда не приходит молча: SetControlPointValue сам
		// печатает Warning, а слотов четыре против наших двух — упереться в них нельзя.
		// Негодный индекс пропускаем МОЛЧА: отказ уже назван колбэком конвара
		// (LeadCpIndexChanged), здесь он повторился бы на каждый отрезок.
		const i32 cp1 = cyb_lead_cp1_index.Get();
		if (LeadCpIndexUsable(cp1))
		{
			line->SetControlPointValue(cp1, cyb_lead_cp1_value.Get());
		}
		const i32 cp2 = cyb_lead_cp2_index.Get();
		if (LeadCpIndexUsable(cp2) && cp2 != cp1)
		{
			line->SetControlPointValue(cp2, cyb_lead_cp2_value.Get());
		}
		return line->GetRefEHandle();
	}

	// Дроссель жалоб в лог: true — пора печатать. Часы движка на смене карты ОБНУЛЯЮТСЯ
	// (известная грабля форка с persistent-таймерами), поэтому отметку «из будущего» трактуем
	// как рестарт часов и разрешаем печать сразу — иначе после смены карты канал молчал бы.
	bool LeadWarnDue(f64 &last)
	{
		const f64 now = g_pKZUtils->GetServerGlobals() ? g_pKZUtils->GetServerGlobals()->realtime : 0.0;
		if (now < last)
		{
			last = -1.0e9;
		}
		if (now - last < KZ_LEAD_WARN_THROTTLE_SEC)
		{
			return false;
		}
		last = now;
		return true;
	}

	// === Доп. поля луча из рецепта ПРОБНИКА =====================================================
	// «Луч виден игроку» доказано на канарейке для СУПЕРМНОЖЕСТВА полей, которое писал пробник, а
	// не для нашей пятёрки, и сырого лога той пробы у нас нет — выписать «что отдало set=ok»
	// нечем. Поэтому здесь повторяется не список, а сам МЕХАНИЗМ пробника: поле пишется, только
	// если оно есть в живой схеме реального класса этой сущности И его ширина совпала с тем, что
	// мы пишем; нет поля — тихо пропускаем, ровно как set=missing у пробника. Значения — те же,
	// что писала проба.
	//
	// Объявить их через SCHEMA_FIELD нельзя: на отсутствующем поле макрос даёт офсет 0, то есть
	// запись в голову объекта. Именно поэтому в cbeam.h лежат только доказанные пять.
	struct LeadBeamExtra
	{
		const char *name;
		bool isFloat;
		f32 floatValue;
		i64 intValue;
	};

	// clang-format off
	const LeadBeamExtra g_leadBeamExtras[] = {
		{"m_flFrameRate",     true,  0.0f, 0},
		{"m_flHDRColorScale", true,  1.0f, 0},
		{"m_flFadeLength",    true,  0.0f, 0},
		{"m_nBeamType",       false, 0.0f, 1}, // BEAM_POINTS в нумерации Source 1
		{"m_nBeamFlags",      false, 0.0f, 0},
		{"m_nNumBeamEnts",    false, 0.0f, 0},
		{"m_nHaloIndex",      false, 0.0f, 0},
	};
	// clang-format on

	struct LeadBeamExtraResolved
	{
		u32 offset;
		int size;
		i16 chain;
		bool networked;
		bool found;
	};

	// Состояние разбора схемы. Глобальное и изменяемое, но ТОЛЬКО главного потока: пишется в
	// создании отрезка (тик/колбэк конвара), читается там же; в хуках движения не участвует.
	LeadBeamExtraResolved g_leadBeamExtraFields[KZ_ARRAYSIZE(g_leadBeamExtras)] {};
	bool g_leadBeamExtrasResolved = false;

	// Спрашиваем схему ОДИН раз, на первом созданном луче, — то есть ИЗ ИГРЫ, а не из
	// KZPlugin::Load: сверка схемы на загрузке отравляет кэш (networked=false у всего класса,
	// память schema-cache-poisoned-at-plugin-load). Кэш общий на все отрезки законно: класс
	// энтити у них один и тот же (KZ_LEAD_BEAM_CLASSNAME), значит и цепочка классов одна.
	void ResolveLeadBeamExtras(CBaseEntity *ent)
	{
		g_leadBeamExtrasResolved = true;
		if (!ent->m_pEntity || !ent->m_pEntity->m_pClass)
		{
			return;
		}
		// Буфер статический: 512 записей на стеке ради разовой сверки не нужны, а зовётся эта
		// функция единственный раз и только с главного потока.
		static schema::FieldDesc fields[512];
		for (CEntityClassInfo *info = ent->m_pEntity->m_pClass->m_pClassInfo; info; info = info->m_pBaseClassInfo)
		{
			const char *className = info->m_pszCPPClassname;
			if (!className)
			{
				break;
			}
			const int count = schema::GetClassFields(className, fields, (int)KZ_ARRAYSIZE(fields));
			for (int i = 0; i < count; i++)
			{
				if (!fields[i].name)
				{
					continue;
				}
				for (u32 w = 0; w < KZ_ARRAYSIZE(g_leadBeamExtras); w++)
				{
					if (g_leadBeamExtraFields[w].found || V_stricmp(fields[i].name, g_leadBeamExtras[w].name) != 0)
					{
						continue;
					}
					// Ширину не угадываем: запись 4 байт в однобайтовое поле затёрла бы соседей.
					// Целые пишем по ОБЪЯВЛЕННОЙ ширине (у Valve m_n* бывают однобайтовыми),
					// вещественные — только при точном совпадении, как делал пробник.
					const int size = fields[i].size;
					const bool sizeOk = g_leadBeamExtras[w].isFloat ? (size == (int)sizeof(f32)) : (size == 1 || size == 2 || size == 4 || size == 8);
					if (!sizeOk)
					{
						KZ_LOG_WARN(LogChannel::Replays, "[lead] beam_extra_skipped name=%s reason=size_mismatch declared_in=%s size=%i\n",
									g_leadBeamExtras[w].name, className, size);
						g_leadBeamExtraFields[w].found = true; // больше не искать: поле найдено, но негодное
						g_leadBeamExtraFields[w].size = 0;
						break;
					}
					g_leadBeamExtraFields[w].offset = fields[i].offset;
					g_leadBeamExtraFields[w].size = size;
					g_leadBeamExtraFields[w].networked = fields[i].networked;
					g_leadBeamExtraFields[w].chain = schema::FindChainOffset(className, hash_32_fnv1a_const(className));
					g_leadBeamExtraFields[w].found = true;
					break;
				}
			}
		}
		for (u32 w = 0; w < KZ_ARRAYSIZE(g_leadBeamExtras); w++)
		{
			KZ_LOG_INFO(LogChannel::Replays, "[lead] beam_extra name=%s resolved=%i offset=0x%X size=%i networked=%i\n", g_leadBeamExtras[w].name,
						g_leadBeamExtraFields[w].size > 0 ? 1 : 0, g_leadBeamExtraFields[w].offset, g_leadBeamExtraFields[w].size,
						g_leadBeamExtraFields[w].networked ? 1 : 0);
		}
	}

	// Ровно то, что делает Set() в SCHEMA_FIELD: сначала цепочка, иначе сама сущность.
	void ApplyLeadBeamExtras(CBaseEntity *ent)
	{
		if (!g_leadBeamExtrasResolved)
		{
			ResolveLeadBeamExtras(ent);
		}
		for (u32 w = 0; w < KZ_ARRAYSIZE(g_leadBeamExtras); w++)
		{
			const LeadBeamExtraResolved &f = g_leadBeamExtraFields[w];
			if (f.size <= 0)
			{
				continue;
			}
			const uintptr_t addr = reinterpret_cast<uintptr_t>(ent) + f.offset;
			if (g_leadBeamExtras[w].isFloat)
			{
				*reinterpret_cast<f32 *>(addr) = g_leadBeamExtras[w].floatValue;
			}
			else
			{
				const i64 value = g_leadBeamExtras[w].intValue;
				switch (f.size)
				{
					case 1:
						*reinterpret_cast<i8 *>(addr) = (i8)value;
						break;
					case 2:
						*reinterpret_cast<i16 *>(addr) = (i16)value;
						break;
					case 4:
						*reinterpret_cast<i32 *>(addr) = (i32)value;
						break;
					default:
						*reinterpret_cast<i64 *>(addr) = value;
						break;
				}
			}
			if (!f.networked)
			{
				continue;
			}
			if (f.chain != 0)
			{
				ChainNetworkStateChanged(reinterpret_cast<uintptr_t>(ent) + f.chain, f.offset);
			}
			else
			{
				EntityNetworkStateChanged(reinterpret_cast<uintptr_t>(ent), f.offset);
			}
		}
	}

	// Ширина луча из конвара, с клампом. Отрицательное, ноль и NaN — это опечатка оператора, а
	// не «луч без ширины»: такой отрезок был бы невидим, и игрок решил бы, что сломан !lead.
	// Сравнение написано как `!(want >= MIN)` намеренно: NaN проваливает ЛЮБОЕ сравнение,
	// поэтому только такая форма его ловит.
	f32 LeadBeamWidth()
	{
		const f32 want = cyb_lead_beam_width.Get();
		f32 use = want;
		const char *reason = nullptr;
		if (!(want >= KZ_LEAD_BEAM_WIDTH_MIN))
		{
			use = KZ_LEAD_BEAM_WIDTH_DEFAULT;
			reason = "below_min_or_nan";
		}
		else if (want > KZ_LEAD_BEAM_WIDTH_MAX)
		{
			use = KZ_LEAD_BEAM_WIDTH_MAX;
			reason = "above_max";
		}
		if (reason)
		{
			// Кламп — это отказ ОПЕРАТОРУ: в конваре он видит своё значение, а луч получает
			// другое. Значит машинная причина обязана быть в логе. Дроссель по времени: функция
			// зовётся на каждый отрезок, до 384 раз за перерисовку.
			static f64 lastWarn = -1.0e9;
			if (LeadWarnDue(lastWarn))
			{
				KZ_LOG_WARN(LogChannel::Replays, "[lead] beam_width_clamped value=%.3f used=%.3f reason=%s range=%.1f..%.1f\n", want, use, reason,
							KZ_LEAD_BEAM_WIDTH_MIN, KZ_LEAD_BEAM_WIDTH_MAX);
			}
		}
		return use;
	}

	// НОВЫЙ путь (дефолт): отрезок — штатная сущность-луч. Доказано живьём пробником
	// kz_beam_probe на канарейке 11.09: класс CBeam существует, поля сетевые, луч виден игроку
	// и БЕЗ заданного материала.
	CEntityHandle CreateLeadBeamSegment(const Vector &start, const Vector &end, const Color &color)
	{
		CBeam *beam = utils::CreateEntityByName<CBeam>(KZ_LEAD_BEAM_CLASSNAME);
		if (!beam)
		{
			// Отказ фабрики — это «луча нет вообще», и молча его оставлять нельзя: игрок увидит
			// пустоту и решит, что сломан !lead. Дроссель по ВРЕМЕНИ, а не защёлка на запуск:
			// функция зовётся до 384 раз за перерисовку (залп недопустим), но и отказ через час
			// работы, на другой карте и другой природы обязан попасть в лог.
			static f64 lastWarn = -1.0e9;
			if (LeadWarnDue(lastWarn))
			{
				KZ_LOG_WARN(LogChannel::Replays, "[lead] beam_create_failed class=%s note=falling_back_is_manual_set_cyb_lead_beam_entity_0\n",
							KZ_LEAD_BEAM_CLASSNAME);
			}
			return CEntityHandle();
		}
		const f32 width = LeadBeamWidth();
		// Ниже — РЕЦЕПТ ПРОБНИКА, повторённый целиком: порядок «поля до спавна → keyvalues →
		// DispatchSpawn → Teleport → поля ещё раз». Видимость луча на канарейке доказана именно
		// для него, и урезать его до «нужного нам минимума» нельзя: дефолты свежесозданной beam
		// нам неизвестны, а другого живого доказательства у нас нет.
		beam->m_vecEndPos(end);
		beam->m_fWidth(width);
		beam->m_fEndWidth(width);
		// 0 — прямая линия. Ненулевая амплитуда дала бы «волну», которая врёт о маршруте.
		beam->m_fAmplitude(0.0f);
		beam->m_bTurnedOff(false);
		// Альфу держим 255: осмысленность полупрозрачности зависит от m_nRenderMode, который мы
		// не задаём вовсе, — а «наполовину прозрачный луч» непроверен и на канарейке не нужен.
		beam->m_clrRender(color);
		// Метка «наша энтити» — дешёвый признак для фильтра передачи (kz_quiet.cpp): движку на
		// не-частице она ничего не значит, зато читается одним полем, без вызова в движок.
		beam->m_iTeamNum(KZ_LEAD_SEGMENT_TEAM);
		// Остальные поля рецепта — те, что есть в живой схеме (см. ApplyLeadBeamExtras).
		ApplyLeadBeamExtras(beam);

		CEntityKeyValues *pKeyValues = new CEntityKeyValues();
		pKeyValues->SetVector("origin", start);
		pKeyValues->SetString("targetname", KZ_LEAD_TARGETNAME);
		pKeyValues->SetFloat("width", width);
		// Остальные ключи — из того же рецепта пробника. Незнакомый ключ энтити просто
		// игнорирует, поэтому цена их присутствия нулевая, а отсутствия — неизвестна.
		// Материал (texture/material/BeamTexture) НЕ задаём намеренно: живая проба показала луч
		// именно с material=- («без него рисуется»).
		pKeyValues->SetFloat("BoltWidth", width);
		pKeyValues->SetFloat("life", 0.0f);  // 0 = луч не гаснет сам, снимаем его мы
		pKeyValues->SetInt("spawnflags", 1); // «start on» у env_beam в Source 1
		pKeyValues->SetBool("start_active", true);
		char renderColor[32];
		V_snprintf(renderColor, sizeof(renderColor), "%d %d %d", (int)color.r(), (int)color.g(), (int)color.b());
		pKeyValues->SetString("rendercolor", renderColor);
		pKeyValues->SetInt("renderamt", (int)color.a());
		// Хендл берём ДО спавна: класс может умереть прямо в DispatchSpawn, и тогда трогать
		// указатель уже нельзя (пробник ловил это же условие).
		const CEntityHandle handle = beam->GetRefEHandle();
		beam->DispatchSpawn(pKeyValues);
		if (!handle.Get())
		{
			return CEntityHandle();
		}
		// Позиция ещё раз после спавна — так делал пробник (спавн мог её сбросить).
		beam->Teleport(&start, nullptr, &vec3_origin);
		// И поля ещё раз: спавн читает свои keyvalue (width, spawnflags, rendercolor) и мог
		// переписать ими то, что мы поставили. Это не «на всякий случай» — это проверенный
		// порядок.
		beam->m_vecEndPos(end);
		beam->m_fWidth(width);
		beam->m_fEndWidth(width);
		beam->m_fAmplitude(0.0f);
		beam->m_bTurnedOff(false);
		beam->m_clrRender(color);
		ApplyLeadBeamExtras(beam);
		// Метку команды повторяем ПОСЛЕ спавна обязательно, и это не дубль ради симметрии: на
		// ней держится личная видимость луча. Сбрось её спавн (чем бы он ни выставлял команду) —
		// фильтр передачи перестал бы узнавать наши лучи; страховкой служит сверка targetname в
		// самом фильтре, но полагаться на страховку вместо метки нельзя.
		beam->m_iTeamNum(KZ_LEAD_SEGMENT_TEAM);
		return handle;
	}

	CEntityHandle CreateLeadSegment(const Vector &start, const Vector &end, const Color &color)
	{
		return cyb_lead_beam_entity.Get() ? CreateLeadBeamSegment(start, end, color) : CreateLeadParticleSegment(start, end, color);
	}

	// Снятие одного отрезка — ОБОИХ примитивов: сверка идёт по targetname, а он у частицы и у
	// луча один. Хендл сам по себе ничего не гарантирует: индекс энтити переиспользуется, и без
	// сверки targetname мы могли бы снести чужую сущность.
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
	// Защита от битого файла — НЕ языковыми исключениями: форк собирается с
	// `-fno-exceptions` (AMBuildScript), поймать их нечем в принципе. Абсурдные размеры из
	// шапок секций (по ним аллоцируют ReadTickSection и ReadEventsCompressed) отсекает пре-валидация внутри
	// data::LoadCutSourceFromMemory — она отдаёт valid=false, что здесь становится
	// parse_failed. Это единственное место такой защиты на весь тракт.
	void BuildPathWorker(std::string filePath, std::shared_ptr<KZLeadService::PendingLoad> pending)
	{
		// Допуск взят из снимка (см. PendingLoad::rdpTolerance): конвар живой, а рабочий поток
		// читать его не должен. Ноль/отрицательное значение = «не упрощать вовсе» — вершин
		// станут десятки тысяч, поэтому пол на 0.01 юнита.
		const f32 tol = pending->rdpTolerance > 0.01f ? pending->rdpTolerance : 0.01f;
		const f32 tolSqr = tol * tol;
		std::vector<KZLeadService::Vertex> out;
		const char *failReason = nullptr;
		const char *cutWarn = nullptr;

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
						SimplifyRange(raw, anchor, i, tolSqr, keep);
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

		{
			std::lock_guard<std::mutex> lock(pending->mu);
			pending->path = std::move(out);
			pending->failReason = failReason;
			pending->cutWarn = cutWarn;
		}
		if (pending->rebuildBudget)
		{
			// Место в бюджете освобождаем ДО метки готовности: к моменту, когда главный поток
			// увидит done, оно уже свободно, и очередь на пересборку не простаивает лишний проход.
			g_leadRebuildsInFlight--;
		}
		pending->done = true;
	}
} // namespace

void KZLeadService::Reset()
{
	// Дисконнект и late load: мир жив, сущности обязаны уйти.
	this->ResetState(false);
	// Преф прогресса — НАСТРОЙКА игрока, а не состояние карты: слот реально освобождается, и
	// новый владелец не должен унаследовать включённый процент прошлого (тот же класс улики,
	// что layoutPrefs в KZHUDService::Reset). Своё значение ему принесёт RefreshLayoutPrefs.
	this->progress = false;
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
	this->beam = false;
	this->ReleasePath();
	// Защёлку отказа снимаем явно: она ключевана курсом и режимом, а карта в ключ не входит —
	// на новой карте «курс 0 + тот же режим» совпал бы с прошлым отказом, и процент не
	// завёлся бы вовсе. Смены курса и режима ВНУТРИ карты защёлка различает сама.
	this->failedCourse = -1;
	this->failedMode[0] = '\0';
	this->failRetriesLeft = 0;
	// Разброс по слоту, а не 0: на смене карты ResetState зовётся всем сразу, и все, у кого
	// процент включён, ушли бы резолвить и качать ОДИН файл в одном тике (кэш downloads/ пуст,
	// дедупа запросов в полёте нет) — N докачек до 32 МБ и N потоков разбора вместо одной.
	this->armCooldown = this->player->GetPlayerSlot().Get() % KZ_LEAD_ARM_COOLDOWN_CYCLES;
	// Преф прогресса здесь НЕ трогаем: OnMapChanged зовёт этот метод на смене карты, а
	// настройка игрока карту переживает — иначе элемент худа молча умирал бы до следующего
	// захода в меню. Гасит его только Reset() (дисконнект, слот освободился).
}

void KZLeadService::ReleasePath()
{
	this->path.clear();
	this->path.shrink_to_fit();
	this->cumLen.clear();
	this->cumLen.shrink_to_fit();
	this->totalLen = 0.0f;
	this->progressPct = -1;
	this->loading = false;
	// Незавершённая загрузка протухает: её результат отбросит проверка поколения.
	this->generation++;
	this->pending.reset();
	this->beamOnLoad = false;
	this->windowFrom = 0;
	this->windowTo = 0;
	this->nearest = 0;
	this->resync = true;
	this->ticksSinceUpdate = 0;
	this->modeName[0] = '\0';
	this->pathCourse = -1;
	// Пересобирать больше нечего: файл пути отпущен вместе с ним.
	this->pathFile.clear();
	this->pathFile.shrink_to_fit();
	this->pathRdp = -1.0f;
	this->rebuildPending = false;
	this->rebuildDelay = 0;
	this->pendingIsRebuild = false;
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

void KZLeadService::RefreshSegments(const char *reason)
{
	if (!this->beam || this->path.empty())
	{
		// Луч выключен (или пути нет) — перерисовывать нечего: значения конваров прочитаются
		// сами, когда игрок включит луч.
		return;
	}
	const unsigned long long steamId = (unsigned long long)this->player->GetSteamId64();
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] segments_refresh reason=%s steam_id=%llu\n", reason, steamId);
	// Обнуляем ОКНО, но сущности здесь НЕ снимаем: снятие и постройку делает штатный проход
	// ApplyWindow, и делает их вместе. «Прошлого окна нет» для него означает отсутствие
	// пересечения — значит он снимет прежние отрезки и построит новые одним махом, а луч не
	// мигнёт пустотой в ожидании своей очереди.
	this->windowFrom = 0;
	this->windowTo = 0;
	// Разброс по слоту — та же идиома, что у armCooldown в ResetState: колбэк конвара зовёт
	// этот метод сразу ВСЕМ, и без разброса при четырёх включённых лучах в ОДИН тик пришлось
	// бы до 768 снятий и столько же созданий сущностей. Слот 0 перестроится на ближайшем
	// тике, слот 31 — через 32 (≤0.5 с, штатный шаг пересчёта окна), слоты 32+ делят фазу с
	// первыми — на глаз это незаметно, а пик размазан. Минус единица обязательна: проход идёт
	// при `++ticksSinceUpdate >= KZ_LEAD_UPDATE_TICKS`, поэтому без неё слоты 0 и 1 попали бы
	// в один тик, а один тик фазы остался бы пустым.
	const i32 slot = this->player->GetPlayerSlot().Get();
	this->ticksSinceUpdate = (u32)(KZ_LEAD_UPDATE_TICKS - 1 - (slot % KZ_LEAD_UPDATE_TICKS));
	// Побочный эффект обнуления окна: до своего прохода прямой скан ближайшей вершины
	// (UpdateNearest при включённом луче) ограничен ею же, то есть ближайшая может не сдвинуться.
	// Догоняет СЛЕДУЮЩИЙ проход (+32 тика): на том, который строит новое окно, UpdateNearest
	// ещё видит windowTo == 0. Практически незаметно — отход больше 300 юнитов даёт полный
	// ресинк тем же проходом, и мусора в проценте не возникает.
}

void KZLeadService::RefreshAllSegments(const char *reason)
{
	// Колбэк конвара срабатывает и на этапе загрузки конфигов, когда менеджера игроков ещё нет,
	// и на выгрузке плагина — обе проверки обязательны.
	if (g_KZPlugin.unloading || !g_pKZPlayerManager)
	{
		return;
	}
	// Граница строго `i < MAXPLAYERS` — та же, что в OnMapChanged (разбор там же).
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->leadService)
		{
			player->leadService->RefreshSegments(reason);
		}
	}
}

void KZLeadService::RebuildAllPaths(const char *reason)
{
	// Те же две проверки, что у RefreshAllSegments: колбэк конвара срабатывает и на загрузке
	// конфигов (менеджера игроков ещё нет), и на выгрузке плагина.
	if (g_KZPlugin.unloading || !g_pKZPlayerManager)
	{
		return;
	}
	// Граница строго `i < MAXPLAYERS` — та же, что в OnMapChanged (разбор там же).
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->leadService)
		{
			player->leadService->ArmPathRebuild(reason);
		}
	}
}

void KZLeadService::ArmPathRebuild(const char *reason)
{
	if (this->pathFile.empty() || this->path.empty())
	{
		// Пути нет (или он не от файла) — пересобирать нечего: новый допуск снимется снимком
		// в OnFileReady при ближайшей штатной загрузке.
		return;
	}
	if (!this->beam && !this->progress)
	{
		// Путь никому не нужен — тратить на него разбор незачем. Такого состояния быть не
		// должно (последний потребитель освобождает путь), но проверка дешевле допущения.
		return;
	}
	this->rebuildPending = true;
	// Разброс по слоту — В ПРОХОДАХ дросселированной ветки, как armCooldown в ResetState, а НЕ
	// в тиках, как в RefreshSegments. Там разброс на 32 тика оправдан: перерисовка отрезков
	// стоит главному потоку создания сущностей, и растягивать её на секунды незачем. Здесь
	// цена другая — чтение файла до 32 МБ и распакованные кадры в памяти на КАЖДЫЙ разбор, и
	// сетевой фазы, которая разносит первичные загрузки сама, у пересборки нет вовсе. Поэтому
	// единица та же, что у пауз загрузки: до KZ_LEAD_ARM_COOLDOWN_CYCLES проходов (≈4 с).
	// Фазу ticksSinceUpdate при этом НЕ трогаем: сдвигать проценту его шаг ради пересборки
	// незачем, а разброс даёт rebuildDelay.
	this->rebuildDelay = this->player->GetPlayerSlot().Get() % KZ_LEAD_ARM_COOLDOWN_CYCLES;
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] path_rebuild_armed reason=%s steam_id=%llu\n", reason,
				 (unsigned long long)this->player->GetSteamId64());
}

void KZLeadService::StartPathRebuild()
{
	if (this->loading || this->pending)
	{
		// Загрузка уже идёт: её снимок допуска либо свежий (снимается в OnFileReady, то есть
		// уже с новым значением), либо устареет — и тогда эта же ветка переспросит пересборку
		// следующим проходом. Флаг держим.
		return;
	}
	if (this->pathFile.empty() || this->path.empty())
	{
		this->rebuildPending = false;
		return;
	}
	const f32 tol = cyb_lead_rdp.Get();
	if (tol == this->pathRdp)
	{
		// Значение вернули к тому, которым путь и построен (или путь уже пересобран под него).
		this->rebuildPending = false;
		return;
	}
	if (g_leadRebuildsInFlight.load() >= KZ_LEAD_REBUILD_BUDGET)
	{
		// Мест нет — ждём своего прохода. Метку НЕ снимаем: пересборка обязана состояться,
		// просто позже. Разброс по слоту делает очередь детерминированной, а не гонкой.
		return;
	}
	this->rebuildPending = false;
	// Поколение НЕ увеличиваем: путь остаётся тем же и живым, а результат пересборки обязан
	// пройти тот же гейт поколения — ReleasePath (смена курса/режима/карты) его отбросит.
	this->pending = std::make_shared<PendingLoad>();
	this->pending->generation = this->generation;
	// Снимок допуска — здесь, на главном потоке (см. PendingLoad::rdpTolerance).
	this->pending->rdpTolerance = tol;
	// Место в бюджете занимает САМ разбор, а не наш pending: освободит его рабочий поток, даже
	// если к тому времени и pending, и сервис уже мертвы.
	this->pending->rebuildBudget = true;
	g_leadRebuildsInFlight++;
	this->pendingIsRebuild = true;
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] path_rebuild_start rdp=%.2f course=%i mode=%s steam_id=%llu\n", tol, this->pathCourse, this->modeName,
				 (unsigned long long)this->player->GetSteamId64());
	// Копия пути к файлу, а не move: он нужен и следующим пересборкам.
	std::thread(BuildPathWorker, this->pathFile, this->pending).detach();
}

void KZLeadService::RebuildOwnedIndex()
{
	this->ownedSorted = this->segments;
	std::sort(this->ownedSorted.begin(), this->ownedSorted.end(), [](const CEntityHandle &a, const CEntityHandle &b) { return a < b; });
}

bool KZLeadService::OwnsSegmentEntity(const CEntityHandle &handle) const
{
	// Горячий путь CheckTransmit: на каждую помеченную энтити, на каждого получателя.
	// Отсюда двоичный поиск (до 384 отрезков × десятки помеченных энтити × число игроков линейным
	// сканом — десятки тысяч сравнений за тик).
	return std::binary_search(this->ownedSorted.begin(), this->ownedSorted.end(), handle,
							  [](const CEntityHandle &a, const CEntityHandle &b) { return a < b; });
}

void KZLeadService::Disable(const char *reason)
{
	// Пересборка (pendingIsRebuild) в счёт не идёт: её заказывал не игрок, а правка конвара, и
	// «выключено» про включённый только процент было бы неправдой.
	if (!this->beam && !this->loading && !(this->pending && !this->pendingIsRebuild))
	{
		// `!lead off` при выключенном луче: «выключен» было бы неправдой. Включённый процент
		// в худе лучом не является и этой командой не выключается (он живёт в меню настроек).
		this->player->languageService->PrintChat(true, false, "Lead - Not Enabled");
		return;
	}
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] disable reason=%s steam_id=%llu\n", reason,
				 (unsigned long long)this->player->GetSteamId64());
	this->beam = false;
	this->beamOnLoad = false;
	this->ClearSegments(false);
	// Путь освобождаем ТОЛЬКО когда его больше некому держать: при включённом проценте он
	// остаётся, и повторный `!lead` поднимет луч по тем же вершинам, без резолва и докачки.
	if (!this->progress)
	{
		this->ReleasePath();
	}
	// Все пути Disable — от игрока (!lead off, повторный !lead, смена режима), поэтому
	// подтверждение в чат безусловно.
	this->player->languageService->PrintChat(true, false, "Lead - Disabled");
}

void KZLeadService::Toggle(CybReplayDownload::Kind kind)
{
	if (this->beam)
	{
		this->Disable("toggle");
		return;
	}
	// Явная команда игрока — повод сходить в сеть заново, даже если молчаливая загрузка под
	// этот ключ уже отказала и защёлкнулась (и не дожидаясь её паузы).
	this->failedCourse = -1;
	this->failedMode[0] = '\0';
	this->failRetriesLeft = 0;
	this->armCooldown = 0;
	if (!this->path.empty())
	{
		if (this->PathKeyMatchesCurrent())
		{
			// Путь уже жив (его держит элемент «Прогресс» худа) и построен под ТЕКУЩИЙ
			// курс+режим — луч поднимается по тем же вершинам, без резолва и докачки.
			this->beam = true;
			this->resync = true;
			// Окно строится на ближайшем же тике, а не через полсекунды.
			this->ticksSinceUpdate = KZ_LEAD_UPDATE_TICKS;
			this->player->languageService->PrintChat(true, false, "Lead - Enabled", (int)this->path.size());
			return;
		}
		// Путь чужого ключа (его держит процент, а игрок уже ушёл на другой курс): показать по
		// нему луч и отчитаться «включён» значило бы нарисовать маршрут ДРУГОГО курса. `!lead`
		// до этой задачи ВСЕГДА резолвил под текущий ключ — это поведение обязано остаться.
		this->ReleasePath();
	}
	if (this->loading || this->pending)
	{
		if (this->RequestKeyMatchesCurrent())
		{
			// Загрузка уже идёт (сеть или разбор) под ТЕКУЩИЙ ключ — второй запрос ничего не
			// ускорит, но завёл бы ещё один резолв, ещё одну докачку и ещё один поток. Если её
			// начал процент (молча), метим результат как ожидаемый ИГРОКОМ: придёт путь —
			// поднимем луч и отчитаемся.
			this->beamOnLoad = true;
			this->player->languageService->PrintChat(true, false, "Lead - Loading");
			return;
		}
		// Идущая загрузка ушла под ДРУГОЙ курс/режим (её начал процент, а игрок с тех пор
		// сменил курс). Подхватить её значило бы поднять луч по чужому маршруту и отчитаться
		// «включён» — ровно то, что запрещает инвариант ветки выше: `!lead` всегда показывает
		// маршрут ТЕКУЩЕГО ключа. Протухший запрос бросаем (поколение отбросит его результат).
		this->ReleasePath();
	}

	this->player->languageService->PrintChat(true, false, "Lead - Loading");
	this->RequestPath(kind, true);
}

i32 KZLeadService::CurrentCourseKey() const
{
	return KZ::course::GetCyberCourseNumber(this->player->timerService->GetCourse());
}

const char *KZLeadService::CurrentModeName() const
{
	const char *mode = this->player->modeService ? this->player->modeService->GetModeName() : "";
	return mode ? mode : "";
}

bool KZLeadService::PathKeyMatchesCurrent() const
{
	return this->pathCourse == this->CurrentCourseKey() && KZ_STREQI(this->modeName, this->CurrentModeName());
}

bool KZLeadService::RequestKeyMatchesCurrent() const
{
	return this->requestCourse == this->CurrentCourseKey() && KZ_STREQI(this->requestMode, this->CurrentModeName());
}

void KZLeadService::RequestPath(CybReplayDownload::Kind kind, bool fromPlayer)
{
	this->loading = true;
	this->beamOnLoad = fromPlayer;
	// Ключ запоминаем СЕЙЧАС: под него уходит резолв (CybReplayDownload::BuildKey читает те же
	// курс и режим), и именно им будет подписан пришедший путь.
	this->requestCourse = this->CurrentCourseKey();
	V_strncpy(this->requestMode, this->CurrentModeName(), sizeof(this->requestMode));
	const u32 gen = ++this->generation;
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

void KZLeadService::ArmProgressPath()
{
	if (!this->progress || this->loading || this->pending)
	{
		return;
	}
	if (this->armCooldown > 0)
	{
		// Пауза после отказа или после отброшенного протухшего пути: без неё мигающий ключ
		// (игрок топчется на границе двух курсов) гнал бы резолв и докачку каждые 32 тика.
		return;
	}
	// Защёлка отказа сверяется ПО КЛЮЧУ: отказ на main ничего не говорит о бонусе, куда игрок
	// может уйти через минуту (иначе процент умирал бы до конца карты).
	if (this->failedCourse == this->CurrentCourseKey() && KZ_STREQI(this->failedMode, this->CurrentModeName()))
	{
		if (this->failRetriesLeft <= 0)
		{
			return;
		}
		// Повторы под тем же ключом (возможная сетевая рябь) — расходуем их здесь: исчерпав,
		// эта же проверка станет глухой защёлкой до смены ключа или карты.
		this->failRetriesLeft--;
	}
	// AWR — тот же вид записи, что у `!lead` по умолчанию: процент считается по ТОМУ ЖЕ
	// маршруту, который показывает луч. Молча (fromPlayer=false): элемент худа в чат не пишет.
	this->RequestPath(CybReplayDownload::Kind::AWR, false);
}

void KZLeadService::SetProgressWanted(bool wanted)
{
	if (this->progress == wanted)
	{
		return;
	}
	this->progress = wanted;
	if (wanted)
	{
		// Саму загрузку начинает дросселированная ветка OnPhysicsSimulatePost (≤0.5 с): там у
		// игрока уже есть пешка и курс, а этот метод зовётся и с загрузки префов (коннект),
		// где курса ещё нет — запрос ушёл бы под чужой ключ. Здесь только снимаем защёлку:
		// включение элемента руками — такой же явный повод сходить в сеть, как `!lead`.
		this->failedCourse = -1;
		this->failedMode[0] = '\0';
		this->failRetriesLeft = 0;
		this->armCooldown = 0;
		return;
	}
	// Процент выключили: своих сущностей у него нет, снимать нечего. Путь держит только луч —
	// без него он больше никому не нужен.
	this->progressPct = -1;
	if (!this->beam)
	{
		this->ReleasePath();
	}
}

void KZLeadService::BuildCumulativeLengths()
{
	// Почему по НАКОПЛЕННОЙ ДЛИНЕ, а не по номеру вершины: после упрощения RDP вершины стоят
	// неравномерно (на прямой — две на сотни юнитов, в петле — десятки на метр), и «вершина
	// 500 из 1000» серединой маршрута не является. По времени тоже нельзя: это был бы процент
	// времени ЧУЖОГО рана, а не доля пройденного игроком пути.
	// Один проход на загрузку (главный поток, PollPending) — в тике не считается ничего.
	const size_t count = this->path.size();
	this->cumLen.assign(count, 0.0f);
	f32 sum = 0.0f;
	for (size_t i = 1; i < count; i++)
	{
		sum += (this->path[i].pos - this->path[i - 1].pos).Length();
		this->cumLen[i] = sum;
	}
	this->totalLen = sum;
}

void KZLeadService::UpdateProgress()
{
	if (this->totalLen <= 0.0f || this->nearest >= this->cumLen.size())
	{
		this->progressPct = -1;
		return;
	}
	const i32 pct = (i32)(this->cumLen[this->nearest] / this->totalLen * 100.0f + 0.5f);
	this->progressPct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
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
		// reason=no_file, а НЕ no_record: пустой путь означает ровно «файла не дали», и записи
		// может не быть (404), а может отказать сеть или api — RequestFile нам этого не
		// сообщает. Настоящая причина есть в ЕГО логе (`file resolve HTTP %u` /
		// `file resolve network error`; 404 он не логирует намеренно), а врать в своей строке
		// нельзя. DEBUG, а не WARN: «на этом курсе нет AWR-записи» — ожидаемое состояние
		// карты, а не отказ нашего кода; для молчаливой загрузки это единственный след.
		KZ_LOG_DEBUG(LogChannel::Replays, "[lead] lead_load_failed reason=no_file course=%i mode=%s steam_id=%llu\n", this->requestCourse,
					 this->requestMode, (unsigned long long)this->player->GetSteamId64());
		this->OnLoadFailed(/* retryable */ true);
		return;
	}
	this->pending = std::make_shared<PendingLoad>();
	this->pending->generation = gen;
	// Снимок допуска — ЗДЕСЬ, на главном потоке: дальше значение уедет в рабочий поток.
	this->pending->rdpTolerance = cyb_lead_rdp.Get();
	this->pendingIsRebuild = false;
	// Путь к файлу оставляем себе: из него же пойдёт пересборка под новый допуск, без сети и
	// без повторного резолва (а значит и без риска подхватить файл ДРУГОГО курса).
	this->pathFile = filePath;
	// Дальше гейтом служит сам pending — сетевая фаза кончилась.
	this->loading = false;
	std::thread(BuildPathWorker, std::move(filePath), this->pending).detach();
}

void KZLeadService::OnLoadFailed(bool retryable)
{
	// Защёлка — ВСЕГДА, кто бы ни просил, и ПО КЛЮЧУ ЗАПРОСА (курс+режим): под ним пути нет, и
	// проверка раз в 32 тика иначе ходила бы в сеть до конца карты; на другом курсе она
	// снимется сама (см. ArmProgressPath), а успешная загрузка снимает её совсем (OnPathLoaded).
	if (!(this->failedCourse == this->requestCourse && KZ_STREQI(this->failedMode, this->requestMode)))
	{
		this->failedCourse = this->requestCourse;
		V_strncpy(this->failedMode, this->requestMode, sizeof(this->failedMode));
		// Пустой путь приходит и на «записи нет», и на сетевую ошибку, а различить их нечем
		// (см. reason=no_file в OnFileReady) — такому отказу даём KZ_LEAD_FAIL_RETRIES повторов
		// с паузой: сетевая рябь лечится сама, а курс без AWR-записи защёлкнётся, исчерпав их
		// (счётчик расходует ArmProgressPath).
		this->failRetriesLeft = KZ_LEAD_FAIL_RETRIES;
	}
	if (!retryable)
	{
		// Отказ РАЗБОРА: файл уже скачан, и второй заход прочитает тот же файл тем же кодом с
		// тем же исходом — это три докачки и три потока разбора впустую. Защёлкиваем сразу
		// (в т.ч. поверх ретраев, начисленных прошлым сетевым отказом под этим же ключом).
		this->failRetriesLeft = 0;
	}
	this->armCooldown = KZ_LEAD_ARM_COOLDOWN_CYCLES;
	if (!this->beamOnLoad)
	{
		// Молчаливая загрузка под элемент худа: ни строки в чат (решение дизайна — элемент
		// просто остаётся скрытым, без прочерка и без спама). Причина — в логе вызывающего.
		return;
	}
	this->beamOnLoad = false;
	this->player->languageService->PrintChat(true, false, "Lead - No Replay");
}

void KZLeadService::PollPending()
{
	if (!this->pending->done)
	{
		return;
	}
	std::shared_ptr<PendingLoad> load = this->pending;
	this->pending.reset();
	const bool isRebuild = this->pendingIsRebuild;
	this->pendingIsRebuild = false;
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
		if (isRebuild)
		{
			// Пересборка не сошлась (файл успели удалить из кэша, битый разбор) — СТАРЫЙ путь
			// остаётся жить как есть. Защёлку отказа здесь ставить нельзя: под этим ключом путь
			// ЕСТЬ, и она погасила бы процент на ровном месте; в чат тоже не пишем — игрок
			// ничего не просил, он правил конвар.
			KZ_LOG_WARN(LogChannel::Replays, "[lead] path_rebuild_failed reason=%s steam_id=%llu\n", failReason,
						(unsigned long long)this->player->GetSteamId64());
			return;
		}
		KZ_LOG_WARN(LogChannel::Replays, "[lead] lead_load_failed reason=%s steam_id=%llu\n", failReason,
					(unsigned long long)this->player->GetSteamId64());
		// Файл скачан, но путь из него не построился — причина в САМОМ файле (битый, обрезанный,
		// ран не сошёлся): повторять нечего, см. retryable у OnLoadFailed.
		this->OnLoadFailed(/* retryable */ false);
		return;
	}
	if (cutWarn && cutWarn[0])
	{
		KZ_LOG_WARN(LogChannel::Replays, "[lead] lead_load_failed reason=cut_failed detail=%s steam_id=%llu\n", cutWarn,
					(unsigned long long)this->player->GetSteamId64());
	}
	if (isRebuild)
	{
		this->OnPathRebuilt(std::move(loaded), load->rdpTolerance);
	}
	else
	{
		this->pathRdp = load->rdpTolerance;
		this->OnPathLoaded(std::move(loaded));
	}
	// Допуск могли сменить, пока шли докачка и разбор: снимок брался в OnFileReady, а колбэк
	// конвара в тот момент видел ПУСТОЙ путь и пометить его не мог (ArmPathRebuild выходит
	// сразу). Без этой догоняющей проверки такой путь остался бы построенным по устаревшему
	// значению до конца карты. Разброс по слоту здесь не нужен: приземления загрузок и так
	// разнесены по тикам (у каждого своя сеть и свой поток), а лишней пересборки не будет —
	// StartPathRebuild сверяет значения ещё раз.
	if (!this->path.empty() && this->pathRdp != cyb_lead_rdp.Get())
	{
		this->rebuildPending = true;
	}
}

void KZLeadService::OnPathLoaded(std::vector<Vertex> &&newPath)
{
	// Молчаливая загрузка (её просил процент) под ключ, которого у игрока больше нет: пока шли
	// резолв, докачка и разбор, он ушёл на другой курс или сменил режим. Процент по такому
	// пути был бы долей ЧУЖОГО маршрута — путь не берём вовсе, и ветка path.empty() в тике
	// перезапросит его под новый ключ. Луч этой проверки не касается: он показывает маршрут,
	// который игрок попросил ЯВНО, и смену курса переживал и до этой задачи.
	if (!this->beamOnLoad && (this->requestCourse != this->CurrentCourseKey() || !KZ_STREQI(this->requestMode, this->CurrentModeName())))
	{
		KZ_LOG_DEBUG(LogChannel::Replays, "[lead] path_dropped reason=key_changed course=%i mode=%s steam_id=%llu\n", this->requestCourse,
					 this->requestMode, (unsigned long long)this->player->GetSteamId64());
		// Пауза перед следующей попыткой: без неё мигающий ключ давал бы резолв и докачку
		// каждые 32 тика (путь приходит — ключ снова другой — путь снова в мусор).
		this->armCooldown = KZ_LEAD_ARM_COOLDOWN_CYCLES;
		// Файл отброшенного пути пересобирать нечего и незачем: живого пути за ним нет.
		this->pathFile.clear();
		this->pathRdp = -1.0f;
		return;
	}
	this->ClearSegments(false);
	this->path = std::move(newPath);
	// Кумулятивные длины — сразу и один раз: по ним считается процент (см. UpdateProgress).
	this->BuildCumulativeLengths();
	this->resync = true;
	this->nearest = 0;
	this->windowFrom = 0;
	this->windowTo = 0;
	this->progressPct = -1; // посчитается на первом же UpdateNearest
	// Окно строится на ближайшем же тике, а не через полсекунды.
	this->ticksSinceUpdate = KZ_LEAD_UPDATE_TICKS;
	// Ключ пути — ключ ЗАПРОСА, а не текущий: файл резолвился под него, и подписать вершины
	// сегодняшним курсом означало бы соврать про то, чей это маршрут (обе сверки — процента и
	// смены режима — читают именно эти два поля). Для луча это ещё и включает штатную реакцию
	// на смену режима посреди загрузки: путь чужого режима будет снят тиком с сообщением.
	this->pathCourse = this->requestCourse;
	V_strncpy(this->modeName, this->requestMode, sizeof(this->modeName));
	// УСПЕХ снимает пометку отказа: под этим ключом путь ЕСТЬ, и прошлые (сетевые) отказы о нём
	// больше ничего не говорят. Без этого исчерпанный счётчик оставался бы висеть на ключе, и
	// когда путь под ним понадобится заново — main → бонус → main, где на смене курса при
	// выключенном луче путь освобождается, — ArmProgressPath ушёл бы в глухую защёлку до смены
	// карты. Счётчик перезарядит сам OnLoadFailed: со снятой пометкой любой следующий отказ
	// попадает в его ветку «новый ключ».
	this->failedCourse = -1;
	this->failedMode[0] = '\0';
	this->failRetriesLeft = 0;
	if (!this->beamOnLoad)
	{
		// Путь просил элемент худа — луч не поднимаем и в чат не пишем.
		return;
	}
	this->beamOnLoad = false;
	this->beam = true;
	this->player->languageService->PrintChat(true, false, "Lead - Enabled", (int)this->path.size());
}

void KZLeadService::OnPathRebuilt(std::vector<Vertex> &&newPath, f32 builtRdp)
{
	if (newPath.size() < 2)
	{
		// Быть не должно (короткий путь приходит как failReason=path_too_short), но менять
		// живой путь на вырожденный нельзя ни при каких обстоятельствах. Допуск при этом
		// помечаем как применённый, хотя путь и остался прежним: иначе догоняющая проверка в
		// PollPending увидела бы расхождение и заказала ТОТ ЖЕ разбор с тем же исходом —
		// вечный цикл на 32 МБ каждые 32 тика.
		this->pathRdp = builtRdp;
		return;
	}
	// Ключ (pathCourse/modeName) НЕ трогаем: пересборка шла из ТОГО ЖЕ файла, под который
	// резолвился путь, значит курс и режим у вершин прежние. Ровно поэтому здесь не может
	// подхватиться чужой курс, даже если игрок сменил его, пока шёл разбор: ни резолва, ни
	// requestCourse/requestMode пересборка не касается вовсе.
	KZ_LOG_DEBUG(LogChannel::Replays, "[lead] path_rebuilt rdp=%.2f verts=%i course=%i mode=%s steam_id=%llu\n", builtRdp, (int)newPath.size(),
				 this->pathCourse, this->modeName, (unsigned long long)this->player->GetSteamId64());
	// Отрезки построены по СТАРЫМ вершинам — снимаем: индексы окна к новому пути не относятся.
	this->ClearSegments(false);
	this->path = std::move(newPath);
	this->BuildCumulativeLengths();
	this->pathRdp = builtRdp;
	this->resync = true;
	this->nearest = 0;
	this->windowFrom = 0;
	this->windowTo = 0;
	// Процент НЕ гасим и не пересчитываем здесь: до готовности нового пути он всё это время
	// шёл по старому (тот же маршрут, другая густота вершин), а новое значение посчитает
	// UpdateProgress ПОСЛЕ UpdateNearest — на этом же тике, см. строку ниже. Прочерка в худе
	// не мигнёт, а мусорного числа (nearest == 0 при живом маршруте) никто не увидит.
	this->ticksSinceUpdate = KZ_LEAD_UPDATE_TICKS;
	// Луч не включаем и не гасим: его состояние пересборкой не меняется, а в чат не пишем —
	// игрок ничего не набирал.
}

void KZLeadService::OnPhysicsSimulatePost()
{
	if (this->pending)
	{
		this->PollPending();
	}
	// Путь нужен, если включён ЛУЧ или процент в худе — оба потребителя ведут одну и ту же
	// ближайшую вершину, поэтому и дросселирование у них общее.
	if (!this->beam && !this->progress)
	{
		return;
	}
	if (++this->ticksSinceUpdate < KZ_LEAD_UPDATE_TICKS)
	{
		return;
	}
	this->ticksSinceUpdate = 0;
	if (this->armCooldown > 0)
	{
		// Пауза молчаливых загрузок считается в проходах ЭТОЙ ветки (один проход = 32 тика):
		// отдельного таймера ради неё заводить незачем.
		this->armCooldown--;
	}

	// Путь режим-зависим: в другом режиме он врал бы и про траекторию, и про процент.
	// Отдельного хука на смену режима у игрока нет, поэтому сверяем имя здесь — раз в 32 тика
	// и без аллокаций (GetModeName отдаёт литерал сервиса, а не собранную строку). Только при
	// живом пути: без него modeName пуст, и сверка срабатывала бы вечно.
	if (!this->path.empty())
	{
		const char *mode = this->player->modeService ? this->player->modeService->GetModeName() : "";
		if (!mode || !KZ_STREQI(this->modeName, mode))
		{
			if (this->beam)
			{
				// Луч гасим с сообщением: его включал сам игрок.
				this->Disable("mode_changed");
			}
			// Путь чужого режима не годится НИ ЛУЧУ, НИ ПРОЦЕНТУ, поэтому освобождаем его
			// всегда: Disable выше оставил бы его жить ради включённого процента, и процент
			// считался бы по маршруту прошлого режима. Под новый режим путь перезапросит
			// ветка path.empty() ниже (на следующем проходе).
			this->ReleasePath();
			return;
		}
	}

	if (this->rebuildPending)
	{
		// Пересборка пути под новый допуск (правка cyb_lead_rdp). Стоит ЗДЕСЬ, до проверки
		// пешки: путь живёт и у мёртвого/спектатора, и его густота обязана догнать конвар.
		// Чтение файла и разбор уходят на рабочий поток — в тике только старт.
		if (this->rebuildDelay > 0)
		{
			// Разброс по слоту, в проходах этой же ветки (см. ArmPathRebuild).
			this->rebuildDelay--;
		}
		else
		{
			this->StartPathRebuild();
		}
	}

	if (!this->player->GetPlayerPawn())
	{
		// Нет пешки (мёртв, спектейт) — окно и процент не двигаем, но и не снимаем: вернётся сам.
		return;
	}

	if (this->path.empty())
	{
		// Путь нужен проценту, но его нет (первый заход, смена карты/режима/курса) — здесь
		// только СТАРТ запроса, чтение файла живёт на рабочем потоке, как и у `!lead`.
		this->ArmProgressPath();
		return;
	}

	Vector origin = vec3_invalid;
	this->player->GetOrigin(&origin);
	if (origin == vec3_invalid)
	{
		return;
	}
	// Ближайшая вершина — общий вход для обоих потребителей.
	this->UpdateNearest(origin);

	// Путь КУРС-зависим — ключ тот же, которым резолвился реплей (см. OnPathLoaded). Проверка
	// касается только ПРОЦЕНТА: луч смену курса переживал и до этой задачи, и менять поведение
	// `!lead` в ней не просили.
	if (KZ::course::GetCyberCourseNumber(this->player->timerService->GetCourse()) == this->pathCourse)
	{
		// Процент считается сразу готовым числом (худ его только читает).
		this->UpdateProgress();
	}
	else
	{
		// Игрок ушёл на другой курс: доля пройденного по ЧУЖОМУ маршруту смысла не имеет —
		// процент гасим, элемент худа скроется сам (см. GetProgressPercent).
		this->progressPct = -1;
		if (!this->beam)
		{
			// Луч путь не держит — освобождаем и перезапрашиваем под новый курс. Защёлку
			// трогать не нужно: она ключевана, и отказ прошлого курса новому не помеха.
			this->ReleasePath();
			return;
		}
		// Луч включён: он и остаётся хозяином пути (игрок просил ИМЕННО этот маршрут), а
		// процент вернётся, когда игрок уйдёт с луча (`!lead off`) — тогда путь перезапросится
		// под текущий курс.
	}

	if (this->beam)
	{
		// Сущности отрезков создаются ТОЛЬКО под включённый луч: у процента их нет вовсе.
		this->UpdateWindow();
	}
}

void KZLeadService::UpdateNearest(const Vector &origin)
{
	const u32 count = (u32)this->path.size();
	u32 bestIdx = this->nearest < count ? this->nearest : 0;
	f32 best = FLT_MAX;

	if (!this->resync)
	{
		// Вперёд от прошлой ближайшей: игрок идёт по маршруту, полный скан пути (десятки
		// тысяч вершин) каждые полсекунды не нужен.
		//
		// Границы у двух режимов РАЗНЫЕ, и это осознанно. При включённом луче она ПРЕЖНЯЯ —
		// конец окна луча: расширять её нельзя, потому что на самопересекающихся маршрутах
		// (спираль, возврат в ту же комнату) минимум расстояния может оказаться на ПОЗДНЕМ
		// пересечении, и луч перескочил бы вперёд по маршруту. Поиск ближайшей вершины для
		// `!lead` эта задача менять не должна вовсе.
		//
		// Без луча (включён только процент) окна не существует, и прежняя граница выродилась
		// бы в одну вершину — ближайшая замирала бы до порога полного ресинка. Своя граница
		// здесь по НАКОПЛЕННОЙ ДЛИНЕ, а не по числу вершин: за 32 тика игрок не уходит по
		// маршруту дальше KZ_LEAD_SCAN_UNITS юнитов, а вершин на этой длине бывает и две (на
		// прямой), и сотни (в петле). Вершинный потолок — только страховка от сверхплотной
		// петли, чтобы стоимость скана оставалась ограниченной.
		// Ниже — ТОЛЬКО bestIdx, а не this->nearest: строкой выше nearest трактуется как
		// потенциально невалидный (при nearest >= count bestIdx обнуляется), и одна из двух
		// трактовок в одной функции — прямой путь к чтению cumLen за концом вектора.
		// Исключение — ветка луча: там сохранена ПРЕЖНЯЯ формула дословно (её нельзя менять),
		// а индексами cumLen она не пользуется вовсе, и цикл всё равно ограничен `i < count`.
		u32 to = bestIdx;
		if (this->beam)
		{
			to = this->windowTo > this->nearest ? this->windowTo : this->nearest;
		}
		else if (this->cumLen.size() == (size_t)count)
		{
			const f32 limit = this->cumLen[bestIdx] + KZ_LEAD_SCAN_UNITS;
			while (to + 1 < count && this->cumLen[to + 1] <= limit && to - bestIdx < KZ_LEAD_SCAN_MAX_VERTS)
			{
				to++;
			}
		}
		else
		{
			// Кумулятивных длин нет (быть не должно: их строит OnPathLoaded вместе с путём) —
			// не читаем их вовсе, а падаем на вершинный потолок.
			to = bestIdx + KZ_LEAD_SCAN_MAX_VERTS;
		}
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
	// Ближайшая вершина уже пересчитана вызывающим (OnPhysicsSimulatePost): её ведёт и
	// процент в худе, у которого этой функции нет вовсе.
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

	// Потолок: живой конвар поверх cfg. Конвар -1 (дефолт) означает «как в cfg», то есть
	// значение оператора не подменяется кодом; всё прочее из конвара побеждает, потому что
	// cybLeadMaxSegments правится только перезагрузкой плагина (см. шапку блока конваров).
	// Не `override`: слово контекстно-ключевое, и держать его именем переменной — напрашиваться
	// на путаницу при чтении.
	const i32 capOverride = cyb_lead_max_segments.Get();
	i64 configured = capOverride >= 1 ? (i64)capOverride : KZOptionService::GetOptionInt("cybLeadMaxSegments", 384);
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

// Ручной рычаг к колбэкам конваров: перерисовать отрезки всем, у кого луч включён. Нужен, если
// значение выставили не конваром (например, правкой cfg + перезагрузкой) или колбэк не
// сработал. Серверная команда, игрокам не отдаём — канон форка (kz_invisible.cpp:453).
CON_COMMAND_F(kz_lead_refresh, "Rebuild !lead beam segments for everyone (applies cyb_lead_* changes without reloading the path).", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()))
	{
		KZ_LOG_WARN(LogChannel::Replays, "[lead] refresh_denied reason=not_server slot=%d\n", context.GetPlayerSlot().Get());
		return;
	}
	KZLeadService::RefreshAllSegments("command");
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
