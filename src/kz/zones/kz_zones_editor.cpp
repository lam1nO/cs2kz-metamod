/*
	Редактор зон в игре: !zone start|end|booster <множитель>|cancel|list|show|remove <N>.

	Координаты берутся из позиции игрока — числа руками не вводятся нигде. Первый вызов
	пишет угол A, второй закрывает бокс и отправляет зону в api.

	Право game.kz_zones_edit спрашивается у api по steam_id и кэшируется на игровую сессию.
	Отказ показывается в чат: молчащая команда неотличима от сломанной.
*/

#include "kz_zones.h"
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/http.h"
#include "utils/ctimer.h"

// Энтити рёбер создаёт KZ::zones::DrawBoxEdges (kz_zones.cpp) — здесь только вызовы, поэтому
// cparticlesystem.h и entitykeyvalues.h больше не нужны. Из SDK остаётся разбор ответов api.
#include "tier1/keyvalues3.h"

#include <string>
#include <optional> // ParseApiReason принимает std::optional (приезжает и из utils/http.h, но явно надёжнее)
#include <vector>   // KZ::zones::Loaded() отдаёт std::vector
#include <stdlib.h>
#include <math.h>

#define KZ_ZONE_PERMISSION      "game.kz_zones_edit"
#define KZ_ZONE_PREVIEW_SECONDS 15.0f
#define KZ_ZONE_PREVIEW_NAME    "cyb_zone_preview"

// !zone show — временный показ всех зон вызвавшему.
#define KZ_ZONE_SHOW_SECONDS 12.0f
#define KZ_ZONE_SHOW_NAME    "cyb_zone_show"
// Раздутие бокса показа. У старта и финиша поверх лежит постоянный контур; ровно совпадающие
// рёбра давали бы z-fighting, а половины юнита хватает, чтобы линии разошлись, и мало, чтобы
// соврать про границу зоны.
#define KZ_ZONE_SHOW_INFLATE 0.5f

static_function std::string ApiBaseUrl()
{
	const char *url = KZOptionService::GetOptionStr("cybEmitUrl", "");
	if (!url || url[0] == '\0')
	{
		return "";
	}
	std::string base = url;
	if (!base.empty() && base.back() == '/')
	{
		base.pop_back();
	}
	return base;
}

static_function void AuthorizeRequest(HTTP::Request &req)
{
	const char *token = KZOptionService::GetOptionStr("cybEmitToken", "");
	if (token && token[0] != '\0')
	{
		req.SetHeader("Authorization", std::string("Bearer ") + token);
	}
}

// Причина отказа из тела ответа api ({"reason": "..."}). Пустая строка, если тела нет, оно не
// разобралось или поля нет.
static_function std::string ParseApiReason(const std::optional<std::string> &body)
{
	if (!body.has_value())
	{
		return "";
	}
	KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
	CUtlString error = "";
	LoadKV3FromJSON(&kv, &error, body->c_str(), "");
	if (!error.IsEmpty())
	{
		return "";
	}
	KeyValues3 *reason = kv.FindMember("reason");
	return reason ? reason->GetString("") : "";
}

// Слот игрока, а не указатель: пока запрос летит, игрок может отключиться, и держать
// KZPlayer* в лямбде значило бы обращаться к освобождённой памяти.
static_function KZPlayer *PlayerBySlotIfSame(CPlayerSlot slot, u64 expectedSteamId)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(slot);
	if (!player || !player->IsAuthenticated() || player->GetSteamId64(false) != expectedSteamId)
	{
		return nullptr;
	}
	return player;
}

void KZZonesService::OnMapChanged()
{
	// Право — свойство игрока, не карты: перепрашивать не нужно. А вот незакрытый угол A
	// привязан к геометрии прошлой карты, и его надо забыть.
	this->ClearPreview(true);
	this->ClearShow(true);
	this->hasPendingCorner = false;
	this->pendingType = {};
	this->pendingJumpFactor = {};
	this->pendingCorner = {};
}

void KZZonesService::RequestPermission()
{
	if (this->perm == PERM_PENDING || this->perm == PERM_ALLOWED || this->perm == PERM_DENIED)
	{
		return;
	}
	const std::string base = ApiBaseUrl();
	const u64 steamId = this->player->GetSteamId64(false);
	if (base.empty() || steamId == 0)
	{
		this->perm = PERM_DENIED;
		return;
	}
	this->perm = PERM_PENDING;

	char url[512];
	V_snprintf(url, sizeof(url), "%s/ingest/v1/players/%llu/permissions", base.c_str(), steamId);

	HTTP::Request req(HTTP::Method::GET, url);
	AuthorizeRequest(req);

	const CPlayerSlot slot = this->player->GetPlayerSlot();

	// clang-format off
	req.Send(
		[slot, steamId](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (!player || !player->zonesService)
			{
				return; // игрок ушёл, пока летел ответ
			}
			if (resp.status < 200 || resp.status >= 300)
			{
				player->zonesService->perm = KZZonesService::PERM_DENIED;
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_perm_failed steam_id=%llu reason=http_%u\n", steamId, (unsigned)resp.status);
				return;
			}
			std::optional<std::string> body = resp.Body();
			if (!body.has_value())
			{
				player->zonesService->perm = KZZonesService::PERM_DENIED;
				return;
			}
			KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
			CUtlString error = "";
			LoadKV3FromJSON(&kv, &error, body->c_str(), "");
			bool allowed = false;
			if (error.IsEmpty())
			{
				KeyValues3 *perms = kv.FindMember("permissions");
				if (perms && perms->GetType() == KV3_TYPE_ARRAY)
				{
					for (int i = 0; i < perms->GetArrayElementCount(); i++)
					{
						KeyValues3 *el = perms->GetArrayElement(i);
						if (el && KZ_STREQ(el->GetString(""), KZ_ZONE_PERMISSION))
						{
							allowed = true;
							break;
						}
					}
				}
			}
			player->zonesService->perm = allowed ? KZZonesService::PERM_ALLOWED : KZZonesService::PERM_DENIED;
		},
		[slot, steamId]()
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player && player->zonesService)
			{
				// «Не смогли проверить» — это не «нет права»: возвращаем UNKNOWN, чтобы
				// следующая команда спросила заново, а не заперла редактор до реконнекта.
				player->zonesService->perm = KZZonesService::PERM_UNKNOWN;
			}
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_perm_failed steam_id=%llu reason=network_error\n", steamId);
		});
	// clang-format on
}

bool KZZonesService::EnsureAllowed()
{
	switch (this->perm)
	{
		case PERM_ALLOWED:
			return true;
		case PERM_PENDING:
			this->player->PrintChat(true, false, "{grey}Зоны:{default} проверяю права, повтори через пару секунд.");
			return false;
		case PERM_DENIED:
			this->player->PrintChat(true, false, "{grey}Зоны:{default} нет права {darkred}%s{default}.", KZ_ZONE_PERMISSION);
			return false;
		default:
			this->RequestPermission();
			this->player->PrintChat(true, false, "{grey}Зоны:{default} проверяю права, повтори через пару секунд.");
			return false;
	}
}

void KZZonesService::ClearPreview(bool keepEntities)
{
	// На смене карты мир перестраивается и прежние энтити уже не наши: лезть туда с
	// RemoveEntity незачем и небезопасно, достаточно забыть хендлы (keepEntities).
	KZ::zones::RemoveBoxEdges(this->previewBeams, KZ_ZONE_BOX_EDGES, KZ_ZONE_PREVIEW_NAME, keepEntities);
	this->previewActive = false;
}

bool KZZonesService::OwnsParticle(const CEntityHandle &handle) const
{
	// Порядок проверок ценой наружу: этот метод зовёт KZ::quiet::OnCheckTransmit на каждый
	// CheckTransmit для каждого получателя. Дешёвый гейт HasOwnedParticles() стоит ПЕРЕД
	// вызовом (там же), а здесь сканы гейтятся своими флагами по отдельности.
	if (this->previewActive)
	{
		for (const CEntityHandle &beam : this->previewBeams)
		{
			if (beam == handle)
			{
				return true;
			}
		}
	}
	for (const CEntityHandle &beam : this->showBeams)
	{
		if (beam == handle)
		{
			return true;
		}
	}
	return false;
}

void KZZonesService::ClearShow(bool keepEntities)
{
	if (this->showBeams.empty())
	{
		return;
	}
	// Снимаем СТРОГО свои хендлы и строго со своим targetname. Постоянная подсветка старта и
	// финиша живёт в другом хранилище (kz_zones.cpp) и под другим именем — этот код физически не
	// может её погасить, и это главное требование к паре слоёв.
	KZ::zones::RemoveBoxEdges(this->showBeams.data(), (i32)this->showBeams.size(), KZ_ZONE_SHOW_NAME, keepEntities);
	this->showBeams.clear();
}

void KZZonesService::ShowAllZones()
{
	// Повторный вызов — выключатель. Гасит ТОЛЬКО показ: постоянный контур старта и финиша не
	// его, он остаётся на месте.
	if (!this->showBeams.empty())
	{
		this->ClearShow();
		this->player->PrintChat(true, false, "{grey}Зоны:{default} показ выключен.");
		return;
	}
	if (!KZ::zones::IsReady())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} набор карты ещё не загружен из api.");
		return;
	}
	const std::vector<KzCyberZone> &zones = KZ::zones::Loaded();
	if (zones.empty())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} на этой карте наших зон нет.");
		return;
	}

	i32 drawn = 0;
	i32 skipped = 0;
	for (const KzCyberZone &zone : zones)
	{
		if (drawn >= KZ_ZONE_HIGHLIGHT_MAX_ZONES)
		{
			skipped++;
			continue;
		}
		// Бокс раздут на пол-юнита: у старта и финиша поверх лежит постоянный контур, и
		// точно совпадающие рёбра мерцали бы z-fighting'ом. «Показать всё» показывает и то, что
		// уже подсвечено, — иначе команда врала бы своим названием.
		const Vector mins(zone.mins.x - KZ_ZONE_SHOW_INFLATE, zone.mins.y - KZ_ZONE_SHOW_INFLATE, zone.mins.z - KZ_ZONE_SHOW_INFLATE);
		const Vector maxs(zone.maxs.x + KZ_ZONE_SHOW_INFLATE, zone.maxs.y + KZ_ZONE_SHOW_INFLATE, zone.maxs.z + KZ_ZONE_SHOW_INFLATE);
		CEntityHandle edges[KZ_ZONE_BOX_EDGES] {};
		// ownerOnly=true: показ адресный, его видит только тот, кто позвал.
		KZ::zones::DrawBoxEdges(mins, maxs, KZ::zones::ZoneColor(zone.type), KZ_ZONE_SHOW_NAME, true, edges);
		for (CEntityHandle &edge : edges)
		{
			// Get() != nullptr — тот же способ проверки живого ребра, что у ztopwatch: ребро,
			// которое движок не отдал, в список не кладём, иначе показ считался бы удавшимся.
			if (edge.Get())
			{
				this->showBeams.push_back(edge);
			}
		}
		drawn++;
	}

	if (this->showBeams.empty())
	{
		// Ни одного ребра — движок не отдал энтити. Молчать здесь нельзя: игрок ждёт картинку.
		this->player->PrintChat(true, false, "{grey}Зоны:{default} не удалось нарисовать зоны.");
		return;
	}

	// Своё поколение: таймер от прошлого показа не должен гасить новый.
	const u32 generation = ++this->showGeneration;
	StartTimer<CPlayerSlot, u64, u32>(
		[](CPlayerSlot slot, u64 steamId, u32 generation) -> f64
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player && player->zonesService && player->zonesService->showGeneration == generation)
			{
				player->zonesService->ClearShow();
			}
			return -1.0;
		},
		this->player->GetPlayerSlot(), this->player->GetSteamId64(false), generation, KZ_ZONE_SHOW_SECONDS, false);

	if (skipped > 0)
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} показываю {yellow}%d{default} зон(ы) на %.0f сек, ещё %d не влезло в потолок.",
								drawn, KZ_ZONE_SHOW_SECONDS, skipped);
	}
	else
	{
		this->player->PrintChat(true, false,
								"{grey}Зоны:{default} показываю {yellow}%d{default} зон(ы) на %.0f секунд. Повтори команду, чтобы убрать.", drawn,
								KZ_ZONE_SHOW_SECONDS);
	}
}

// Превью автора: рёбра помечены как плагинные (ownerOnly), поэтому их видит только он — и только
// потому, что KZZonesService::OwnsParticle назван в белом списке KZ::quiet::OnCheckTransmit. Без
// этой записи метка означала бы «не видит никто», включая самого редактора.
void KZZonesService::DrawBox(const Vector &mins, const Vector &maxs, Color color, f32 duration)
{
	this->ClearPreview();
	KZ::zones::DrawBoxEdges(mins, maxs, color, KZ_ZONE_PREVIEW_NAME, true, this->previewBeams);
	this->previewActive = true;

	// Превью живёт ограниченное время: беамы висят до ручного снятия, а игрок про них забудет.
	// CTimer::Fn — сырой указатель на функцию, захватывающая лямбда в него не приводится.
	// Поэтому слот, steam_id и поколение уезжают параметрами самого таймера.
	const u32 generation = ++this->previewGeneration;
	StartTimer<CPlayerSlot, u64, u32>(
		[](CPlayerSlot slot, u64 steamId, u32 generation) -> f64
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			// Гасим только своё поколение: за 15 секунд игрок мог поставить вторую зону, и
			// таймер от первой не должен стирать её превью.
			if (player && player->zonesService && player->zonesService->previewGeneration == generation)
			{
				player->zonesService->ClearPreview();
			}
			return -1.0;
		},
		this->player->GetPlayerSlot(), this->player->GetSteamId64(false), generation, duration, false);
}

void KZZonesService::BeginOrFinish(KzCyberZoneType type, f32 jumpFactor)
{
	if (!this->EnsureAllowed())
	{
		return;
	}
	if (!KZ::zones::IsReady())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} набор карты ещё не загружен из api.");
		return;
	}
	if (!this->player->GetPlayerPawn())
	{
		return;
	}

	Vector origin;
	this->player->GetOrigin(&origin);

	if (!this->hasPendingCorner)
	{
		this->hasPendingCorner = true;
		this->pendingType = type;
		this->pendingJumpFactor = jumpFactor;
		this->pendingCorner = origin;
		this->player->PrintChat(true, false,
								"{grey}Зоны:{default} угол A поставлен. Отойди и повтори команду, чтобы закрыть зону. {grey}!zone cancel — сброс.");
		return;
	}

	if (this->pendingType != type)
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} начата зона {yellow}%s{default} — закрой её или сбрось {grey}!zone cancel.",
								KZ::zones::ZoneTypeToString(this->pendingType));
		return;
	}

	KzCyberZone zone {};
	zone.type = type;
	zone.jumpFactor = this->pendingJumpFactor;
	zone.mins.x = MIN(this->pendingCorner.x, origin.x);
	zone.mins.y = MIN(this->pendingCorner.y, origin.y);
	zone.mins.z = MIN(this->pendingCorner.z, origin.z);
	zone.maxs.x = MAX(this->pendingCorner.x, origin.x);
	zone.maxs.y = MAX(this->pendingCorner.y, origin.y);
	// Высоту добавляет РЕДАКТОР: обе точки взяты с пола, и без этого по оси Z вышел бы ноль —
	// api отбивает такой бокс как degenerate_box, а игрок не понял бы, почему.
	zone.maxs.z = MAX(this->pendingCorner.z, origin.z) + KZ_ZONE_EDITOR_HEIGHT;

	// Те же пороги, что в контракте: незачем гонять заведомо отказной запрос по сети.
	if (zone.maxs.x - zone.mins.x < KZ_ZONE_MIN_SIZE || zone.maxs.y - zone.mins.y < KZ_ZONE_MIN_SIZE)
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} слишком узко — минимум %.0f юнитов по каждой оси. Отойди дальше.",
								KZ_ZONE_MIN_SIZE);
		return;
	}
	const f32 coords[6] = {zone.mins.x, zone.mins.y, zone.mins.z, zone.maxs.x, zone.maxs.y, zone.maxs.z};
	for (f32 c : coords)
	{
		if (fabsf(c) > KZ_ZONE_WORLD_LIMIT)
		{
			this->player->PrintChat(true, false, "{grey}Зоны:{default} координата вне мира.");
			return;
		}
	}

	// Угол A забываем только когда бокс действительно принят: иначе слишком узкий второй угол
	// молча терял бы начатую зону, хотя текст обещает «отойди дальше и повтори».
	this->hasPendingCorner = false;
	this->DrawBox(zone.mins, zone.maxs, Color(0, 200, 255, 255), KZ_ZONE_PREVIEW_SECONDS);
	this->SubmitZone(zone);
}

void KZZonesService::SubmitZone(const KzCyberZone &zone)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		return;
	}
	const u64 steamId = this->player->GetSteamId64(false);

	// Алфавит имени карты у ЗОН свой: ^[a-z0-9_]{1,64}$ (kzMapNameSchema в контракте). Брать
	// IsValidMapName реплеев нельзя — там [a-z0-9_-]{1,128}, и карта с дефисом прошла бы
	// локально, а api ответил бы телом валидатора без поля reason: игрок увидел бы
	// бессмысленное «api отказал: http_error».
	const std::string mapName = KZ::zones::CurrentMapName();
	if (!KZ::zones::IsValidZoneMapName(mapName))
	{
		KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_rejected steam_id=%llu map=%s reason=invalid_map_name\n", steamId, mapName.c_str());
		this->player->PrintChat(true, false, "{grey}Зоны:{default} на этой карте зоны не поддерживаются (имя карты вне алфавита api).");
		return;
	}

	char body[768];
	if (zone.type == KZ_CYBER_ZONE_MODIFIER)
	{
		V_snprintf(body, sizeof(body),
				   "{\"steamId64\":\"%llu\",\"map\":\"%s\",\"triggerType\":\"%s\",\"mins\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
				   "\"maxs\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"params\":{\"jumpFactor\":%.3f}}",
				   steamId, KZ::zones::CurrentMapName(), KZ::zones::ZoneTypeToString(zone.type), zone.mins.x, zone.mins.y, zone.mins.z, zone.maxs.x,
				   zone.maxs.y, zone.maxs.z, zone.jumpFactor);
	}
	else
	{
		V_snprintf(body, sizeof(body),
				   "{\"steamId64\":\"%llu\",\"map\":\"%s\",\"triggerType\":\"%s\",\"mins\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
				   "\"maxs\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}",
				   steamId, KZ::zones::CurrentMapName(), KZ::zones::ZoneTypeToString(zone.type), zone.mins.x, zone.mins.y, zone.mins.z, zone.maxs.x,
				   zone.maxs.y, zone.maxs.z);
	}

	HTTP::Request req(HTTP::Method::POST, base + "/ingest/v1/kz/zones");
	AuthorizeRequest(req);
	req.SetHeader("Content-Type", "application/json");
	req.SetBody(body);

	const CPlayerSlot slot = this->player->GetPlayerSlot();
	const KzCyberZone pending = zone;
	// Карта могла смениться, пока запрос летел: без этой сверки зона со старыми координатами
	// уехала бы в набор НОВОЙ карты и переспавнивалась бы там каждый раунд.
	const std::string submittedMap = KZ::zones::CurrentMapName();

	// clang-format off
	req.Send(
		[slot, steamId, pending, submittedMap](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			std::optional<std::string> body = resp.Body();
			if (resp.status < 200 || resp.status >= 300)
			{
				// Отказ api приезжает как {"reason": "..."} — показываем причину, а не «не вышло».
				std::string reason = ParseApiReason(body);
				if (reason.empty())
				{
					reason = "http_error";
				}
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_rejected steam_id=%llu reason=%s http=%u\n", steamId, reason.c_str(),
							(unsigned)resp.status);
				if (player)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал: {darkred}%s{default}.", reason.c_str());
				}
				return;
			}
			if (!KZ_STREQ(submittedMap.c_str(), KZ::zones::CurrentMapName()))
			{
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_stale steam_id=%llu map=%s reason=map_changed\n", steamId,
							submittedMap.c_str());
				return;
			}
			// Ответ api: {ok, zone, revision, replaced}. id лежит в zone.id, а не на корне.
			KzCyberZone stored = pending;
			i32 revision = 0;
			if (body.has_value())
			{
				KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
				CUtlString error = "";
				LoadKV3FromJSON(&kv, &error, body->c_str(), "");
				if (error.IsEmpty())
				{
					KeyValues3 *zoneMember = kv.FindMember("zone");
					KeyValues3 *id = zoneMember ? zoneMember->FindMember("id") : kv.FindMember("id");
					if (id)
					{
						V_snprintf(stored.id, sizeof(stored.id), "%s", id->GetString(""));
					}
					KeyValues3 *rev = kv.FindMember("revision");
					if (rev)
					{
						revision = (i32)rev->GetDouble(0.0);
					}
					// start/end у курса ровно по одному: api soft-удаляет прежнюю зону и
					// возвращает её id в replaced[]. Без этого на карте остались бы ДВА
					// старта — таймер начинал бы отсчёт не там, ради чего всё и делалось.
					KeyValues3 *replaced = kv.FindMember("replaced");
					if (replaced && replaced->GetType() == KV3_TYPE_ARRAY)
					{
						for (int i = 0; i < replaced->GetArrayElementCount(); i++)
						{
							KeyValues3 *el = replaced->GetArrayElement(i);
							if (!el)
							{
								continue;
							}
							const char *replacedId = el->GetType() == KV3_TYPE_TABLE
														 ? (el->FindMember("id") ? el->FindMember("id")->GetString("") : "")
														 : el->GetString("");
							if (replacedId && replacedId[0] != '\0')
							{
								KZ::zones::RemoveById(replacedId);
							}
						}
					}
				}
			}
			stored.createdBy = steamId;
			const char *inactiveReason = KZ::zones::AddAndSpawn(stored, revision);
			if (player)
			{
				if (!inactiveReason)
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} зона {yellow}%s{default} поставлена и уже действует.",
									  KZ::zones::ZoneTypeToString(stored.type));
				}
				else if (KZ_STREQ(inactiveReason, "world_not_ready"))
				{
					player->PrintChat(true, false, "{grey}Зоны:{default} зона {yellow}%s{default} сохранена, включится в начале раунда.",
									  KZ::zones::ZoneTypeToString(stored.type));
				}
				else
				{
					// «Сохранено в api» и «работает на сервере» — разные вещи. Молчать об этом
					// нельзя: игрок пробежит через мёртвую зону и решит, что сломан таймер.
					player->PrintChat(true, false, "{grey}Зоны:{default} зона {yellow}%s{default} сохранена, но НЕ действует: {darkred}%s{default}.",
									  KZ::zones::ZoneTypeToString(stored.type), inactiveReason);
				}
			}
		},
		[slot, steamId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_add_rejected steam_id=%llu reason=network_error\n", steamId);
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, зона не сохранена.");
			}
		});
	// clang-format on
}

void KZZonesService::CancelPending()
{
	this->ClearPreview();
	if (!this->hasPendingCorner)
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} незакрытой зоны нет.");
		return;
	}
	this->hasPendingCorner = false;
	this->player->PrintChat(true, false, "{grey}Зоны:{default} незакрытая зона сброшена.");
}

void KZZonesService::ListZones()
{
	const std::vector<KzCyberZone> &zones = KZ::zones::Loaded();
	if (zones.empty())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} на карте {yellow}%s{default} наших зон нет.", KZ::zones::CurrentMapName());
		return;
	}
	this->player->PrintChat(true, false, "{grey}Зоны карты {yellow}%s{default} (revision %d):", KZ::zones::CurrentMapName(), KZ::zones::Revision());
	for (size_t i = 0; i < zones.size(); i++)
	{
		const KzCyberZone &z = zones[i];
		if (z.type == KZ_CYBER_ZONE_MODIFIER)
		{
			this->player->PrintChat(true, false, "  {yellow}%d{default}. %s x%.2f  (%.0f %.0f %.0f)", (i32)i + 1, KZ::zones::ZoneTypeToString(z.type),
									z.jumpFactor, z.mins.x, z.mins.y, z.mins.z);
		}
		else
		{
			this->player->PrintChat(true, false, "  {yellow}%d{default}. %s  (%.0f %.0f %.0f)", (i32)i + 1, KZ::zones::ZoneTypeToString(z.type),
									z.mins.x, z.mins.y, z.mins.z);
		}
	}
	this->player->PrintChat(true, false, "{grey}Удалить: {default}!zone remove <номер>");
}

// Сколько ВСЕГО попыток делаем на операцию с зоной. Два — то есть один повтор.
//
// 409 zone_moved это не отказ, а «состояние уехало между чтением и записью»: зону в момент
// операции перенесли в другой курс. Семантика отличается от соседей — 400 «так нельзя»,
// 404 «нет такой», а 409 «повтори». Без повтора редкая параллельная правка молча не применялась
// бы, и админ видел бы «ничего не произошло».
// Одного повтора достаточно: гонка требует одновременного редактирования ОДНОЙ зоны двумя
// людьми, и второй заход уже читает уехавшее состояние. Больше повторов означало бы цикл на
// сетевой ручке из игрового потока.
#define KZ_ZONE_API_ATTEMPTS 2

static_function void SendZoneDelete(CPlayerSlot slot, u64 steamId, std::string zoneId, i32 attempt);

static_function void SendZoneDelete(CPlayerSlot slot, u64 steamId, std::string zoneId, i32 attempt)
{
	const std::string base = ApiBaseUrl();
	if (base.empty())
	{
		// Молчаливый выход неотличим от сломанной команды. На постановке зоны про это говорят —
		// здесь было унаследованное расхождение.
		KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
		if (player)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} платформенный источник выключен.");
		}
		return;
	}

	char url[512];
	V_snprintf(url, sizeof(url), "%s/ingest/v1/kz/zones/%s", base.c_str(), zoneId.c_str());

	HTTP::Request req(HTTP::Method::DELETE_, url);
	AuthorizeRequest(req);
	// Актор — query-параметром: DELETE у нас идёт без тела (см. комментарий в http.h).
	char steamIdStr[32];
	V_snprintf(steamIdStr, sizeof(steamIdStr), "%llu", steamId);
	req.SetQuery("steamId64", steamIdStr);

	// clang-format off
	req.Send(
		[slot, steamId, zoneId, attempt](HTTP::Response resp)
		{
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			std::optional<std::string> delBody = resp.Body();

			if (resp.status == 409 && ParseApiReason(delBody) == "zone_moved" && attempt < KZ_ZONE_API_ATTEMPTS)
			{
				KZ_LOG_INFO(LogChannel::MappingAPI, "[cyb] zone_remove_retry steam_id=%llu id=%s attempt=%d reason=zone_moved\n", steamId,
							zoneId.c_str(), attempt);
				SendZoneDelete(slot, steamId, zoneId, attempt + 1);
				return;
			}

			if (resp.status < 200 || resp.status >= 300)
			{
				const std::string reason = ParseApiReason(delBody);
				KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_remove_rejected steam_id=%llu id=%s http=%u reason=%s attempt=%d\n", steamId,
							zoneId.c_str(), (unsigned)resp.status, reason.empty() ? "http_error" : reason.c_str(), attempt);
				if (player)
				{
					// Причина, а не код: reason уже разобран, а «409» админу ничего не говорит.
					// На постановке зоны причина показывается — здесь было расхождение.
					player->PrintChat(true, false, "{grey}Зоны:{default} api отказал в удалении: {darkred}%s{default}.",
									  reason.empty() ? "http_error" : reason.c_str());
				}
				return;
			}
			i32 revision = 0;
			if (delBody.has_value())
			{
				KeyValues3 kv(KV3_TYPEEX_TABLE, KV3_SUBTYPE_UNSPECIFIED);
				CUtlString error = "";
				LoadKV3FromJSON(&kv, &error, delBody->c_str(), "");
				KeyValues3 *rev = error.IsEmpty() ? kv.FindMember("revision") : nullptr;
				if (rev)
				{
					revision = (i32)rev->GetDouble(0.0);
				}
			}
			const bool removed = KZ::zones::RemoveById(zoneId.c_str(), revision);
			if (player)
			{
				player->PrintChat(true, false, removed ? "{grey}Зоны:{default} зона удалена." : "{grey}Зоны:{default} зона удалена в api.");
			}
		},
		[slot, steamId, zoneId]()
		{
			KZ_LOG_WARN(LogChannel::MappingAPI, "[cyb] zone_remove_rejected steam_id=%llu id=%s reason=network_error\n", steamId, zoneId.c_str());
			KZPlayer *player = PlayerBySlotIfSame(slot, steamId);
			if (player)
			{
				player->PrintChat(true, false, "{grey}Зоны:{default} api недоступен, зона не удалена.");
			}
		});
	// clang-format on
}

void KZZonesService::RemoveZone(i32 humanIndex)
{
	if (!this->EnsureAllowed())
	{
		return;
	}
	const std::vector<KzCyberZone> &zones = KZ::zones::Loaded();
	if (humanIndex < 1 || (size_t)humanIndex > zones.size())
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} нет зоны с номером {yellow}%d{default}. Смотри {grey}!zone list.", humanIndex);
		return;
	}
	const KzCyberZone target = zones[humanIndex - 1];
	if (target.id[0] == '\0')
	{
		this->player->PrintChat(true, false, "{grey}Зоны:{default} у зоны нет id — удалить через api нельзя.");
		return;
	}

	SendZoneDelete(this->player->GetPlayerSlot(), this->player->GetSteamId64(false), target.id, 1);
}

static_function void PrintUsage(KZPlayer *player)
{
	player->PrintChat(true, false, "{grey}Зоны:{default} !zone start | end | booster <множитель> | cancel | list | show | remove <номер>");
}

// SCFL_HIDDEN (то есть флагов нет вовсе) — команда не попадает НИ В ОДНУ таблицу !help:
// PrintCategoryCommands отбирает строки по `flags & (1 << категория)`, а нулевые флаги не
// совпадут ни с одной. Прежний SCFL_MAP выводил её в консольную таблицу категории «Map», причём
// сырым ключом вместо описания: ключа "Command Description - kz_zone" в translations/ нет, и
// игрок видел непонятную строку под командой, которой всё равно не может пользоваться.
// На вызываемость это не влияет: scmd::OnClientCommand матчит команду ТОЛЬКО по имени
// (simplecmds.cpp), флаги там не смотрят вовсе. Прецедент — SCMD(jointeam, SCFL_HIDDEN).
// Подсказку по подкомандам печатает сама команда без аргументов.
SCMD(kz_zone, SCFL_HIDDEN)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (!player->zonesService)
	{
		return MRES_SUPERCEDE;
	}
	if (args->ArgC() < 2)
	{
		PrintUsage(player);
		return MRES_SUPERCEDE;
	}
	const char *sub = args->Arg(1);

	if (KZ_STREQI(sub, "start"))
	{
		player->zonesService->BeginOrFinish(KZ_CYBER_ZONE_START, 1.0f);
	}
	else if (KZ_STREQI(sub, "end"))
	{
		player->zonesService->BeginOrFinish(KZ_CYBER_ZONE_END, 1.0f);
	}
	else if (KZ_STREQI(sub, "booster"))
	{
		f32 factor = args->ArgC() > 2 ? (f32)atof(args->Arg(2)) : 0.0f;
		if (factor <= 0.0f || factor > 10.0f)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} множитель прыжка — число от 0 до 10, например {yellow}!zone booster 1.35{default}.");
			return MRES_SUPERCEDE;
		}
		player->zonesService->BeginOrFinish(KZ_CYBER_ZONE_MODIFIER, factor);
	}
	else if (KZ_STREQI(sub, "cancel"))
	{
		player->zonesService->CancelPending();
	}
	else if (KZ_STREQI(sub, "list"))
	{
		player->zonesService->ListZones();
	}
	else if (KZ_STREQI(sub, "show"))
	{
		// Права НЕ спрашиваем: команда только смотрит. Проверка стоила бы запроса в api и кэша,
		// а прятать нечего — контур старта и финиша и так видно всем без всякой команды.
		player->zonesService->ShowAllZones();
	}
	else if (KZ_STREQI(sub, "remove"))
	{
		if (args->ArgC() < 3)
		{
			player->PrintChat(true, false, "{grey}Зоны:{default} !zone remove <номер из !zone list>");
			return MRES_SUPERCEDE;
		}
		player->zonesService->RemoveZone(atoi(args->Arg(2)));
	}
	else
	{
		PrintUsage(player);
	}
	return MRES_SUPERCEDE;
}
