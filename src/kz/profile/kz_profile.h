#pragma once
#include "../kz.h"

class KZProfileService : public KZBaseService
{
public:
	using KZBaseService::KZBaseService;

	// Флаш склеенной пачки «раскрыть ранги», а не периодическая рассылка: без изменений
	// рангов не шлёт ничего.
	static void OnGameFrame();
	static void OnCheckTransmit();
	// Рестарт раунда пересобирает клиентский худ — переутверждаем раскрытие рангов.
	static void OnRoundStart();
	// Заход на сервер и каждая смена карты: адресное раскрытие рангов этому клиенту.
	void OnPlayerActive();

	virtual void Reset() override
	{
		clanTag[0] = '\0';
		desiredMode[0] = '\0';
		timeToNextRatingRefresh = 0.0f;
		currentPoints = -1;
		clantagOverrideApplied = false;
		personalTag[0] = '\0';
		personalTagChatColor[0] = '\0';
		personalTagRequested = false;
		personalTagRetryTime = 0.0f;
		personalTagRetryUsed = false;
	}

	char clanTag[32] {};
	// api-режим ("kzt"/"ckz"/"vnl") на момент последнего запроса очков: ответ,
	// прилетевший после смены режима, отбрасывается сравнением с ним.
	char desiredMode[8] {};
	f32 timeToNextRatingRefresh = 0.0f;
	// Платформенные NUB-очки текущего режима; -1 = не загружено (тэг без звания).
	i32 currentPoints = -1;
	// Персональный тег уже проставлен в этой жизни игрока. Нужен, потому что штатно тег
	// перерисовывается только по ответу платформы об очках, а он у такого игрока не
	// обязателен (сеть легла, включены стили — запрос очков вообще не уходит).
	bool clantagOverrideApplied = false;
	// Персональная приписка у ника с платформы: ГОЛЫЙ текст без скобок (скобки ставит формат
	// подстановки) и уже прошедший санитайз форка. Пусто = приписки нет, рисуем ранговый тег.
	// 32 байта, хотя платформа режет до 24: буфер clanTag того же размера, а формат "[%s]"
	// съедает ещё два — вписаться обязан сам snprintf, а не наша вера в лимит api.
	char personalTag[32] {};
	// Имя цветового токена чата для приписки, УЖЕ сверенное с закрытым списком движка
	// (utils::IsChatColorName). Пусто = дефолт. Скорборд цвет клан-тега не передаёт вовсе.
	char personalTagChatColor[16] {};
	// Приписку в этой сессии уже запрашивали (кэш на сессию). Повтор — на реконнекте: Reset
	// зовётся на дисконнекте. Снятие из админки применяется по следующему заходу, так и
	// задумано.
	bool personalTagRequested = false;
	// realtime отложенного ПОВТОРА запроса приписки; 0 = повтор не запланирован. Нужен потому,
	// что транзиентный отказ (сетевой блип ровно на коннекте, рестарт api, 5xx) иначе оставлял
	// бы игрока без приписки до реконнекта — а прежний хардкод отказать не мог вовсе.
	f32 personalTagRetryTime = 0.0f;
	// Повтор уже давали. Повтор РОВНО один: на лежащем api ретраи каждые пять секунд от каждого
	// игрока — это спам в лог и в сеть, а приписка не гейт игры.
	bool personalTagRetryUsed = false;

	void RequestRating();
	// Персональная приписка у ника: GET {cybEmitUrl}/ingest/v1/players/<steamid64>/tag
	// (ServerTokenGuard, ответ {"tag": {...}|null}). Fail-soft: любой отказ = приписки нет,
	// ранговый тег работает как раньше. Зовётся на аутентификации, на OnPlayerActive (страховка
	// late-load без колбэков авторизации) и из такта OnPhysicsSimulatePost — единственным
	// отложенным повтором после транзиентного отказа. Лишние вызовы гасит personalTagRequested.
	void RequestPersonalTag();
	bool CanDisplayRank();
	// Индекс звания 0..22 по текущим очкам и шкале режима; -1 = звание недоступно.
	i32 GetCurrentRankIndex();
	// Финиш рана: платформа пересчитает очки после инджеста — подтянуть с малым лагом.
	void OnRunFinished();
	// Мост режима для GG1: серверная команда `cyb_gg1_mode <steamid64> <kzt|ckz|vnl>`.
	void EmitGG1Bridge();
	// Ответ на !rank: звание, очки, до следующего звания.
	void PrintRank();

	void SetClantag(const char *clanTag)
	{
		V_strncpy(this->clanTag, clanTag, sizeof(this->clanTag));
		this->player->SetClan(clanTag);
	}

	void UpdateClantag();
	void OnPhysicsSimulatePost();
	void UpdateCompetitiveRank();
	std::string GetPrefix(bool colors = true);
};
