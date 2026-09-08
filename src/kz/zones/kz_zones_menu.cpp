/*
	Игровой редактор зон и курсов (LAM-20): меню cs2menus + постановка точек по прицелу.

	Вход — !zones (алиас !zone menu), право game.kz_zones_edit проверяется ДО создания меню.
	Точка зоны — ровно место попадания луча по прицелу (без прилипания к полу); пол зоны —
	нижняя из двух точек, высота — регулируемая строка. Каждый шаг перерисовывает превью.

	Правки живут в per-slot состоянии; в api уходит только «Сохранить»/«Подтвердить»:
	start/end — штатной заменой (POST того же типа и курса, api вернёт replaced[]),
	бустер — DELETE старого + POST нового одним действием.

	Навигация: скелет корня (постановка/зоны/курсы) связан AddSubMenu — там работает бинд R
	(назад). Экраны глубже зависят от выбранной строки и создаются по требованию через
	DisplayMenu — R там не действует, выход — F. Действия меню НЕ закрывают (решение
	пользователя): отмена/сохранение/удаление возвращают в уместный экран, а когда позже
	приходит ответ api, открытые обзорные экраны перестраивает RefreshOpenEditorMenus
	(зовётся из kz_zones.cpp после каждого применения набора).
*/

#include "kz_zones.h"
#include "kz/measure/kz_measure.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "utils/utils.h"
#include "utils/http.h"

#include "tier1/keyvalues3.h"

#include <vendor/mm-cs2menus/src/public/ics2menus.h>

#include <functional>
#include <string>
#include <math.h>   // fabsf — проверка мира
#include <stdlib.h> // atoi — номер курса из info-тега

extern ICS2Menus *g_pMenus;
// Движок умеет E-захват adjustable-строк (интерфейс 006). Со старым движком редактор живёт
// по-старому: стрелки всегда, A/D сразу, подсказка «(A/D …)» в тексте строки.
extern bool g_menusHasAdjustCapture;

// Все хелперы api-запросов — общие с kz_zones_editor.cpp (объявлены в kz_zones.h).
using KZ::zones::ApiBaseUrl;
using KZ::zones::AuthorizeRequest;
using KZ::zones::ParseApiReason;
using KZ::zones::PlayerBySlotIfSame;

// Геометрия панели постановки/правки. Минимумы — те же пороги, что у api (checkZoneBox),
// иначе «Подтвердить» гонял бы заведомо отказной запрос: по горизонтали KZ_ZONE_MIN_SIZE (8),
// по высоте — 1 (решение пользователя: касание считается пересечением хитбоксов и работает
// на плоском «коврике», запрещён только вырожденный ноль). Контракт: KZ_ZONE_MIN_HEIGHT.
#define KZ_ZONEMENU_HEIGHT_STEP 8.0f
#define KZ_ZONEMENU_HEIGHT_MIN  1.0f
#define KZ_ZONEMENU_HEIGHT_MAX  2048.0f
// Множитель бустера — границы контракта (params.jumpFactor: >0, <=10).
#define KZ_ZONEMENU_FACTOR_STEP 0.05f
#define KZ_ZONEMENU_FACTOR_MIN  0.05f
#define KZ_ZONEMENU_FACTOR_MAX  10.0f
// Превью редактора живёт столько же, сколько у командного редактора.
#define KZ_ZONEMENU_PREVIEW_SECONDS 15.0f
// Раздутие превью: у стоящей зоны поверх лежит постоянный контур/её собственные рёбра,
// точно совпадающие линии мерцали бы z-fighting'ом (то же, что у !zone show).
#define KZ_ZONEMENU_PREVIEW_INFLATE 0.5f
// Маркер одиночной точки, пока второй нет: кубик по полюнита хитбокса.
#define KZ_ZONEMENU_POINT_MARKER 4.0f

// Ступени шага координатных строк (A/D по строке «Шаг» листает по ним).
static_global constexpr f32 s_stepPresets[] = {1.0f, 8.0f, 32.0f, 128.0f};

// Состояние редактора на слот. Привязано к карте (map) — каждый колбэк сверяет её с текущей:
// меню могло висеть открытым через смену карты, и координаты прошлой карты применять нельзя.
struct ZoneMenuState
{
	std::string map;

	// Что ставим/правим.
	KzCyberZoneType type {};
	f32 jumpFactor = 1.0f;
	char courseDescriptor[128] {};
	char courseId[40] {};
	char courseLabel[128] {};

	// Точки и высота.
	bool hasA {}, hasB {};
	Vector a {}, b {};
	f32 height = KZ_ZONE_EDITOR_HEIGHT;
	f32 step = 8.0f;

	// Правка существующей зоны ("" = постановка новой).
	char editZoneId[40] {};

	// Индексы строк панели постановки (у бустера есть строка множителя, индексы плавают).
	i32 idxA {}, idxB {}, idxHeight {}, idxFactor = -1, idxConfirm {};

	// Контекст управления курсом (экран действий курса).
	bool courseOwn {};
	bool courseDisabled {};
	i32 courseNumber {};

	// Фильтр открытого сейчас списка зон (пустой = все зоны карты) — чтобы наблюдатель
	// перестроил список с тем же фильтром, когда придёт ответ api.
	char lastListFilter[128] {};
	// Куда возвращаться после Сохранить/Удалить из правки зоны: фильтр списка, из которого
	// зашли (пустой = список всех зон карты).
	char returnFilter[128] {};

	void ResetEdit()
	{
		this->type = {};
		this->jumpFactor = 1.0f;
		this->courseDescriptor[0] = '\0';
		this->courseId[0] = '\0';
		this->courseLabel[0] = '\0';
		this->hasA = this->hasB = false;
		this->a = this->b = Vector(0, 0, 0);
		this->height = KZ_ZONE_EDITOR_HEIGHT;
		this->step = 8.0f;
		this->editZoneId[0] = '\0';
		this->idxA = this->idxB = this->idxHeight = this->idxConfirm = 0;
		this->idxFactor = -1;
		this->courseOwn = this->courseDisabled = false;
		this->courseNumber = 0;
		this->lastListFilter[0] = '\0';
		this->returnFilter[0] = '\0';
	}
};

// Хэндлы экранов на слот. Скелет (root/typeSel/zonesList/coursesList) пересоздаётся на каждое
// открытие !zones; остальные — по требованию, старый хэндл того же экрана гасится перед новым.
struct ZoneMenuHandles
{
	MenuHandle root {}, typeSel {}, zonesList {}, coursesList {};
	MenuHandle wizardCourseSel {}, createCourse {}, placement {}, courseAct {}, zoneEdit {}, pointA {}, pointB {}, courseZonesList {};
};

static_global ZoneMenuState s_zmState[MAXPLAYERS] = {};
static_global ZoneMenuHandles s_zmMenus[MAXPLAYERS] = {};

static_function void DestroyHandle(MenuHandle &handle)
{
	if (handle != kInvalidMenuHandle && g_pMenus)
	{
		g_pMenus->DestroyMenu(handle);
	}
	handle = kInvalidMenuHandle;
}

static_function void DestroySlotMenus(i32 slot)
{
	ZoneMenuHandles &h = s_zmMenus[slot];
	MenuHandle *all[] = {&h.root,      &h.typeSel,   &h.zonesList, &h.coursesList, &h.wizardCourseSel, &h.createCourse,
						 &h.placement, &h.courseAct, &h.zoneEdit,  &h.pointA,      &h.pointB,          &h.courseZonesList};
	for (MenuHandle *handle : all)
	{
		DestroyHandle(*handle);
	}
}

void KZ::zones::ResetEditorMenuState()
{
	for (i32 slot = 0; slot < MAXPLAYERS; slot++)
	{
		s_zmState[slot].ResetEdit();
		s_zmState[slot].map.clear();
	}
}

void KZ::zones::DestroyEditorMenus()
{
	for (i32 slot = 0; slot < MAXPLAYERS; slot++)
	{
		DestroySlotMenus(slot);
	}
	KZ::zones::ResetEditorMenuState();
}

// Игрок по слоту колбэка меню + сверка карты. nullptr = действие устарело (игрок ушёл или
// карта сменилась под открытым меню) — молча выходим, cs2menus меню и так закроет.
static_function KZPlayer *MenuPlayer(int slot)
{
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return nullptr;
	}
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
	if (!player || !player->zonesService)
	{
		return nullptr;
	}
	if (!KZ_STREQ(s_zmState[slot].map.c_str(), KZ::zones::CurrentMapName()))
	{
		return nullptr;
	}
	return player;
}

// Живой дескриптор курса карты по targetname (включая отключённые платформой).
static_function const KZCourseDescriptor *FindLiveCourse(const char *descriptor)
{
	if (!descriptor || !descriptor[0])
	{
		return nullptr;
	}
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		if (course && KZ_STREQI(course->entityTargetname, descriptor))
		{
			return course;
		}
	}
	return nullptr;
}

// Подпись курса для заголовков и строк: имя живого курса, иначе дескриптор из api.
static_function void CourseLabel(const char *descriptor, char *out, i32 outSize)
{
	const KZCourseDescriptor *live = FindLiveCourse(descriptor);
	V_snprintf(out, outSize, "%s", live && live->name[0] ? live->name : descriptor);
}

static_function const char *ZoneTypeRu(KzCyberZoneType type)
{
	switch (type)
	{
		case KZ_CYBER_ZONE_START:
			return "старт";
		case KZ_CYBER_ZONE_END:
			return "финиш";
		case KZ_CYBER_ZONE_MODIFIER:
			return "бустер";
		case KZ_CYBER_ZONE_STAGE:
			return "стейдж";
		case KZ_CYBER_ZONE_CHECKPOINT:
			return "чекпоинт";
		default:
			return "зона";
	}
}

// Дескриптор в JSON-строку. Управляющие символы = «отдавать нельзя» (false): контракт их
// отбивает, а класть их в тело — ломать разбор всего запроса.
static_function bool JsonEscape(const char *in, char *out, i32 outSize)
{
	i32 w = 0;
	for (const char *p = in; *p; p++)
	{
		if ((unsigned char)*p < 0x20)
		{
			return false;
		}
		if (*p == '"' || *p == '\\')
		{
			if (w + 2 >= outSize)
			{
				return false;
			}
			out[w++] = '\\';
		}
		if (w + 1 >= outSize)
		{
			return false;
		}
		out[w++] = *p;
	}
	out[w] = '\0';
	return true;
}

// Подписи регулируемых строк. С захватом (006) подсказка клавиш живёт в футере движка
// (E Настроить / A/D Настр.), и хвост «(A/D …)» в тексте только шумел бы — по решению
// пользователя незахваченная строка выглядит как обычный пункт. На старом движке хвост
// остаётся: там стрелки постоянные и другой подсказки про шаг нет.
static_function std::string HeightRowText(f32 height)
{
	char text[64];
	if (g_menusHasAdjustCapture)
	{
		V_snprintf(text, sizeof(text), "Высота: %.0f", height);
	}
	else
	{
		V_snprintf(text, sizeof(text), "Высота: %.0f (A/D ±%.0f)", height, KZ_ZONEMENU_HEIGHT_STEP);
	}
	return text;
}

static_function std::string FactorRowText(f32 factor)
{
	char text[64];
	if (g_menusHasAdjustCapture)
	{
		V_snprintf(text, sizeof(text), "Множитель: x%.2f", factor);
	}
	else
	{
		V_snprintf(text, sizeof(text), "Множитель: x%.2f (A/D)", factor);
	}
	return text;
}

static_function std::string StepRowText(f32 step)
{
	char text[32];
	if (g_menusHasAdjustCapture)
	{
		V_snprintf(text, sizeof(text), "Шаг: %.0f", step);
	}
	else
	{
		V_snprintf(text, sizeof(text), "Шаг: %.0f (A/D)", step);
	}
	return text;
}

// Включить E-захват для меню с регулируемыми строками, если движок умеет (006). У наших
// adjustable-строк нет действий на E — контракт SetAdjustCapture это и требует.
static_function void EnableAdjustCapture(MenuHandle menu)
{
	if (g_menusHasAdjustCapture)
	{
		g_pMenus->SetAdjustCapture(menu, true);
	}
}

// --- Геометрия из состояния ---

// Бокс из двух точек и высоты: пол — нижняя из точек, XY — по крайним координатам.
static_function void StateBox(const ZoneMenuState &state, Vector &mins, Vector &maxs)
{
	const f32 floorZ = MIN(state.a.z, state.b.z);
	mins = Vector(MIN(state.a.x, state.b.x), MIN(state.a.y, state.b.y), floorZ);
	maxs = Vector(MAX(state.a.x, state.b.x), MAX(state.a.y, state.b.y), floorZ + state.height);
}

// Перерисовать превью по текущему состоянию. Одна точка — маркер-кубик, две — весь бокс.
static_function void RedrawPreview(KZPlayer *player, ZoneMenuState &state)
{
	const Color color = KZ::zones::ZoneColor(state.type);
	if (state.hasA && state.hasB)
	{
		Vector mins, maxs;
		StateBox(state, mins, maxs);
		mins -= Vector(KZ_ZONEMENU_PREVIEW_INFLATE, KZ_ZONEMENU_PREVIEW_INFLATE, KZ_ZONEMENU_PREVIEW_INFLATE);
		maxs += Vector(KZ_ZONEMENU_PREVIEW_INFLATE, KZ_ZONEMENU_PREVIEW_INFLATE, KZ_ZONEMENU_PREVIEW_INFLATE);
		player->zonesService->DrawBox(mins, maxs, color, KZ_ZONEMENU_PREVIEW_SECONDS);
		return;
	}
	const Vector *point = state.hasA ? &state.a : (state.hasB ? &state.b : nullptr);
	if (!point)
	{
		return;
	}
	const Vector half(KZ_ZONEMENU_POINT_MARKER, KZ_ZONEMENU_POINT_MARKER, KZ_ZONEMENU_POINT_MARKER);
	player->zonesService->DrawBox(*point - half, *point + half, color, KZ_ZONEMENU_PREVIEW_SECONDS);
}

// Проверки геометрии перед отправкой — те же пороги, что у контракта (см. BeginOrFinish).
static_function bool ValidateStateBox(KZPlayer *player, const ZoneMenuState &state, Vector &mins, Vector &maxs)
{
	if (!state.hasA || !state.hasB)
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} поставь обе точки по прицелу.");
		return false;
	}
	StateBox(state, mins, maxs);
	if (maxs.x - mins.x < KZ_ZONE_MIN_SIZE || maxs.y - mins.y < KZ_ZONE_MIN_SIZE)
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} слишком узко — минимум %.0f юнитов по каждой оси.", (f32)KZ_ZONE_MIN_SIZE);
		return false;
	}
	const f32 coords[6] = {mins.x, mins.y, mins.z, maxs.x, maxs.y, maxs.z};
	for (f32 c : coords)
	{
		if (fabsf(c) > KZ_ZONE_WORLD_LIMIT)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} координата вне мира.");
			return false;
		}
	}
	return true;
}

// --- HTTP: курсы ---

// Продолжение после того, как курс гарантированно заведён в api. courseId непустой.
using CourseReadyFn = std::function<void(KZPlayer *player, const std::string &courseId, const std::string &descriptor)>;

// POST /ingest/v1/kz/courses. kind: own (descriptor может быть пустым — api сгенерирует) или
// override (descriptor обязателен). После успеха — RefreshFromApi и continuation.
static_function void SendCourseCreate(KZPlayer *player, const char *kind, const char *descriptor, i32 courseNumber, CourseReadyFn then)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		return;
	}
	char escaped[256] = "";
	if (descriptor && descriptor[0] && !JsonEscape(descriptor, escaped, sizeof(escaped)))
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} имя курса содержит недопустимые символы.");
		return;
	}

	const u64 steamId = player->GetSteamId64(false);
	char body[512];
	if (escaped[0])
	{
		V_snprintf(body, sizeof(body), "{\"steamId64\":\"%llu\",\"map\":\"%s\",\"kind\":\"%s\",\"descriptor\":\"%s\",\"courseNumber\":%d}", steamId,
				   KZ::zones::CurrentMapName(), kind, escaped, courseNumber);
	}
	else
	{
		V_snprintf(body, sizeof(body), "{\"steamId64\":\"%llu\",\"map\":\"%s\",\"kind\":\"%s\",\"courseNumber\":%d}", steamId,
				   KZ::zones::CurrentMapName(), kind, courseNumber);
	}

	HTTP::Request req(HTTP::Method::POST, base + "/ingest/v1/kz/courses");
	AuthorizeRequest(req);
	req.SetHeader("Content-Type", "application/json");
	req.SetBody(body);

	const CPlayerSlot slot = player->GetPlayerSlot();
	const std::string submittedMap = KZ::zones::CurrentMapName();

	// clang-format off
	req.Send(
		[slot, steamId, submittedMap, then](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			std::optional<std::string> body = resp.Body();
			if (resp.status < 200 || resp.status >= 300)
			{
				std::string reason = ParseApiReason(body);
				if (reason.empty())
				{
					reason = "http_error";
				}
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_create_rejected steam_id=%llu map=%s reason=%s http=%u\n", steamId,
							submittedMap.c_str(), reason.c_str(), (unsigned)resp.status);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал в курсе: {darkred}%s{default}.", reason.c_str());
				}
				return;
			}
			if (!KZ_STREQ(submittedMap.c_str(), KZ::zones::CurrentMapName()))
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_create_stale steam_id=%llu map=%s reason=map_changed\n", steamId,
							submittedMap.c_str());
				return;
			}
			std::string courseId;
			std::string descriptor;
			if (body.has_value())
			{
				KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
				CUtlString error = "";
				LoadKV3FromJSON(&kv, &error, body->c_str(), "");
				if (error.IsEmpty())
				{
					KeyValues3 *course = kv.FindMember("course");
					KeyValues3 *id = course ? course->FindMember("id") : nullptr;
					KeyValues3 *desc = course ? course->FindMember("descriptor") : nullptr;
					courseId = id ? id->GetString("") : "";
					descriptor = desc ? desc->GetString("") : "";
				}
			}
			if (courseId.empty() || descriptor.empty())
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_create_rejected steam_id=%llu map=%s reason=bad_response\n", steamId,
							submittedMap.c_str());
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api ответил без id курса — операция не завершена.");
				}
				return;
			}
			// Локальное состояние — пересинхронизацией с api: точечное зеркалирование мутаций
			// разъехалось бы первым же пропуском.
			KZ::zones::RefreshFromApi("course_created");
			if (player && then)
			{
				then(player, courseId, descriptor);
			}
		},
		[slot, steamId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_create_rejected steam_id=%llu reason=network_error\n", steamId);
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, курс не заведён.");
			}
		});
	// clang-format on
}

// PATCH /ingest/v1/kz/courses/:id — «удалить»/«вернуть» курс флагом disabled.
static_function void SendCoursePatch(KZPlayer *player, const std::string &courseId, bool disabled)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		return;
	}
	const u64 steamId = player->GetSteamId64(false);
	char body[128];
	V_snprintf(body, sizeof(body), "{\"steamId64\":\"%llu\",\"disabled\":%s}", steamId, disabled ? "true" : "false");

	HTTP::Request req(HTTP::Method::PATCH, base + "/ingest/v1/kz/courses/" + courseId);
	AuthorizeRequest(req);
	req.SetHeader("Content-Type", "application/json");
	req.SetBody(body);

	const CPlayerSlot slot = player->GetPlayerSlot();

	// clang-format off
	req.Send(
		[slot, steamId, courseId, disabled](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (resp.status < 200 || resp.status >= 300)
			{
				std::string reason = ParseApiReason(resp.Body());
				if (reason.empty())
				{
					reason = "http_error";
				}
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_patch_rejected steam_id=%llu course=%s disabled=%d reason=%s http=%u\n", steamId,
							courseId.c_str(), disabled ? 1 : 0, reason.c_str(), (unsigned)resp.status);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал: {darkred}%s{default}.", reason.c_str());
				}
				return;
			}
			KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_patched steam_id=%llu course=%s disabled=%d\n", steamId, courseId.c_str(),
						disabled ? 1 : 0);
			KZ::zones::RefreshFromApi(disabled ? "course_disabled" : "course_enabled");
			if (player)
			{
				player->PrintChat(true, false, disabled ? "{grey}Зоны:{default} курс отключён на нашем сервере."
														: "{grey}Зоны:{default} курс включён обратно.");
			}
		},
		[slot, steamId, courseId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_patch_rejected steam_id=%llu course=%s reason=network_error\n", steamId,
						courseId.c_str());
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, курс не изменён.");
			}
		});
	// clang-format on
}

// DELETE /ingest/v1/kz/courses/:id — полное удаление нашего курса вместе с его зонами.
// 409 zone_moved — гонка, один повтор (та же семантика, что у удаления зоны).
static_function void SendCourseDelete(CPlayerSlot slot, u64 steamId, std::string courseId, i32 attempt)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
		if (player)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		}
		return;
	}
	HTTP::Request req(HTTP::Method::DELETE_, base + "/ingest/v1/kz/courses/" + courseId);
	AuthorizeRequest(req);
	char steamIdStr[32];
	V_snprintf(steamIdStr, sizeof(steamIdStr), "%llu", steamId);
	req.SetQuery("steamId64", steamIdStr);

	// clang-format off
	req.Send(
		[slot, steamId, courseId, attempt](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			std::optional<std::string> body = resp.Body();
			if (resp.status == 409 && ParseApiReason(body) == "zone_moved" && attempt < 2)
			{
				KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_delete_retry steam_id=%llu course=%s attempt=%d reason=zone_moved\n", steamId,
							courseId.c_str(), attempt);
				SendCourseDelete(slot, steamId, courseId, attempt + 1);
				return;
			}
			if (resp.status < 200 || resp.status >= 300)
			{
				std::string reason = ParseApiReason(body);
				if (reason.empty())
				{
					reason = "http_error";
				}
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_delete_rejected steam_id=%llu course=%s reason=%s http=%u attempt=%d\n", steamId,
							courseId.c_str(), reason.c_str(), (unsigned)resp.status, attempt);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал в удалении курса: {darkred}%s{default}.", reason.c_str());
				}
				return;
			}
			KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] course_deleted steam_id=%llu course=%s\n", steamId, courseId.c_str());
			KZ::zones::RefreshFromApi("course_deleted");
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} курс удалён вместе с его зонами.");
			}
		},
		[slot, steamId, courseId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] course_delete_rejected steam_id=%llu course=%s reason=network_error\n", steamId,
						courseId.c_str());
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, курс не удалён.");
			}
		});
	// clang-format on
}

// Курс гарантированно заведён в api: запись с id уже есть — продолжаем сразу; родной ещё не
// заведён — регистрируем override (идемпотентно по (карта, дескриптор)) и продолжаем из ответа.
static_function void EnsureCourseRegistered(KZPlayer *player, const char *descriptor, i32 courseNumber, CourseReadyFn then)
{
	const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(descriptor);
	if (record && record->id[0] != '\0')
	{
		then(player, record->id, record->descriptor);
		return;
	}
	SendCourseCreate(player, "override", descriptor, courseNumber, then);
}

// --- HTTP: замена бустера (DELETE старого + POST нового одним действием) ---

// submittedMap — карта на момент ПЕРВОГО вызова, протаскивается через рекурсию повтора 409:
// перезахват CurrentMapName() внутри повтора обнулял бы сверку «карта сменилась, пока летели
// запросы» (map-guard ниже стоит ПОСЛЕ ветки повтора, и повтор с новой картой считал бы её той же).
static_function void DeleteZoneThenSubmit(CPlayerSlot slot, u64 steamId, std::string oldZoneId, KzCyberZone newZone, std::string courseId,
										  i32 attempt, std::string submittedMap)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
		if (player)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		}
		return;
	}
	char url[512];
	V_snprintf(url, sizeof(url), "%s/ingest/v1/kz/zones/%s", base.c_str(), oldZoneId.c_str());
	HTTP::Request req(HTTP::Method::DELETE_, url);
	AuthorizeRequest(req);
	char steamIdStr[32];
	V_snprintf(steamIdStr, sizeof(steamIdStr), "%llu", steamId);
	req.SetQuery("steamId64", steamIdStr);

	// Пока DELETE летит, карта может смениться, а слот и steam_id останутся теми же —
	// PlayerBySlotIfSame это не поймает. Без сверки с submittedMap (карта первого вызова)
	// SubmitZone ниже собрал бы тело уже с НОВОЙ картой и координатами старой.

	// clang-format off
	req.Send(
		[slot, steamId, oldZoneId, newZone, courseId, attempt, submittedMap](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			std::optional<std::string> body = resp.Body();
			if (resp.status == 409 && ParseApiReason(body) == "zone_moved" && attempt < 2)
			{
				KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zone_replace_retry steam_id=%llu id=%s attempt=%d reason=zone_moved\n", steamId,
							oldZoneId.c_str(), attempt);
				DeleteZoneThenSubmit(slot, steamId, oldZoneId, newZone, courseId, attempt + 1, submittedMap);
				return;
			}
			if (resp.status < 200 || resp.status >= 300)
			{
				std::string reason = ParseApiReason(body);
				if (reason.empty())
				{
					reason = "http_error";
				}
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_replace_rejected steam_id=%llu id=%s stage=delete reason=%s http=%u\n", steamId,
							oldZoneId.c_str(), reason.c_str(), (unsigned)resp.status);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал в удалении старого бустера: {darkred}%s{default}. Зона не изменена.",
									  reason.c_str());
				}
				return;
			}
			// Карта сменилась, пока DELETE летел: локальный набор уже от новой карты, а POST ниже
			// уехал бы с именем новой карты и координатами старой. В api старый бустер удалён —
			// это честно логируем как оборванную замену с данными для повтора.
			if (!KZ_STREQ(submittedMap.c_str(), KZ::zones::CurrentMapName()))
			{
				KZ_LOG_WARN(LogChannel::MappingAPI,
							"[cyb] zone_replace_orphaned steam_id=%llu old_id=%s map=%s reason=map_changed mins=(%.0f %.0f %.0f) maxs=(%.0f %.0f %.0f) factor=%.2f\n",
							steamId, oldZoneId.c_str(), submittedMap.c_str(), newZone.mins.x, newZone.mins.y, newZone.mins.z, newZone.maxs.x,
							newZone.maxs.y, newZone.maxs.z, newZone.jumpFactor);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} карта сменилась — старый бустер удалён, новый НЕ поставлен.");
				}
				return;
			}
			i32 revision = 0;
			if (body.has_value())
			{
				KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
				CUtlString error = "";
				LoadKV3FromJSON(&kv, &error, body->c_str(), "");
				KeyValues3 *rev = error.IsEmpty() ? kv.FindMember("revision") : nullptr;
				if (rev)
				{
					revision = (i32)rev->GetDouble(0.0);
				}
			}
			KZ::zones::RemoveById(oldZoneId.c_str(), revision);
			// Старый уже удалён — если постановка нового не подтвердится (SubmitZone напишет
			// отказ api), у игрока должны остаться данные для повтора. Честно говорим заранее,
			// а не делаем вид, что умеем откатывать.
			if (!player || !player->zonesService)
			{
				KZ_LOG_WARN(LogChannel::MappingAPI,
							"[cyb] zone_replace_orphaned steam_id=%llu old_id=%s reason=author_left mins=(%.0f %.0f %.0f) maxs=(%.0f %.0f %.0f) factor=%.2f\n",
							steamId, oldZoneId.c_str(), newZone.mins.x, newZone.mins.y, newZone.mins.z, newZone.maxs.x, newZone.maxs.y, newZone.maxs.z,
							newZone.jumpFactor);
				return;
			}
			player->PrintChat(true, false,
							  "{grey}Зоны:{default} старый бустер удалён, ставлю новый. Если не подтвердится — повтори: (%.0f %.0f %.0f)-(%.0f %.0f %.0f) x%.2f",
							  newZone.mins.x, newZone.mins.y, newZone.mins.z, newZone.maxs.x, newZone.maxs.y, newZone.maxs.z, newZone.jumpFactor);
			player->zonesService->SubmitZone(newZone, courseId.empty() ? nullptr : courseId.c_str());
		},
		[slot, steamId, oldZoneId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_replace_rejected steam_id=%llu id=%s stage=delete reason=network_error\n", steamId,
						oldZoneId.c_str());
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, бустер не изменён.");
			}
		});
	// clang-format on
}

// --- Экраны ---

// Какой экран скелета показать после перестройки (см. OpenSkeleton).
enum class ZmScreen
{
	Root,
	ZonesList,
	CoursesList,
};

static_function void OpenSkeleton(KZPlayer *player, ZmScreen show);
static_function void OpenPlacement(KZPlayer *player);
static_function void OpenZoneEdit(KZPlayer *player, const KzCyberZone &zone);
static_function void OpenZonesList(KZPlayer *player, const char *filterDescriptor);
static_function void OpenWizardCourseSelect(KZPlayer *player);
static_function void OpenCreateCourse(KZPlayer *player, bool fromWizard);
static_function void OpenCourseActions(KZPlayer *player);
static_function bool FillCourseContext(ZoneMenuState &state, const char *descriptor);

// После Сохранить/Удалить из правки зоны — назад в список, из которого зашли: курсовый
// (фильтр запомнен в returnFilter) или общий список зон карты.
static_function void ReturnToZonesList(KZPlayer *player)
{
	const i32 slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return;
	}
	// Фильтр копируем заранее: ветка OpenSkeleton чистит состояние слота (ResetEdit).
	char filter[128];
	V_snprintf(filter, sizeof(filter), "%s", s_zmState[slot].returnFilter);
	if (filter[0])
	{
		OpenZonesList(player, filter);
	}
	else
	{
		OpenSkeleton(player, ZmScreen::ZonesList);
	}
}

// Свободный номер бонуса: наименьший из 1..99, не занятый ни живыми курсами карты, ни
// записями платформы. -1 = свободных нет.
static_function i32 NextFreeBonusNumber()
{
	bool used[100] = {};
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		const i32 n = KZ::course::GetCyberCourseNumber(course);
		if (n >= 0 && n <= 99)
		{
			used[n] = true;
		}
	}
	for (const KzCyberCourse &record : KZ::zones::Courses())
	{
		if (record.courseNumber >= 0 && record.courseNumber <= 99)
		{
			used[record.courseNumber] = true;
		}
	}
	for (i32 n = 1; n <= 99; n++)
	{
		if (!used[n])
		{
			return n;
		}
	}
	return -1;
}

static_function bool MainCourseExists()
{
	// Любой живой курс карты означает существующий cyber-номер 0 (главный определяется по
	// полному списку). Плюс записи платформы: own-курс с номером 0 мог ещё не ожить.
	if (KZ::course::GetCourseDescriptorTotal() > 0)
	{
		return true;
	}
	for (const KzCyberCourse &record : KZ::zones::Courses())
	{
		if (record.courseNumber == 0)
		{
			return true;
		}
	}
	return false;
}

// --- Панель постановки/правки точек ---

static_function std::string PointRowText(const char *label, bool has, const Vector &point)
{
	char text[96];
	if (has)
	{
		V_snprintf(text, sizeof(text), "%s: %.0f %.0f %.0f (E — по прицелу)", label, point.x, point.y, point.z);
	}
	else
	{
		V_snprintf(text, sizeof(text), "%s: по прицелу (E)", label);
	}
	return text;
}

// Точка по прицелу игрока. false = луч ни во что не упёрся (смотрит в небо).
static_function bool AimPoint(KZPlayer *player, Vector &out)
{
	if (!player->measureService)
	{
		return false;
	}
	KZMeasureService::MeasurePos pos = player->measureService->GetLookAtPos();
	if (!pos.IsValid())
	{
		return false;
	}
	out = pos.origin;
	return true;
}

static_function void OnPlacementSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}

	if (KZ_STREQ(tag, "cancel"))
	{
		player->zonesService->ClearPreview();
		// Меню не закрываем (решение пользователя): постановка отменена — назад в корень.
		OpenSkeleton(player, ZmScreen::Root);
		return;
	}
	if (KZ_STREQ(tag, "a") || KZ_STREQ(tag, "b"))
	{
		Vector point;
		if (!AimPoint(player, point))
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} прицел не упирается в поверхность.");
			return;
		}
		const bool isA = KZ_STREQ(tag, "a");
		(isA ? state.a : state.b) = point;
		(isA ? state.hasA : state.hasB) = true;
		g_pMenus->SetItemText(menu, isA ? state.idxA : state.idxB, PointRowText(isA ? "Точка A" : "Точка B", true, point).c_str());
		if (state.hasA && state.hasB)
		{
			g_pMenus->SetItemDisabled(menu, state.idxConfirm, false);
		}
		RedrawPreview(player, state);
		return;
	}
	if (KZ_STREQ(tag, "confirm"))
	{
		if (!player->zonesService->EnsureAllowed() || !KZ::zones::IsReady())
		{
			return;
		}
		Vector mins, maxs;
		if (!ValidateStateBox(player, state, mins, maxs))
		{
			return;
		}
		KzCyberZone zone {};
		zone.type = state.type;
		zone.jumpFactor = state.type == KZ_CYBER_ZONE_MODIFIER ? state.jumpFactor : 1.0f;
		zone.mins = mins;
		zone.maxs = maxs;
		V_snprintf(zone.courseDescriptor, sizeof(zone.courseDescriptor), "%s", state.courseDescriptor);
		RedrawPreview(player, state);

		if (state.editZoneId[0] != '\0' && state.type == KZ_CYBER_ZONE_MODIFIER)
		{
			// Правка бустера: у него нет штатной замены на стороне api — DELETE + POST.
			DeleteZoneThenSubmit(player->GetPlayerSlot(), player->GetSteamId64(false), state.editZoneId, zone,
								 state.courseId[0] ? state.courseId : "", 1, KZ::zones::CurrentMapName());
		}
		else
		{
			// Постановка и правка start/end: POST того же типа и курса, api вернёт replaced[].
			player->zonesService->SubmitZone(zone, state.courseId[0] ? state.courseId : nullptr);
		}
		// Меню не закрываем: назад в корень. Счётчики зон там пока прежние — ответ api ещё
		// летит; когда набор применится, открытый корень перестроит RefreshOpenEditorMenus.
		OpenSkeleton(player, ZmScreen::Root);
		return;
	}
}

static_function void OnPlacementAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	if (KZ_STREQ(tag, "height"))
	{
		state.height = MIN(MAX(state.height + delta, minValue), maxValue);
		g_pMenus->SetItemText(menu, item, HeightRowText(state.height).c_str());
		RedrawPreview(player, state);
		return;
	}
	if (KZ_STREQ(tag, "factor"))
	{
		state.jumpFactor = MIN(MAX(state.jumpFactor + delta, minValue), maxValue);
		g_pMenus->SetItemText(menu, item, FactorRowText(state.jumpFactor).c_str());
		return;
	}
}

static_function void OpenPlacement(KZPlayer *player)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuState &state = s_zmState[slot];
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.placement);

	char title[192];
	if (state.courseDescriptor[0])
	{
		V_snprintf(title, sizeof(title), "%s: %s → %s", state.editZoneId[0] ? "Правка" : "Постановка", ZoneTypeRu(state.type), state.courseLabel);
	}
	else
	{
		V_snprintf(title, sizeof(title), "%s: %s", state.editZoneId[0] ? "Правка" : "Постановка", ZoneTypeRu(state.type));
	}
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnPlacementSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	state.idxA = g_pMenus->AddItem(m, PointRowText("Точка A", state.hasA, state.a).c_str(), "a", false);
	state.idxB = g_pMenus->AddItem(m, PointRowText("Точка B", state.hasB, state.b).c_str(), "b", false);
	state.idxHeight = g_pMenus->AddAdjustableItem(m, HeightRowText(state.height).c_str(), "height", KZ_ZONEMENU_HEIGHT_STEP, KZ_ZONEMENU_HEIGHT_MIN,
												  KZ_ZONEMENU_HEIGHT_MAX);
	state.idxFactor = -1;
	if (state.type == KZ_CYBER_ZONE_MODIFIER)
	{
		state.idxFactor = g_pMenus->AddAdjustableItem(m, FactorRowText(state.jumpFactor).c_str(), "factor", KZ_ZONEMENU_FACTOR_STEP,
													  KZ_ZONEMENU_FACTOR_MIN, KZ_ZONEMENU_FACTOR_MAX);
	}
	state.idxConfirm = g_pMenus->AddItem(m, "✓ Подтвердить", "confirm", !(state.hasA && state.hasB));
	g_pMenus->AddItem(m, "Отмена", "cancel", false);

	g_pMenus->SetAdjustCallback(m, &OnPlacementAdjust);
	EnableAdjustCapture(m);
	g_pMenus->SetCloseOnSelect(m, false);
	handles.placement = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// --- Подменю точки (правка зоны) ---

static_function std::string CoordRowText(const char *axis, f32 value)
{
	char text[48];
	if (g_menusHasAdjustCapture)
	{
		V_snprintf(text, sizeof(text), "%s: %.0f", axis, value);
	}
	else
	{
		V_snprintf(text, sizeof(text), "%s: %.0f (A/D ±шаг)", axis, value);
	}
	return text;
}

// Индексы строк здесь жёстко связаны с порядком AddItem/AddAdjustableItem в BuildPointMenu:
// 0=aim, 1=X, 2=Y, 3=Z, 4=шаг, 5=save. Меняя порядок там — менять и здесь.
static_function void RefreshPointMenuTexts(MenuHandle menu, const Vector &point, f32 step)
{
	g_pMenus->SetItemText(menu, 1, CoordRowText("X", point.x).c_str());
	g_pMenus->SetItemText(menu, 2, CoordRowText("Y", point.y).c_str());
	g_pMenus->SetItemText(menu, 3, CoordRowText("Z", point.z).c_str());
	g_pMenus->SetItemText(menu, 4, StepRowText(step).c_str());
}

// Сохранение правки: start/end — штатная замена, бустер — DELETE + POST. Возвращает в список зон.
static_function void SaveZoneEdit(KZPlayer *player)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuState &state = s_zmState[slot];
	if (!player->zonesService->EnsureAllowed() || !KZ::zones::IsReady())
	{
		return;
	}
	Vector mins, maxs;
	if (!ValidateStateBox(player, state, mins, maxs))
	{
		return;
	}
	KzCyberZone zone {};
	zone.type = state.type;
	zone.jumpFactor = state.type == KZ_CYBER_ZONE_MODIFIER ? state.jumpFactor : 1.0f;
	zone.mins = mins;
	zone.maxs = maxs;
	V_snprintf(zone.courseDescriptor, sizeof(zone.courseDescriptor), "%s", state.courseDescriptor);
	RedrawPreview(player, state);

	if (state.type == KZ_CYBER_ZONE_MODIFIER)
	{
		DeleteZoneThenSubmit(player->GetPlayerSlot(), player->GetSteamId64(false), state.editZoneId, zone, state.courseId[0] ? state.courseId : "", 1,
							 KZ::zones::CurrentMapName());
	}
	else
	{
		player->zonesService->SubmitZone(zone, state.courseId[0] ? state.courseId : nullptr);
	}
	// Меню не закрываем: назад в список зон; свежие данные доедут через RefreshOpenEditorMenus.
	ReturnToZonesList(player);
}

static_function void OnPointSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	ZoneMenuHandles &handles = s_zmMenus[slot];
	const bool isA = menu == handles.pointA;
	Vector &point = isA ? state.a : state.b;
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	if (KZ_STREQ(tag, "aim"))
	{
		Vector aimed;
		if (!AimPoint(player, aimed))
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} прицел не упирается в поверхность.");
			return;
		}
		point = aimed;
		RefreshPointMenuTexts(menu, point, state.step);
		RedrawPreview(player, state);
		return;
	}
	if (KZ_STREQ(tag, "save"))
	{
		SaveZoneEdit(player);
		return;
	}
}

static_function void OnPointAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	ZoneMenuHandles &handles = s_zmMenus[slot];
	const bool isA = menu == handles.pointA;
	Vector &point = isA ? state.a : state.b;
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	if (KZ_STREQ(tag, "step"))
	{
		// Листаем ступени 1→8→32→128; delta задаёт направление.
		i32 idx = 0;
		for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(s_stepPresets); i++)
		{
			if (state.step >= s_stepPresets[i] - 0.5f)
			{
				idx = i;
			}
		}
		idx += delta > 0.0f ? 1 : -1;
		idx = MIN(MAX(idx, 0), (i32)KZ_ARRAYSIZE(s_stepPresets) - 1);
		state.step = s_stepPresets[idx];
		// Шаг общий на обе точки — обновляем строку «Шаг» и в соседнем меню, иначе там
		// остаётся устаревшее число до первого действия.
		RefreshPointMenuTexts(menu, point, state.step);
		const MenuHandle sibling = isA ? handles.pointB : handles.pointA;
		if (sibling != kInvalidMenuHandle)
		{
			RefreshPointMenuTexts(sibling, isA ? state.b : state.a, state.step);
		}
		return;
	}
	// Координаты: строка создана со step=1, реальный шаг — состояние (движок фиксирует step
	// при создании строки, а наш меняется ступенями).
	f32 *coord = nullptr;
	if (KZ_STREQ(tag, "x"))
	{
		coord = &point.x;
	}
	else if (KZ_STREQ(tag, "y"))
	{
		coord = &point.y;
	}
	else if (KZ_STREQ(tag, "z"))
	{
		coord = &point.z;
	}
	if (!coord)
	{
		return;
	}
	const f32 signedStep = delta > 0.0f ? state.step : -state.step;
	*coord = MIN(MAX(*coord + signedStep, minValue), maxValue);
	RefreshPointMenuTexts(menu, point, state.step);
	RedrawPreview(player, state);
}

static_function MenuHandle BuildPointMenu(const char *title, const Vector &point, f32 step)
{
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnPointSelect);
	if (m == kInvalidMenuHandle)
	{
		return m;
	}
	g_pMenus->AddItem(m, "Переставить по прицелу (E)", "aim", false);
	g_pMenus->AddAdjustableItem(m, CoordRowText("X", point.x).c_str(), "x", 1.0f, -KZ_ZONE_WORLD_LIMIT, KZ_ZONE_WORLD_LIMIT);
	g_pMenus->AddAdjustableItem(m, CoordRowText("Y", point.y).c_str(), "y", 1.0f, -KZ_ZONE_WORLD_LIMIT, KZ_ZONE_WORLD_LIMIT);
	g_pMenus->AddAdjustableItem(m, CoordRowText("Z", point.z).c_str(), "z", 1.0f, -KZ_ZONE_WORLD_LIMIT, KZ_ZONE_WORLD_LIMIT);
	g_pMenus->AddAdjustableItem(m, StepRowText(step).c_str(), "step", 1.0f, s_stepPresets[0], s_stepPresets[KZ_ARRAYSIZE(s_stepPresets) - 1]);
	g_pMenus->AddItem(m, "Сохранить", "save", false);
	g_pMenus->SetAdjustCallback(m, &OnPointAdjust);
	EnableAdjustCapture(m);
	g_pMenus->SetCloseOnSelect(m, false);
	return m;
}

// --- Экран правки зоны ---

static_function void OnZoneEditSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	if (KZ_STREQ(tag, "preview"))
	{
		RedrawPreview(player, state);
		return;
	}
	if (KZ_STREQ(tag, "save"))
	{
		SaveZoneEdit(player);
		return;
	}
	if (KZ_STREQ(tag, "delete"))
	{
		player->zonesService->DeleteZoneById(state.editZoneId);
		// Меню не закрываем: назад в список зон; зона исчезнет из него, когда api подтвердит
		// удаление (RemoveById -> ApplyLoadedZones -> RefreshOpenEditorMenus).
		ReturnToZonesList(player);
		return;
	}
}

static_function void OnZoneEditAdjust(MenuHandle menu, int slot, int item, f32 delta, f32 minValue, f32 maxValue)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !KZ_STREQ(tag, "height"))
	{
		return;
	}
	state.height = MIN(MAX(state.height + delta, minValue), maxValue);
	g_pMenus->SetItemText(menu, item, HeightRowText(state.height).c_str());
	RedrawPreview(player, state);
}

static_function void OpenZoneEdit(KZPlayer *player, const KzCyberZone &zone)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuState &state = s_zmState[slot];
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.zoneEdit);
	DestroyHandle(handles.pointA);
	DestroyHandle(handles.pointB);

	// Правка в локальном состоянии: диагональ исходных углов не хранится, восстанавливаем
	// A и B по полу зоны (обе точки на mins.z, высота — отдельной строкой).
	state.ResetEdit();
	state.type = zone.type;
	state.jumpFactor = zone.jumpFactor > 0.0f ? zone.jumpFactor : 1.0f;
	V_snprintf(state.editZoneId, sizeof(state.editZoneId), "%s", zone.id);
	V_snprintf(state.courseDescriptor, sizeof(state.courseDescriptor), "%s", zone.courseDescriptor);
	if (zone.courseDescriptor[0])
	{
		CourseLabel(zone.courseDescriptor, state.courseLabel, sizeof(state.courseLabel));
		const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(zone.courseDescriptor);
		if (record)
		{
			V_snprintf(state.courseId, sizeof(state.courseId), "%s", record->id);
		}
	}
	state.a = Vector(zone.mins.x, zone.mins.y, zone.mins.z);
	state.b = Vector(zone.maxs.x, zone.maxs.y, zone.mins.z);
	state.hasA = state.hasB = true;
	state.height = MAX(zone.maxs.z - zone.mins.z, (f32)KZ_ZONEMENU_HEIGHT_MIN);

	char title[192];
	if (zone.courseDescriptor[0])
	{
		V_snprintf(title, sizeof(title), "Зона: %s (%s)", ZoneTypeRu(zone.type), state.courseLabel);
	}
	else if (zone.type == KZ_CYBER_ZONE_MODIFIER)
	{
		V_snprintf(title, sizeof(title), "Зона: бустер x%.2f", zone.jumpFactor);
	}
	else
	{
		V_snprintf(title, sizeof(title), "Зона: %s", ZoneTypeRu(zone.type));
	}

	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnZoneEditSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}

	const bool numbered = zone.type == KZ_CYBER_ZONE_STAGE || zone.type == KZ_CYBER_ZONE_CHECKPOINT;
	if (!numbered)
	{
		// Геометрия: подменю точек (R возвращает сюда — связаны AddSubMenu) и высота.
		handles.pointA = BuildPointMenu("Точка A", state.a, state.step);
		handles.pointB = BuildPointMenu("Точка B", state.b, state.step);
		if (handles.pointA != kInvalidMenuHandle)
		{
			g_pMenus->AddSubMenu(m, "Точка A ▸", handles.pointA, "");
		}
		if (handles.pointB != kInvalidMenuHandle)
		{
			g_pMenus->AddSubMenu(m, "Точка B ▸", handles.pointB, "");
		}
		state.idxHeight = g_pMenus->AddAdjustableItem(m, HeightRowText(state.height).c_str(), "height", KZ_ZONEMENU_HEIGHT_STEP,
													  KZ_ZONEMENU_HEIGHT_MIN, KZ_ZONEMENU_HEIGHT_MAX);
	}
	g_pMenus->AddItem(m, "Показать превью", "preview", false);
	if (!numbered)
	{
		// Замена stage/checkpoint не поддержана осознанно: POST без stageNumber уводит стейдж
		// в конец и перенумеровывает курс — молча менять номера зон правкой геометрии нельзя.
		g_pMenus->AddItem(m, "Сохранить", "save", false);
	}
	g_pMenus->AddItem(m, "Удалить", "delete", false);

	g_pMenus->SetAdjustCallback(m, &OnZoneEditAdjust);
	EnableAdjustCapture(m);
	g_pMenus->SetCloseOnSelect(m, false);
	handles.zoneEdit = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// --- Списки зон ---

static_function std::string ZoneRowText(const KzCyberZone &zone)
{
	char label[128];
	char text[192];
	if (zone.type == KZ_CYBER_ZONE_MODIFIER)
	{
		V_snprintf(text, sizeof(text), "бустер x%.2f (%.0f %.0f %.0f)", zone.jumpFactor, zone.mins.x, zone.mins.y, zone.mins.z);
		return text;
	}
	if (zone.courseDescriptor[0])
	{
		CourseLabel(zone.courseDescriptor, label, sizeof(label));
		if (zone.type == KZ_CYBER_ZONE_STAGE || zone.type == KZ_CYBER_ZONE_CHECKPOINT)
		{
			V_snprintf(text, sizeof(text), "%s %d (%s)", ZoneTypeRu(zone.type), zone.stageNumber, label);
		}
		else
		{
			V_snprintf(text, sizeof(text), "%s (%s)", ZoneTypeRu(zone.type), label);
		}
		return text;
	}
	V_snprintf(text, sizeof(text), "%s", ZoneTypeRu(zone.type));
	return text;
}

static_function void OnZonesListSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	const char *zoneId = g_pMenus->GetItemInfo(menu, item);
	if (!zoneId || !zoneId[0])
	{
		return;
	}
	// Откуда зашли в правку — до OpenZoneEdit (он чистит состояние слота). Курсовый список =
	// хэндл courseZonesList с непустым фильтром; общий список зон карты даёт пустой возврат.
	char returnFilter[128] = "";
	if (menu == s_zmMenus[slot].courseZonesList)
	{
		V_snprintf(returnFilter, sizeof(returnFilter), "%s", s_zmState[slot].lastListFilter);
	}
	for (const KzCyberZone &zone : KZ::zones::Loaded())
	{
		if (KZ_STREQ(zone.id, zoneId))
		{
			OpenZoneEdit(player, zone);
			V_snprintf(s_zmState[slot].returnFilter, sizeof(s_zmState[slot].returnFilter), "%s", returnFilter);
			return;
		}
	}
	player->PrintChat(true, false, "{grey}Зоны:{default} зона уже удалена или набор обновился.");
}

// Список зон: без фильтра — все зоны карты, с фильтром — только зоны курса.
static_function MenuHandle BuildZonesList(const char *filterDescriptor)
{
	char title[192];
	if (filterDescriptor && filterDescriptor[0])
	{
		char label[128];
		CourseLabel(filterDescriptor, label, sizeof(label));
		V_snprintf(title, sizeof(title), "Зоны курса %s", label);
	}
	else
	{
		V_snprintf(title, sizeof(title), "Зоны карты %s", KZ::zones::CurrentMapName());
	}
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnZonesListSelect);
	if (m == kInvalidMenuHandle)
	{
		return m;
	}
	i32 added = 0;
	for (const KzCyberZone &zone : KZ::zones::Loaded())
	{
		if (filterDescriptor && filterDescriptor[0] && !KZ_STREQI(zone.courseDescriptor, filterDescriptor))
		{
			continue;
		}
		// Зона без id пришла бы только из битого ответа api — редактировать её нечем.
		g_pMenus->AddItem(m, ZoneRowText(zone).c_str(), zone.id, zone.id[0] == '\0');
		added++;
	}
	if (added == 0)
	{
		g_pMenus->AddItem(m, "Зон нет", "", true);
	}
	g_pMenus->SetCloseOnSelect(m, false);
	return m;
}

static_function void OpenZonesList(KZPlayer *player, const char *filterDescriptor)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.courseZonesList);
	// Фильтр запоминаем для наблюдателя: придёт ответ api — список перестроится с ним же.
	V_snprintf(s_zmState[slot].lastListFilter, sizeof(s_zmState[slot].lastListFilter), "%s", filterDescriptor ? filterDescriptor : "");
	MenuHandle m = BuildZonesList(filterDescriptor);
	if (m == kInvalidMenuHandle)
	{
		return;
	}
	handles.courseZonesList = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// --- Курсы: выбор в мастере, управление, создание ---

static_function std::string CourseRowText(const char *descriptor)
{
	const KZCourseDescriptor *live = FindLiveCourse(descriptor);
	const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(descriptor);
	char text[224];
	const char *name = live && live->name[0] ? live->name : descriptor;
	const bool disabled = (live && live->disabled) || (record && record->disabled);
	V_snprintf(text, sizeof(text), "%s%s%s", name, record && record->own ? " — наш" : "", disabled ? " — отключён" : "");
	return text;
}

// Заполнить контекст выбранного курса в состоянии слота. false = курса больше нет.
static_function bool FillCourseContext(ZoneMenuState &state, const char *descriptor)
{
	const KZCourseDescriptor *live = FindLiveCourse(descriptor);
	const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(descriptor);
	if (!live && !record)
	{
		return false;
	}
	// Каноническое написание дескриптора — живое с карты (KZ_STREQI при поиске, KZ_STREQ при
	// счёте зон курса: регистр важен).
	V_snprintf(state.courseDescriptor, sizeof(state.courseDescriptor), "%s", live ? live->entityTargetname : record->descriptor);
	V_snprintf(state.courseId, sizeof(state.courseId), "%s", record ? record->id : "");
	CourseLabel(state.courseDescriptor, state.courseLabel, sizeof(state.courseLabel));
	state.courseOwn = record && record->own;
	state.courseDisabled = (live && live->disabled) || (record && record->disabled);
	state.courseNumber = live ? KZ::course::GetCyberCourseNumber(live) : record->courseNumber;
	return true;
}

// Показывать ли курс в списках выбора/управления. Курс, известный платформе (есть запись
// api), — всегда да. Курс без записи скрываем, если он платформенный: это синтетический
// «Default», который форк заводит сам на картах без Mapping API (или наш ещё не оживший), —
// он не выбор админа, а автоматика безкурсовых зон; предлагать его значило бы регистрировать
// override на собственную синтетику. Мапперские курсы IsPlatformOwnedCourse не проходят и
// остаются в списке всегда.
static_function bool CourseSelectable(const KZCourseDescriptor *course)
{
	if (KZ::zones::FindCourseByDescriptor(course->entityTargetname))
	{
		return true;
	}
	return !KZ::mapapi::IsPlatformOwnedCourse(course->entityTargetname);
}

static_function void OnCreateCourseSelectWizard(MenuHandle menu, int slot, int item);
static_function void OnCreateCourseSelectManage(MenuHandle menu, int slot, int item);

// Общая постройка меню «+ Создать курс»: предустановки Main (если main нет) и Bonus N.
static_function void OpenCreateCourse(KZPlayer *player, bool fromWizard)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.createCourse);

	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, "Создать курс", fromWizard ? &OnCreateCourseSelectWizard : &OnCreateCourseSelectManage);
	if (m == kInvalidMenuHandle)
	{
		return;
	}
	const bool hasMain = MainCourseExists();
	g_pMenus->AddItem(m, hasMain ? "Main — уже есть" : "Main", "0", hasMain);
	const i32 bonus = NextFreeBonusNumber();
	char text[64], info[16];
	if (bonus > 0)
	{
		V_snprintf(text, sizeof(text), "Bonus %d", bonus);
		V_snprintf(info, sizeof(info), "%d", bonus);
		g_pMenus->AddItem(m, text, info, false);
	}
	else
	{
		g_pMenus->AddItem(m, "Бонусы: нет свободных номеров", "", true);
	}
	g_pMenus->SetCloseOnSelect(m, false);
	handles.createCourse = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// Создание курса из меню: предустановка по номеру, descriptor генерирует api
// (cyber_main / cyber_bonus_<n>). Произвольное имя — только командой !zone course new.
static_function void CreateCourseFromMenu(KZPlayer *player, MenuHandle menu, int item, bool fromWizard)
{
	if (!player->zonesService->EnsureAllowed() || !KZ::zones::IsReady())
	{
		return;
	}
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	const i32 number = atoi(info);
	SendCourseCreate(player, "own", nullptr, number,
					 [fromWizard](KZPlayer *player, const std::string &courseId, const std::string &descriptor)
					 {
						 player->PrintChat(true, false, "{grey}Зоны:{default} курс {yellow}%s{default} создан.", descriptor.c_str());
						 const i32 slot = player->GetPlayerSlot().Get();
						 if (slot < 0 || slot >= MAXPLAYERS)
						 {
							 return;
						 }
						 ZoneMenuState &state = s_zmState[slot];
						 if (!KZ_STREQ(state.map.c_str(), KZ::zones::CurrentMapName()))
						 {
							 return;
						 }
						 if (fromWizard)
						 {
							 // Продолжаем мастер: курс выбран, дальше панель постановки.
							 V_snprintf(state.courseDescriptor, sizeof(state.courseDescriptor), "%s", descriptor.c_str());
							 V_snprintf(state.courseId, sizeof(state.courseId), "%s", courseId.c_str());
							 CourseLabel(descriptor.c_str(), state.courseLabel, sizeof(state.courseLabel));
							 OpenPlacement(player);
						 }
						 else
						 {
							 // Меню не закрываем: назад в список курсов. Новый курс появится в нём,
							 // когда доедет RefreshFromApi (RefreshOpenEditorMenus).
							 OpenSkeleton(player, ZmScreen::CoursesList);
						 }
					 });
}

static_function void OnCreateCourseSelectWizard(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (player)
	{
		CreateCourseFromMenu(player, menu, item, true);
	}
}

static_function void OnCreateCourseSelectManage(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (player)
	{
		CreateCourseFromMenu(player, menu, item, false);
	}
}

// Выбор курса в мастере постановки start/end.
static_function void OnWizardCourseSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	if (KZ_STREQ(info, "create"))
	{
		OpenCreateCourse(player, true);
		return;
	}
	if (!FillCourseContext(state, info))
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} курс уже исчез с карты.");
		return;
	}
	if (state.courseId[0] != '\0')
	{
		OpenPlacement(player);
		return;
	}
	// Родной курс ещё не заведён в api — регистрируем override и продолжаем из ответа: без id
	// POST зоны ушёл бы вне курса, и зона молча встала бы не туда.
	EnsureCourseRegistered(player, state.courseDescriptor, state.courseNumber,
						   [](KZPlayer *player, const std::string &courseId, const std::string &descriptor)
						   {
							   const i32 slot = player->GetPlayerSlot().Get();
							   if (slot < 0 || slot >= MAXPLAYERS)
							   {
								   return;
							   }
							   ZoneMenuState &state = s_zmState[slot];
							   if (!KZ_STREQ(state.map.c_str(), KZ::zones::CurrentMapName())
								   || !KZ_STREQI(state.courseDescriptor, descriptor.c_str()))
							   {
								   return; // контекст уехал, пока летел запрос
							   }
							   V_snprintf(state.courseId, sizeof(state.courseId), "%s", courseId.c_str());
							   OpenPlacement(player);
						   });
}

static_function void OpenWizardCourseSelect(KZPlayer *player)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.wizardCourseSel);

	char title[96];
	V_snprintf(title, sizeof(title), "Курс для зоны %s", ZoneTypeRu(s_zmState[slot].type));
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnWizardCourseSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		if (!course || !CourseSelectable(course))
		{
			continue;
		}
		// Отключённый курс серый: сначала верни курс, потом ставь ему зоны.
		const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(course->entityTargetname);
		const bool disabled = course->disabled || (record && record->disabled);
		g_pMenus->AddItem(m, CourseRowText(course->entityTargetname).c_str(), course->entityTargetname, disabled);
	}
	g_pMenus->AddItem(m, "+ Создать курс", "create", false);
	g_pMenus->SetCloseOnSelect(m, false);
	handles.wizardCourseSel = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

// --- Экран действий курса ---

static_function void OnCourseActSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	if (KZ_STREQ(tag, "zones"))
	{
		OpenZonesList(player, state.courseDescriptor);
		return;
	}
	if (KZ_STREQ(tag, "setstart") || KZ_STREQ(tag, "setend"))
	{
		if (!player->zonesService->EnsureAllowed() || !KZ::zones::IsReady())
		{
			return;
		}
		state.type = KZ_STREQ(tag, "setstart") ? KZ_CYBER_ZONE_START : KZ_CYBER_ZONE_END;
		state.jumpFactor = 1.0f;
		state.hasA = state.hasB = false;
		state.height = KZ_ZONE_EDITOR_HEIGHT;
		state.editZoneId[0] = '\0';
		if (state.courseId[0] != '\0')
		{
			OpenPlacement(player);
			return;
		}
		EnsureCourseRegistered(player, state.courseDescriptor, state.courseNumber,
							   [](KZPlayer *player, const std::string &courseId, const std::string &descriptor)
							   {
								   const i32 slot = player->GetPlayerSlot().Get();
								   if (slot < 0 || slot >= MAXPLAYERS)
								   {
									   return;
								   }
								   ZoneMenuState &state = s_zmState[slot];
								   if (!KZ_STREQ(state.map.c_str(), KZ::zones::CurrentMapName())
									   || !KZ_STREQI(state.courseDescriptor, descriptor.c_str()))
								   {
									   return;
								   }
								   V_snprintf(state.courseId, sizeof(state.courseId), "%s", courseId.c_str());
								   OpenPlacement(player);
							   });
		return;
	}
	if (KZ_STREQ(tag, "delcourse"))
	{
		if (!player->zonesService->EnsureAllowed())
		{
			return;
		}
		if (state.courseId[0] == '\0')
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} у курса нет id в api — удалить нельзя.");
			return;
		}
		SendCourseDelete(player->GetPlayerSlot(), player->GetSteamId64(false), state.courseId, 1);
		// Меню не закрываем: назад в список курсов; курс исчезнет из него после ответа api.
		OpenSkeleton(player, ZmScreen::CoursesList);
		return;
	}
	if (KZ_STREQ(tag, "disable") || KZ_STREQ(tag, "enable"))
	{
		if (!player->zonesService->EnsureAllowed())
		{
			return;
		}
		const bool disable = KZ_STREQ(tag, "disable");
		if (state.courseId[0] != '\0')
		{
			SendCoursePatch(player, state.courseId, disable);
		}
		else
		{
			// Родной курс ещё не заведён — регистрируем и гасим одним действием.
			EnsureCourseRegistered(player, state.courseDescriptor, state.courseNumber,
								   [disable](KZPlayer *player, const std::string &courseId, const std::string &)
								   { SendCoursePatch(player, courseId, disable); });
		}
		// Меню не закрываем и никуда не уходим: экран курса перестроит RefreshOpenEditorMenus,
		// когда PATCH подтвердится и набор пересинхронизируется (пункт «Отключить» сменится
		// на «Включить обратно» сам).
		return;
	}
}

static_function void OpenCourseActions(KZPlayer *player)
{
	const i32 slot = player->GetPlayerSlot().Get();
	ZoneMenuState &state = s_zmState[slot];
	ZoneMenuHandles &handles = s_zmMenus[slot];
	DestroyHandle(handles.courseAct);

	char title[192];
	V_snprintf(title, sizeof(title), "Курс: %s%s", state.courseLabel, state.courseDisabled ? " (отключён)" : "");
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnCourseActSelect);
	if (m == kInvalidMenuHandle)
	{
		return;
	}
	g_pMenus->AddItem(m, "Переставить start ▸", "setstart", false);
	g_pMenus->AddItem(m, "Переставить end ▸", "setend", false);
	if (state.courseOwn)
	{
		g_pMenus->AddItem(m, "Удалить курс", "delcourse", false);
	}
	else if (state.courseDisabled)
	{
		g_pMenus->AddItem(m, "Включить обратно", "enable", false);
	}
	else
	{
		g_pMenus->AddItem(m, "Отключить на сервере", "disable", false);
	}
	g_pMenus->AddItem(m, "Зоны курса ▸", "zones", false);
	g_pMenus->SetCloseOnSelect(m, false);
	handles.courseAct = m;
	g_pMenus->DisplayMenu(m, slot, 0);
}

static_function void OnCoursesListSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	const char *info = g_pMenus->GetItemInfo(menu, item);
	if (!info || !info[0])
	{
		return;
	}
	if (KZ_STREQ(info, "create"))
	{
		OpenCreateCourse(player, false);
		return;
	}
	if (!FillCourseContext(s_zmState[slot], info))
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} курс уже исчез с карты.");
		return;
	}
	OpenCourseActions(player);
}

// Список курсов: живые с карты (родные + наши), плюс записи api, которых на карте больше нет
// (карта обновилась в Workshop) — они серые, действий по ним из меню нет.
static_function MenuHandle BuildCoursesList(i32 &outCount)
{
	char title[160];
	V_snprintf(title, sizeof(title), "Курсы карты %s", KZ::zones::CurrentMapName());
	MenuHandle m = g_pMenus->CreateMenu(MenuType::Default, title, &OnCoursesListSelect);
	if (m == kInvalidMenuHandle)
	{
		return m;
	}
	outCount = 0;
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		if (!course || !CourseSelectable(course))
		{
			continue;
		}
		g_pMenus->AddItem(m, CourseRowText(course->entityTargetname).c_str(), course->entityTargetname, false);
		outCount++;
	}
	for (const KzCyberCourse &record : KZ::zones::Courses())
	{
		if (FindLiveCourse(record.descriptor))
		{
			continue;
		}
		char text[192];
		V_snprintf(text, sizeof(text), "%s — нет на карте", record.descriptor);
		g_pMenus->AddItem(m, text, "", true);
		outCount++;
	}
	g_pMenus->AddItem(m, "+ Создать курс", "create", false);
	g_pMenus->SetCloseOnSelect(m, false);
	return m;
}

// --- Мастер: выбор типа ---

static_function void OnTypeSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	if (!player->zonesService->EnsureAllowed() || !KZ::zones::IsReady())
	{
		return;
	}
	ZoneMenuState &state = s_zmState[slot];
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (!tag || !tag[0])
	{
		return;
	}
	state.ResetEdit(); // map не трогает — контекст карты остаётся
	if (KZ_STREQ(tag, "booster"))
	{
		state.type = KZ_CYBER_ZONE_MODIFIER;
		OpenPlacement(player);
		return;
	}
	state.type = KZ_STREQ(tag, "start") ? KZ_CYBER_ZONE_START : KZ_CYBER_ZONE_END;
	// Есть ли на карте курсы, среди которых реально есть что выбирать (см. CourseSelectable —
	// синтетический «Default» форка выбором не считается). Нет — существующая автоматика
	// платформенного курса: зона уходит без курса, EnsurePlatformCourse заведёт дефолтный.
	bool anySelectable = false;
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total && !anySelectable; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		anySelectable = course && CourseSelectable(course);
	}
	if (!anySelectable)
	{
		OpenPlacement(player);
		return;
	}
	OpenWizardCourseSelect(player);
}

// --- Корень ---

static_function void OnRootSelect(MenuHandle menu, int slot, int item)
{
	KZPlayer *player = MenuPlayer(slot);
	if (!player)
	{
		return;
	}
	const char *tag = g_pMenus->GetItemInfo(menu, item);
	if (tag && KZ_STREQ(tag, "showall"))
	{
		// Ровно то, что делает !zone show (там же кулдаун и бюджеты).
		player->zonesService->ShowAllZones();
	}
}

// Перестроить весь скелет (корень + статичные подсписки) и показать выбранный экран.
// Общий путь и для !zones, и для возвратов после действий, и для наблюдателя: данные списков
// живут в наборе, а не в меню, поэтому каждая перестройка читает свежие (паттерн см. replays/menu.cpp).
static_function void OpenSkeleton(KZPlayer *player, ZmScreen show)
{
	const i32 slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return;
	}

	DestroySlotMenus(slot);
	ZoneMenuState &state = s_zmState[slot];
	state.ResetEdit();
	state.map = KZ::zones::CurrentMapName();
	ZoneMenuHandles &handles = s_zmMenus[slot];

	char title[160];
	V_snprintf(title, sizeof(title), "Зоны и курсы — %s", KZ::zones::CurrentMapName());
	MenuHandle root = g_pMenus->CreateMenu(MenuType::Default, title, &OnRootSelect);
	if (root == kInvalidMenuHandle)
	{
		return;
	}
	handles.root = root;

	// Тип зоны — статичное подменю (R возвращает в корень).
	MenuHandle typeSel = g_pMenus->CreateMenu(MenuType::Default, "Тип зоны", &OnTypeSelect);
	if (typeSel != kInvalidMenuHandle)
	{
		g_pMenus->AddItem(typeSel, "Старт", "start", false);
		g_pMenus->AddItem(typeSel, "Финиш", "end", false);
		g_pMenus->AddItem(typeSel, "Бустер", "booster", false);
		g_pMenus->SetCloseOnSelect(typeSel, false);
		handles.typeSel = typeSel;
		g_pMenus->AddSubMenu(root, "Поставить зону ▸", typeSel, "");
	}

	handles.zonesList = BuildZonesList(nullptr);
	if (handles.zonesList != kInvalidMenuHandle)
	{
		char label[96];
		V_snprintf(label, sizeof(label), "Зоны карты (%d) ▸", (i32)KZ::zones::Loaded().size());
		g_pMenus->AddSubMenu(root, label, handles.zonesList, "");
	}

	i32 courseCount = 0;
	handles.coursesList = BuildCoursesList(courseCount);
	if (handles.coursesList != kInvalidMenuHandle)
	{
		char label[96];
		V_snprintf(label, sizeof(label), "Курсы карты (%d) ▸", courseCount);
		g_pMenus->AddSubMenu(root, label, handles.coursesList, "");
	}

	g_pMenus->AddItem(root, "Показать все зоны", "showall", false);
	g_pMenus->SetCloseOnSelect(root, false);

	// Подсписок мог не создаться (движок не отдал хэндл) — корень остаётся честным фолбэком.
	MenuHandle target = root;
	if (show == ZmScreen::ZonesList && handles.zonesList != kInvalidMenuHandle)
	{
		target = handles.zonesList; // parent = root (AddSubMenu), R возвращает в корень
	}
	else if (show == ZmScreen::CoursesList && handles.coursesList != kInvalidMenuHandle)
	{
		target = handles.coursesList;
	}
	g_pMenus->DisplayMenu(target, slot, 0);
}

void KZ::zones::OpenZonesMenu(KZPlayer *player)
{
	if (!player || !player->zonesService)
	{
		return;
	}
	// Гейт ДО создания меню: у не-админов команда не рисует ничего.
	if (!player->zonesService->EnsureAllowed())
	{
		return;
	}
	if (g_pMenus == nullptr)
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} движок меню не загружен.");
		return;
	}
	if (!KZ::zones::IsReady())
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} набор карты ещё не загружен из api.");
		return;
	}
	OpenSkeleton(player, ZmScreen::Root);
}

// Ответ api пришёл и набор применён (зовёт kz_zones.cpp из ApplyLoadedZones): перестроить
// ОБЗОРНЫЕ экраны редактора у всех, кто их сейчас смотрит, — списки и счётчики обязаны
// показывать применённое состояние, а не снимок на момент открытия. Экраны правки
// (постановка, правка зоны, точки, мастер) не трогаем: там незакрытая работа игрока, и
// перерисовка данными сервера снесла бы несохранённые значения.
void KZ::zones::RefreshOpenEditorMenus()
{
	if (g_pMenus == nullptr)
	{
		return;
	}
	for (i32 slot = 0; slot < MAXPLAYERS; slot++)
	{
		// MenuPlayer сверяет карту состояния с текущей: меню, пережившее смену карты, не трогаем
		// (его колбэки и так молчат по той же сверке).
		KZPlayer *player = MenuPlayer(slot);
		if (!player)
		{
			continue;
		}
		ZoneMenuHandles &handles = s_zmMenus[slot];
		ZoneMenuState &state = s_zmState[slot];
		const MenuHandle active = g_pMenus->GetActiveMenu(slot);
		if (active == kInvalidMenuHandle)
		{
			continue;
		}
		if (active == handles.root)
		{
			OpenSkeleton(player, ZmScreen::Root);
		}
		else if (active == handles.zonesList)
		{
			OpenSkeleton(player, ZmScreen::ZonesList);
		}
		else if (active == handles.coursesList)
		{
			OpenSkeleton(player, ZmScreen::CoursesList);
		}
		else if (active == handles.courseZonesList)
		{
			// Свой список (курсовый или общий после возврата) — перестроить с тем же фильтром.
			char filter[128];
			V_snprintf(filter, sizeof(filter), "%s", state.lastListFilter);
			OpenZonesList(player, filter[0] ? filter : nullptr);
		}
		else if (active == handles.courseAct)
		{
			// Контекст курса перечитываем из свежего набора; курс мог исчезнуть целиком.
			if (FillCourseContext(state, state.courseDescriptor))
			{
				OpenCourseActions(player);
			}
			else
			{
				OpenSkeleton(player, ZmScreen::CoursesList);
			}
		}
	}
}

// --- Команды управления курсами: !zone course new|delete|disable|enable <имя> ---

// Имя курса из хвоста аргументов (имя может содержать пробелы).
static_function std::string JoinArgs(const CCommand *args, i32 from)
{
	std::string name;
	for (i32 i = from; i < args->ArgC(); i++)
	{
		if (!name.empty())
		{
			name += ' ';
		}
		name += args->Arg(i);
	}
	// Пробелы по краям контракт отбивает; срезаем сами, чтобы «имя не то» не выглядело загадкой.
	while (!name.empty() && name.front() == ' ')
	{
		name.erase(name.begin());
	}
	while (!name.empty() && name.back() == ' ')
	{
		name.pop_back();
	}
	return name;
}

// Курс по аргументу команды: сначала как дескриптор (targetname), затем как видимое имя —
// в !courses и меню игрок видит именно имя, и «Bonus 1» обязан находиться, даже если
// targetname у него другой. Имена не уникальны (см. GetCourseLocalDatabaseId) — при
// дубликатах берётся первый по списку, это осознанная цена адресации по имени.
static_function const KZCourseDescriptor *FindLiveCourseByArg(const char *arg)
{
	const KZCourseDescriptor *byDescriptor = FindLiveCourse(arg);
	if (byDescriptor)
	{
		return byDescriptor;
	}
	const u32 total = KZ::course::GetCourseDescriptorTotal();
	for (u32 i = 0; i < total; i++)
	{
		const KZCourseDescriptor *course = KZ::course::GetCourseDescriptorByIndex(i);
		if (course && KZ_STREQI(course->name, arg))
		{
			return course;
		}
	}
	return nullptr;
}

void KZ::zones::CourseSubcommand(KZPlayer *player, const CCommand *args)
{
	if (!player || !player->zonesService)
	{
		return;
	}
	if (!player->zonesService->EnsureAllowed())
	{
		return;
	}
	if (!KZ::zones::IsReady())
	{
		player->PrintChat(true, false, "{grey}Зоны:{default} набор карты ещё не загружен из api.");
		return;
	}
	const char *sub = args->ArgC() > 2 ? args->Arg(2) : "";
	const std::string name = JoinArgs(args, 3);
	if (!sub[0] || name.empty())
	{
		player->PrintChat(true, false, "{grey}Курсы:{default} !zone course new <имя> | delete <имя> | disable <имя> | enable <имя>");
		return;
	}
	// Слот с картой — для колбэков создания (контекст меню тут не используется, но
	// MenuPlayer-валидации курсовых continuation'ов сверяют state.map).
	const i32 slot = player->GetPlayerSlot().Get();
	if (slot >= 0 && slot < MAXPLAYERS && s_zmState[slot].map.empty())
	{
		s_zmState[slot].map = KZ::zones::CurrentMapName();
	}

	if (KZ_STREQI(sub, "new"))
	{
		if (name.size() > 120)
		{
			player->PrintChat(true, false, "{grey}Курсы:{default} имя слишком длинное (максимум 120).");
			return;
		}
		// Номер новому курсу: main, если его нет, иначе следующий свободный бонус.
		i32 number = 0;
		if (MainCourseExists())
		{
			number = NextFreeBonusNumber();
			if (number < 0)
			{
				player->PrintChat(true, false, "{grey}Курсы:{default} свободных номеров бонусов не осталось.");
				return;
			}
		}
		SendCourseCreate(player, "own", name.c_str(), number, [](KZPlayer *player, const std::string &, const std::string &descriptor)
						 { player->PrintChat(true, false, "{grey}Курсы:{default} курс {yellow}%s{default} создан.", descriptor.c_str()); });
		return;
	}
	if (KZ_STREQI(sub, "delete"))
	{
		// Аргумент — дескриптор или видимое имя; запись api ищем по каноническому targetname.
		const KZCourseDescriptor *live = FindLiveCourseByArg(name.c_str());
		const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(live ? live->entityTargetname : name.c_str());
		if (!record || record->id[0] == '\0')
		{
			player->PrintChat(true, false, "{grey}Курсы:{default} курс {yellow}%s{default} в api не найден.", name.c_str());
			return;
		}
		if (!record->own)
		{
			player->PrintChat(true, false, "{grey}Курсы:{default} это родной курс карты — его можно только отключить: !zone course disable.");
			return;
		}
		SendCourseDelete(player->GetPlayerSlot(), player->GetSteamId64(false), record->id, 1);
		return;
	}
	if (KZ_STREQI(sub, "disable") || KZ_STREQI(sub, "enable"))
	{
		const bool disable = KZ_STREQI(sub, "disable");
		// Аргумент — дескриптор или видимое имя (см. FindLiveCourseByArg).
		const KZCourseDescriptor *live = FindLiveCourseByArg(name.c_str());
		const KzCyberCourse *record = KZ::zones::FindCourseByDescriptor(live ? live->entityTargetname : name.c_str());
		if (!live && !record)
		{
			player->PrintChat(true, false, "{grey}Курсы:{default} курс {yellow}%s{default} не найден на карте.", name.c_str());
			return;
		}
		if (record && record->id[0] != '\0')
		{
			SendCoursePatch(player, record->id, disable);
			return;
		}
		const char *canonical = live ? live->entityTargetname : record->descriptor;
		const i32 number = live ? KZ::course::GetCyberCourseNumber(live) : record->courseNumber;
		EnsureCourseRegistered(player, canonical, number, [disable](KZPlayer *player, const std::string &courseId, const std::string &)
							   { SendCoursePatch(player, courseId, disable); });
		return;
	}
	player->PrintChat(true, false, "{grey}Курсы:{default} !zone course new <имя> | delete <имя> | disable <имя> | enable <имя>");
}
