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
	}

	char clanTag[32] {};
	// api-режим ("kzt"/"ckz"/"vnl") на момент последнего запроса очков: ответ,
	// прилетевший после смены режима, отбрасывается сравнением с ним.
	char desiredMode[8] {};
	f32 timeToNextRatingRefresh = 0.0f;
	// Платформенные NUB-очки текущего режима; -1 = не загружено (тэг без звания).
	i32 currentPoints = -1;

	void RequestRating();
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
