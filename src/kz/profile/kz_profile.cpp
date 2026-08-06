#include "kz_profile.h"
#include "utils/http.h"
#include "utils/json.h"
#include "utils/simplecmds.h"
#include "kz/anticheat/kz_anticheat.h"
#include "kz/language/kz_language.h"
#include "kz/mode/kz_mode.h"
#include "kz/style/kz_style.h"
#include "kz/option/kz_option.h"
#include "kz/replays/cyb_replay_common.h" // MapMode — общий маппинг шорт-нейма режима в api ("kzt"/"ckz"/"vnl")

#include "sdk/recipientfilters.h"
#include "public/networksystem/inetworkmessages.h"

// Лестница из 23 званий по платформенным NUB-очкам. Значения — из брифа
// «Константы званий» (калибровка 2026-07-30); копия для api/web —
// packages/contracts/src/kz/ranks.ts, синхронизация версией профиля.
#define KZ_RANK_COUNT 23

// clang-format off
static_global const char *rankNames[KZ_RANK_COUNT] = {
	"New",      "Beginner-", "Beginner", "Beginner+",
	"Amateur-", "Amateur",   "Amateur+",
	"Casual-",  "Casual",    "Casual+",
	"Regular-", "Regular",   "Regular+",
	"Skilled-", "Skilled",   "Skilled+",
	"Expert-",  "Expert",    "Expert+",
	"Semipro",  "Pro",       "Master",   "Legend",
};

// {lime} — ближайший чат-токен к lightgreen из брифа (такого токена в utils_print нет).
static_global const char *rankColors[KZ_RANK_COUNT] = {
	"{grey}",     "{default}",  "{default}", "{default}",
	"{blue}",     "{blue}",     "{blue}",
	"{lime}",     "{lime}",     "{lime}",
	"{green}",    "{green}",    "{green}",
	"{purple}",   "{purple}",   "{purple}",
	"{orchid}",   "{orchid}",   "{orchid}",
	"{lightred}", "{lightred}", "{red}",     "{gold}",
};

// Пороги «с этого значения». KZT и CKZ — общая шкала; VNL сжата под малый пул карт.
static_global const i32 rankThresholdsKztCkz[KZ_RANK_COUNT] = {
	0,     1,     250,   500,   1000,  1500,  2000,  3000,  4000,  5000,  6500,  8000,
	10000, 12500, 15000, 17500, 20000, 25000, 30000, 35000, 42500, 50000, 60000,
};

static_global const i32 rankThresholdsVnl[KZ_RANK_COUNT] = {
	0,    1,    100,  200,  300,  450,  600,  800,  1000, 1250, 1500, 1750,
	2000, 2400, 2800, 3200, 3600, 4000, 4400, 4800, 5200, 5600, 6000,
};

// clang-format on

// Шкала порогов по api-режиму; nullptr — режим без званий.
static_function const i32 *GetRankThresholds(const char *apiMode)
{
	if (KZ_STREQ(apiMode, "vnl"))
	{
		return rankThresholdsVnl;
	}
	if (KZ_STREQ(apiMode, "kzt") || KZ_STREQ(apiMode, "ckz"))
	{
		return rankThresholdsKztCkz;
	}
	return nullptr;
}

static_function i32 GetRankIndexForPoints(const i32 *thresholds, i32 points)
{
	i32 rank = 0;
	for (i32 i = KZ_RANK_COUNT - 1; i >= 0; i--)
	{
		if (points >= thresholds[i])
		{
			rank = i;
			break;
		}
	}
	return rank;
}

#define RATING_REFRESH_PERIOD 120.0f // seconds
// Грейс после финиша рана: платформа должна успеть принять и посчитать ран.
#define RATING_RUN_FINISH_DELAY 5.0f // seconds
// Ретрай после раннего выхода «не сейчас» (нет аутентификации/режима/стили):
// без сдвига таймера OnPhysicsSimulatePost долбил бы RequestRating каждый тик
// (реплей-боты не аутентифицированы никогда).
#define RATING_RETRY_PERIOD 5.0f // seconds

CConVar<bool> kz_profile_rating_badge_enabled("kz_profile_rating_badge_enabled", FCVAR_NONE, "Whether to show competitive rank in scoreboard.", true);
// Мост режима для GG1 (map chooser): до его релиза команды-приёмника нет,
// эмит тогда молча пропускается по FindConCommand (см. EmitGG1Bridge).
CConVar<bool> kz_gg1_bridge("kz_gg1_bridge", FCVAR_NONE, "Whether to broadcast player mode to GG1 via cyb_gg1_mode server command.", true);

void KZProfileService::OnGameFrame()
{
	// Гейт тем же cvar'ом, что и сам значок: сообщение существует ровно ради раскрытия
	// рангов в скорборде — при выключенном значке это чистый бродкаст всем 2 раза в секунду.
	// Заодно даёт разведку одной командой: `kz_profile_rating_badge_enabled 0` гасит ВЕСЬ
	// ранго-скорбордный тракт (и записи в контроллеры, и этот бродкаст) — если TAB при этом
	// перестаёт дёргаться, виновник здесь, а не в скрытии невидимок (репорт 05.08).
	if (!kz_profile_rating_badge_enabled.Get())
	{
		return;
	}
	if (g_pKZUtils->GetServerGlobals()->tickcount % 64 != 0)
	{
		return;
	}
	CBroadcastRecipientFilter filter;
	INetworkMessageInternal *netmsg = g_pNetworkMessages->FindNetworkMessagePartial("CCSUsrMsg_ServerRankRevealAll");
	CNetMessage *msg = netmsg->AllocateMessage();
	interfaces::pGameEventSystem->PostEventAbstract(0, false, &filter, netmsg, msg, 0);
	delete msg;
}

void KZProfileService::OnCheckTransmit()
{
	if (!kz_profile_rating_badge_enabled.Get())
	{
		return;
	}
	for (i32 i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (player && player->IsInGame())
		{
			player->profileService->UpdateCompetitiveRank();
		}
	}
}

// Источник очков — НАША платформа (тот же корень, что PB/WR-кэш худа и эмиттер
// ранов), а не cs2kz.org: гейт KZGlobalService::IsAvailable из апстрима снят.
void KZProfileService::RequestRating()
{
	// Бэкофф ставится ДО ранних выходов: любой выход «не сейчас» = ретрай через
	// RATING_RETRY_PERIOD, успешный путь ниже перепишет на полный период.
	// GetName() в этих ветках не использовать — он аллоцирует (CUtlString+Trim).
	this->timeToNextRatingRefresh = g_pKZUtils->GetServerGlobals()->realtime + RATING_RETRY_PERIOD;
	if (!this->player->IsAuthenticated() || !this->player->IsConnected())
	{
		KZ_LOG_DEBUG(LogChannel::Profile, "Slot %d not authenticated or not connected, cannot request rating.\n",
					 this->player->GetPlayerSlot().Get());
		return;
	}
	const char *apiMode = CybReplayCommon::MapMode(this->player->modeService->GetModeShortName());
	if (apiMode[0] == '\0')
	{
		KZ_LOG_DEBUG(LogChannel::Profile, "Slot %d has non-platform mode '%s', cannot request rating.\n", this->player->GetPlayerSlot().Get(),
					 this->player->modeService->GetModeShortName());
		return;
	}
	u64 steamID64 = this->player->GetSteamId64();
	if (steamID64 == 0)
	{
		KZ_LOG_DEBUG(LogChannel::Profile, "Slot %d has invalid SteamID, cannot request rating.\n", this->player->GetPlayerSlot().Get());
		return;
	}
	if (this->player->styleServices.Count() > 0)
	{
		KZ_LOG_DEBUG(LogChannel::Profile, "Slot %d has styles enabled, skipping rating request.\n", this->player->GetPlayerSlot().Get());
		return;
	}
	this->timeToNextRatingRefresh = g_pKZUtils->GetServerGlobals()->realtime + RATING_REFRESH_PERIOD + RandomFloat(-30.0f, 30.0f);
	std::string apiURL = std::string(KZOptionService::GetOptionStr("cybEmitUrl", ""));
	if (apiURL.empty())
	{
		return; // платформа не сконфигурирована — играем без званий, не отказ
	}
	// Removing trailing slash if present to avoid double slashes in the URL.
	if (apiURL.back() == '/')
	{
		apiURL.pop_back();
	}
	V_strncpy(this->desiredMode, apiMode, sizeof(this->desiredMode));
	std::string url = apiURL + "/v1/kz/ranking/player/" + std::to_string(steamID64);
	HTTP::Request request(HTTP::Method::GET, url);
	request.SetQuery("mode", apiMode);
	request.SetQuery("category", "nub");
	std::string modeStr = apiMode;
	KZ_LOG_DEBUG(LogChannel::Profile, "Requesting rating for player %s (%llu) in mode %s.\n", this->player->GetName(), steamID64, apiMode);
	auto onResponse = [steamID64, modeStr](HTTP::Response response)
	{
		if (response.status < 200 || response.status >= 300)
		{
			KZ_LOG_WARN(LogChannel::Profile, "[cyb] rank_fetch_fail steam_id=%llu reason=http_%u\n", steamID64, (unsigned)response.status);
			return;
		}
		KZPlayer *player = g_pKZPlayerManager->SteamIdToPlayer(steamID64);
		if (player == nullptr)
		{
			return; // игрок вышел, пока запрос летел
		}
		// The player mode has changed since the request was made.
		if (!KZ_STREQ(player->profileService->desiredMode, modeStr.c_str()))
		{
			KZ_LOG_DEBUG(LogChannel::Profile, "Player %s mode changed since request, ignoring response.\n", player->GetName());
			return;
		}
		std::string body = response.Body().value_or("");
		// Пустое тело при 200 — аномалия (обрыв/прокси), НЕ «нет очков»: молча
		// принять его за ноль = fail-open, Master внезапно станет New.
		if (body.empty())
		{
			KZ_LOG_WARN(LogChannel::Profile, "[cyb] rank_fetch_fail steam_id=%llu reason=bad_body\n", steamID64);
			return;
		}
		i32 points = 0;
		// api отвечает строкой null, если у игрока ещё нет ни одного рана — это
		// успешно загруженный ноль очков (звание New), а не отказ.
		if (body != "null")
		{
			Json json(body);
			f64 pointsF = 0.0;
			if (!json.IsValid() || !json.Get("points", pointsF))
			{
				KZ_LOG_WARN(LogChannel::Profile, "[cyb] rank_fetch_fail steam_id=%llu reason=bad_body\n", steamID64);
				return;
			}
			points = static_cast<i32>(pointsF);
		}
		const i32 *thresholds = GetRankThresholds(modeStr.c_str());
		i32 oldPoints = player->profileService->currentPoints;
		player->profileService->currentPoints = points;
		// Смена звания (не первая загрузка) — единственный info-лог тракта.
		if (thresholds && oldPoints >= 0)
		{
			i32 oldRank = GetRankIndexForPoints(thresholds, oldPoints);
			i32 newRank = GetRankIndexForPoints(thresholds, points);
			if (oldRank != newRank)
			{
				KZ_LOG_INFO(LogChannel::Profile, "[cyb] rank_change steam_id=%llu mode=%s rank=%s points=%d\n", steamID64, modeStr.c_str(),
							rankNames[newRank], points);
			}
		}
		player->profileService->UpdateCompetitiveRank();
		player->profileService->UpdateClantag();
	};
	auto onError = [steamID64]() { KZ_LOG_WARN(LogChannel::Profile, "[cyb] rank_fetch_fail steam_id=%llu reason=network\n", steamID64); };
	request.Send(onResponse, onError);
}

bool KZProfileService::CanDisplayRank()
{
	// Haven't obtained points yet.
	if (this->currentPoints < 0)
	{
		return false;
	}
	// No rank if styles are enabled.
	if (this->player->styleServices.Count() > 0)
	{
		return false;
	}
	// No rank if player is banned.
	if (this->player->anticheatService->isBanned)
	{
		return false;
	}
	return CybReplayCommon::MapMode(this->player->modeService->GetModeShortName())[0] != '\0';
}

i32 KZProfileService::GetCurrentRankIndex()
{
	if (!this->CanDisplayRank())
	{
		return -1;
	}
	const i32 *thresholds = GetRankThresholds(CybReplayCommon::MapMode(this->player->modeService->GetModeShortName()));
	if (!thresholds)
	{
		return -1;
	}
	return GetRankIndexForPoints(thresholds, this->currentPoints);
}

void KZProfileService::OnRunFinished()
{
	f32 refreshAt = g_pKZUtils->GetServerGlobals()->realtime + RATING_RUN_FINISH_DELAY;
	if (this->timeToNextRatingRefresh > refreshAt)
	{
		this->timeToNextRatingRefresh = refreshAt;
	}
}

void KZProfileService::EmitGG1Bridge()
{
	if (!kz_gg1_bridge.Get())
	{
		return;
	}
	// Живой в сессии игрок. IsInGame (signon == FULL) отсекает ранний OnAuthorized —
	// Steam обычно подтверждает тикет, пока клиент ещё качает карту; такой эмит GG1
	// не нужен, мост уйдёт из OnPlayerActive. Но дисконнект IsInGame НЕ ловит (клиент
	// остаётся в SIGNONSTATE_FULL до конца) — его отсекает состояние контроллера,
	// тот же паттерн, что в UpdateClantag.
	if (!this->player->IsInGame() || !this->player->GetController()
		|| this->player->GetController()->m_iConnected() != PlayerConnectedState::PlayerConnected)
	{
		return;
	}
	u64 steamID64 = this->player->GetSteamId64();
	if (steamID64 == 0)
	{
		return;
	}
	const char *apiMode = CybReplayCommon::MapMode(this->player->modeService->GetModeShortName());
	if (apiMode[0] == '\0')
	{
		return;
	}
	// Приёмник — GG1 (CSSharp), появится отдельным релизом (задача D1). Пока команда
	// не зарегистрирована, эмит пропускаем: слепой ServerCommand печатал бы
	// "Unknown command" в консоль на каждый заход/смену режима.
	if (!g_pCVar || !g_pCVar->FindConCommand("cyb_gg1_mode").IsValidRef())
	{
		KZ_LOG_DEBUG(LogChannel::Profile, "cyb_gg1_mode is not registered, skipping GG1 bridge emit.\n");
		return;
	}
	char cmd[64];
	V_snprintf(cmd, sizeof(cmd), "cyb_gg1_mode %llu %s", steamID64, apiMode);
	interfaces::pEngine->ServerCommand(cmd);
}

void KZProfileService::PrintRank()
{
	const char *apiMode = CybReplayCommon::MapMode(this->player->modeService->GetModeShortName());
	if (apiMode[0] == '\0')
	{
		this->player->languageService->PrintChat(true, false, "Rank - Not Available (Mode)");
		return;
	}
	if (this->player->styleServices.Count() > 0)
	{
		this->player->languageService->PrintChat(true, false, "Rank - Disabled (Styles)");
		return;
	}
	i32 rank = this->GetCurrentRankIndex();
	if (rank < 0)
	{
		this->player->languageService->PrintChat(true, false, "Rank - Not Loaded");
		return;
	}
	if (rank >= KZ_RANK_COUNT - 1)
	{
		this->player->languageService->PrintChat(true, false, "Rank - Info (Max)", rankColors[rank], rankNames[rank], this->currentPoints);
		return;
	}
	const i32 *thresholds = GetRankThresholds(apiMode);
	this->player->languageService->PrintChat(true, false, "Rank - Info", rankColors[rank], rankNames[rank], this->currentPoints,
											 thresholds[rank + 1] - this->currentPoints, rankColors[rank + 1], rankNames[rank + 1]);
}

void KZProfileService::UpdateClantag()
{
	if (!this->player->IsConnected()
		|| (this->player->GetController() && this->player->GetController()->m_iConnected() != PlayerConnectedState::PlayerConnected))
	{
		if (this->player->GetController() && V_strlen(this->player->GetController()->m_szClan().String()) > 0)
		{
			this->SetClantag("");
		}
		return;
	}
	i32 rank = this->GetCurrentRankIndex();
	if (rank >= 0)
	{
		V_snprintf(this->clanTag, sizeof(this->clanTag), "[%s %s]", this->player->modeService->GetModeShortName(), rankNames[rank]);
	}
	else
	{
		V_snprintf(this->clanTag, sizeof(this->clanTag), "[%s%s]", this->player->modeService->GetModeShortName(),
				   this->player->styleServices.Count() > 0 ? "*" : "");
	}

	if (this->clanTag[0] == '\0')
	{
		return;
	}
	this->SetClantag(this->clanTag);
}

void KZProfileService::OnPhysicsSimulatePost()
{
	if (g_pKZUtils->GetServerGlobals()->realtime >= this->timeToNextRatingRefresh)
	{
		this->RequestRating();
	}
}

void KZProfileService::UpdateCompetitiveRank()
{
	if (!this->player->IsInGame() || !kz_profile_rating_badge_enabled.GetBool() || !this->player->GetController())
	{
		return;
	}
	// Цифра в скорборде = платформенные NUB-очки режима.
	i32 rating = this->CanDisplayRank() ? this->currentPoints : 0;
	CCSPlayerController *controller = this->player->GetController();
	// Пишем ТОЛЬКО по разнице (идиома kz_fov.cpp): SCHEMA-сеттер безусловно зовёт
	// NetworkStateChanged, а метод крутится на КАЖДОМ CheckTransmit — апстрим помечал
	// контроллеры всех игроков грязными 128 раз в секунду одним и тем же значением.
	if (controller->m_iCompetitiveRankType() != 11)
	{
		controller->m_iCompetitiveRankType(11);
	}
	if (controller->m_iCompetitiveRanking() != rating)
	{
		controller->m_iCompetitiveRanking(rating);
	}
}

std::string KZProfileService::GetPrefix(bool colors)
{
	if (this->clanTag[0] == '\0')
	{
		this->UpdateClantag();
	}
	i32 rank = this->GetCurrentRankIndex();
	if (rank >= 0)
	{
		return std::string(colors ? rankColors[rank] : "") + "[" + this->player->modeService->GetModeShortName() + " " + rankNames[rank]
			   + (colors ? "]{default}" : "]");
	}
	else
	{
		return std::string(colors ? "{default}[" : "[") + this->player->modeService->GetModeShortName()
			   + (this->player->styleServices.Count() > 0 ? "*" : "") + (colors ? "]{default}" : "]");
	}
}

SCMD(kz_rank, SCFL_PLAYER | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->profileService->PrintRank();
	return MRES_SUPERCEDE;
}
