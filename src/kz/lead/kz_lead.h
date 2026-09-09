/*
 * kz_lead — `!lead`: луч по ногам сшитого маршрута (по умолчанию AWR), видимый ТОЛЬКО
 * вызвавшему игроку, в скользящем окне вокруг него.
 *
 * Почему окно, а не весь путь: в CS2 у серверных плагинов нет temp-entity лучей, каждый
 * отрезок — сетевая сущность (info_particle_system с ui_annotation_line_segment.vpcf, тот
 * же примитив, что у !measure и рёбер зон). Весь маршрут — это тысячи сущностей, окно —
 * десятки (потолок `cybLeadMaxSegments`, дефолт 96).
 *
 * Видимость только владельцу держится на KZ::quiet::OnCheckTransmit: метка
 * CUSTOM_PARTICLE_SYSTEM_TEAM означает «не видит никто», и обратно в белый список луч
 * возвращает именно OwnsParticle отсюда.
 *
 * Путь режим-зависим (спека §5.1): смена режима или карты гасит луч.
 */
#pragma once

#include "../kz.h"
#include "kz/replays/cyb_replay_download.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

class KZLeadService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	struct Vertex
	{
		Vector pos;
		bool onGround;
		// ЭФФЕКТИВНЫЙ серверный тик: мёртвые (вырезанные) интервалы в нём не занимают
		// времени. Именно поэтому окно «1 с назад / 6 с вперёд» не спотыкается о стык
		// разреза — по сырым индексам кадров там был бы прыжок на тысячи тиков и окно
		// вырождалось бы в один отрезок.
		u32 tickIdx;
	};

	// Разбор файла на рабочем потоке. Живёт на shared_ptr: игрок может уйти (сервис
	// удалён) раньше, чем поток закончит, — владение общее, use-after-free невозможен.
	struct PendingLoad
	{
		std::mutex mu;
		std::atomic<bool> done {false};
		// Поколение запроса: результат протухшей загрузки (успели выключить и включить
		// снова) отбрасывается на главном потоке.
		u32 generation = 0;
		std::vector<Vertex> path;
		const char *failReason = nullptr; // != nullptr — путь не построен
		const char *cutWarn = nullptr;    // разрез не сошёлся, путь без вырезки
	};

	virtual void Reset() override;

	// Смена карты. НЕ Reset: на выделенном сервере KZPlayer::Reset зовётся только с
	// дисконнекта (player_manager.cpp) и с late load (cs2kz.cpp), а Hook_StartupServer
	// игроков не сбрасывает — то же ограничение задокументировано у weapon/hud/zones.
	// Без этого после changelevel остался бы включённый луч с маршрутом ЧУЖОЙ карты, а
	// снятие пошло бы RemoveEntity по хендлам уже мёртвого мира. Сущности здесь не
	// удаляются: мира, которому они принадлежали, больше нет.
	static void OnMapChanged();

	// `!lead` / `!lead pb|wr|awr`: включить, если выключен, иначе выключить.
	void Toggle(CybReplayDownload::Kind kind);
	// `!lead off`, смена карты/режима, дисконнект, телепорт-петля данных.
	void Disable(const char *reason);
	// Байты файла из CybReplayDownload::RequestFile (главный поток, колбэк Steam HTTP):
	// отсюда стартует разбор на рабочем потоке. Пустой буфер — реплея нет.
	void OnFileReady(u32 gen, std::vector<char> &&bytes);
	// Готовый путь с рабочего потока (зовётся с ГЛАВНОГО потока, из PollPending).
	void OnPathLoaded(std::vector<Vertex> &&newPath);
	void OnPhysicsSimulatePost();

	void OnTeleport()
	{
		// Телепорт рвёт непрерывность движения — ближайшую вершину ищем заново по всему пути.
		this->resync = true;
	}

	bool OwnsParticle(const CEntityHandle &handle) const;

	bool HasOwnedParticles() const
	{
		return !this->segments.empty();
	}

private:
	void ResetState(bool keepEntities);
	void PollPending();
	void UpdateWindow();
	void UpdateNearest(const Vector &origin);
	void ApplyWindow(u32 newFrom, u32 newTo);
	void ClearSegments(bool keepEntities);
	void RebuildOwnedIndex();

	std::vector<Vertex> path;
	// Окно: segments[i] — отрезок path[windowFrom + i] → path[windowFrom + i + 1].
	std::vector<CEntityHandle> segments;
	// Те же хендлы, отсортированные, — для двоичного поиска в OwnsParticle (горячий путь
	// CheckTransmit: на каждую помеченную частицу, на каждого получателя). Идиома взята у
	// KZZonesService::showBeams по той же причине.
	std::vector<CEntityHandle> ownedSorted;
	u32 windowFrom = 0, windowTo = 0;
	u32 nearest = 0;
	bool resync = true;
	bool enabled = false;
	// Сетевая фаза: резолв/докачка уже идут, пути ещё нет. Без этого гейта каждый
	// повторный !lead в окне между Toggle и колбэком заводил бы новый резолв, новую
	// докачку и новый поток разбора.
	bool loading = false;
	u32 ticksSinceUpdate = 0;
	std::shared_ptr<PendingLoad> pending;
	u32 generation = 0;
	// Имя режима, под который построен путь (модель «путь режим-зависим»).
	char modeName[64] {};
};
