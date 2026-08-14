#pragma once
#include "../kz.h"

#include <string>
#include <vector>

/*
	Зоны KZ-карт из БД платформы (LAM-20).

	Чиним сломанные карты, не трогая их в Workshop: свои start/end/бустер-зоны лежат в нашем
	api, форк тянет их на загрузке карты и спавнит настоящими trigger_multiple, которые штатный
	Mapping API регистрирует как родные триггеры карты. Правок в коде касаний и таймера — ноль.

	ДОКАЗАНО СПАЙКОМ (cyb.117, замер на srv-5, 5 прогонов, две карты) И НЕ ПОДЛЕЖИТ ПЕРЕСМОТРУ:
	объём энтити (m_vecMins/m_vecMaxs/m_nSolidType) обязан задаваться ДО DispatchSpawn. Движок
	строит представление коллизии внутри Spawn; запись тех же полей после спавна в broadphase
	не попадает, и движковая трассировка касаний (TraceShape + CTraceFilterHitAllTriggers)
	зону не видит вовсе — verdict=trace_blind при hit_ours=0. С записью до спавна —
	verdict=trace_sees_zone. Ни CollisionRulesChanged(), ни Teleport() после спавна этого не
	лечат: их вызовы были в обеих ветках замера.
*/

// Потолок зон на карту. Держим синхронно с KZ_ZONES_PER_MAP_LIMIT в
// packages/contracts/src/http/kz-zones.ts: api не отдаст больше, но вектор триггеров
// (CUtlVectorFixed<KzTrigger, 2048>) общий с картой, и полагаться на чужую проверку нельзя.
#define KZ_ZONES_PER_MAP_LIMIT 64

// Геометрия. Оба угла редактор берёт с позиции игрока, то есть с пола: без добавленной
// высоты вышел бы триггер нулевой высоты, а api отбил бы его как degenerate_box.
#define KZ_ZONE_EDITOR_HEIGHT 72.0f // рост стоящего игрока
#define KZ_ZONE_MIN_SIZE      8.0f  // как KZ_ZONE_MIN_SIZE в контракте
#define KZ_ZONE_WORLD_LIMIT   16384.0f

// Сколько ждём round_start после ответа api, прежде чем признать, что мир так и не доделан.
#define KZ_ZONES_WORLD_READY_TIMEOUT 60.0f

enum KzCyberZoneType
{
	KZ_CYBER_ZONE_START = 0,
	KZ_CYBER_ZONE_END,
	KZ_CYBER_ZONE_MODIFIER,
	KZ_CYBER_ZONE_TYPE_COUNT,
};

// Зона, как её отдал api. Живёт в памяти процесса до следующей загрузки карты.
struct KzCyberZone
{
	char id[40] {}; // uuid зоны, нужен для DELETE
	KzCyberZoneType type {};
	Vector mins {};
	Vector maxs {};
	f32 jumpFactor {}; // только у modifier
	u64 createdBy {};  // steam_id64 автора, 0 = не отдан api
};

namespace KZ::zones
{
	// Алфавит имени карты у зон: ^[a-z0-9_]{1,64}$ (kzMapNameSchema в контракте api).
	bool IsValidZoneMapName(const std::string &name);
	bool ParseZoneType(const char *raw, KzCyberZoneType &out);
	const char *ZoneTypeToString(KzCyberZoneType type);

	// Карта загрузилась — тянем набор зон из api (асинхронно). Отказ api = играем на родных
	// триггерах карты: один warn с reason, без ретраев до следующей загрузки.
	void OnMapLoaded();

	// round_prestart: только помечаем мир «не готов». Спавнить здесь НЕЛЬЗЯ — движковая очистка
	// мира идёт после этого события и сносит созданные энтити (баг cyb.118).
	void OnRoundPreStart();

	// round_start: мир доделан. Считаем аудит выживаемости прошлых зон и ставим набор заново
	// (Mapping API чистит вектор триггеров на round_prestart, поэтому регистрация нужна снова).
	void OnRoundStart();

	// Разрешено ли редактору ставить зоны прямо сейчас (набор загружен и карта известна).
	bool IsReady();

	const char *CurrentMapName();

	// Набор зон текущей карты (для !zone list и debug-draw).
	const std::vector<KzCyberZone> &Loaded();

	// Добавить зону в набор и сразу заспавнить, не дожидаясь смены раунда: рестарт раунда
	// срубил бы раны всем на сервере.
	// revision — авторитетный из ответа api (0 = api не отдал, тогда считаем локально).
	// Возвращает nullptr, если зона ожила прямо сейчас, иначе машинно-читаемую причину, почему
	// нет (её же показываем автору): «сохранено» и «работает» обязаны различаться в интерфейсе.
	const char *AddAndSpawn(const KzCyberZone &zone, i32 apiRevision);

	// Убрать зону из набора и снять её энтити.
	// apiRevision — авторитетный из ответа api (0 = не отдан, считаем локально).
	bool RemoveById(const char *id, i32 apiRevision = 0);

	// Смена карты: сбросить незакрытые углы у всех игроков — иначе бокс склеится из координат
	// двух разных карт (KZPlayer::Reset на смену карты не зовётся).
	void ResetEditors();

	i32 Revision();
} // namespace KZ::zones

// Состояние редактора — на игрока. В хуках движения не участвует: только команды и рисование.
class KZZonesService : public KZBaseService
{
	using KZBaseService::KZBaseService;

public:
	// Право game.kz_zones_edit, спрошенное у api. Кэш на игровую сессию: PENDING до ответа,
	// дальше ALLOWED/DENIED. Отказ показываем в чат, а не молчим.
	enum PermState
	{
		PERM_UNKNOWN = 0,
		PERM_PENDING,
		PERM_ALLOWED,
		PERM_DENIED,
	};

	PermState perm {};
	// Незакрытая зона: первый !zone <тип> пишет угол A, второй закрывает бокс.
	bool hasPendingCorner {};
	KzCyberZoneType pendingType {};
	f32 pendingJumpFactor {};
	Vector pendingCorner {};
	CEntityHandle previewBeams[12] {};
	// Растёт на каждое новое превью: таймер снятия гасит только своё поколение, иначе таймер
	// от первой зоны стирал бы превью второй, поставленной в те же 15 секунд.
	u32 previewGeneration {};

	virtual void Reset() override
	{
		this->ClearPreview();
		this->perm = PERM_UNKNOWN;
		this->hasPendingCorner = false;
		this->pendingType = {};
		this->pendingJumpFactor = {};
		this->pendingCorner = {};
	}

	void OnMapChanged();
	void RequestPermission();
	bool EnsureAllowed();

	void BeginOrFinish(KzCyberZoneType type, f32 jumpFactor);
	void SubmitZone(const KzCyberZone &zone);
	void CancelPending();
	void ListZones();
	void RemoveZone(i32 humanIndex);

	// Рисование AABB беамом: штатный kz_showtriggers наши зоны не покажет — он берёт форму
	// из VPhysX-меша энтити, а у наших её нет (объём задан bbox'ом).
	void DrawBox(const Vector &mins, const Vector &maxs, Color color, f32 duration);
	// keepEntities=true — только занулить хендлы, не трогая мир: на смене карты энтити уже
	// не наши, и RemoveEntity по ним лез бы в перестраивающийся мир.
	void ClearPreview(bool keepEntities = false);
};
