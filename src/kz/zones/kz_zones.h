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

// Потолок курсов платформы на карту. Согласован с api (он не отдаст больше) и взят с запасом
// относительно KZ_MAX_COURSE_COUNT = 128 слотов вектора дескрипторов: остаток нужен родным
// курсам карты, которые мы не создаём, но которые занимают те же слоты.
#define KZ_COURSES_PER_MAP_LIMIT 64

// Геометрия. Оба угла редактор берёт с позиции игрока, то есть с пола: без добавленной
// высоты вышел бы триггер нулевой высоты, а api отбил бы его как degenerate_box.
#define KZ_ZONE_EDITOR_HEIGHT 72.0f // рост стоящего игрока
#define KZ_ZONE_MIN_SIZE      8.0f  // как KZ_ZONE_MIN_SIZE в контракте
#define KZ_ZONE_WORLD_LIMIT   16384.0f

// Потолок ОТРИСОВКИ (не набора): сколько зон максимум обводим постоянным контуром. Ребро —
// отдельный сетевой info_particle_system, то есть зона стоит двенадцати энтити; при потолке
// набора в 64 зоны без этого ограничения вышло бы под тысячу сущностей, которые вдобавок
// перебирает KZ::quiet::OnCheckTransmit на КАЖДЫЙ CheckTransmit для КАЖДОГО получателя.
// Постоянный слой один на карту, поэтому его цена фиксирована и от числа игроков не зависит.
#define KZ_ZONE_HIGHLIGHT_MAX_ZONES 20

// Потолки ВРЕМЕННОГО показа (!zone show). Он умножается на игроков, поэтому потолков два и
// оба в РЁБРАХ, а не в зонах: считать надо то, что реально стоит в мире и перебирается в
// трансмите.
//   * на игрока — 96 рёбер (8 зон). Втрое меньше постоянного слоя намеренно: показ адресный,
//     живёт 12 секунд и нужен, чтобы сориентироваться, а не изучать карту целиком.
//   * на сервер — 576 рёбер, то есть шесть одновременных показов. Без него команда без прав
//     при полном сервере ставила бы тысячи энтити, а упор в лимит энтити бьёт не по картинке:
//     переставать спавниться начнут САМИ ЗОНЫ (SpawnZone вернёт removed_on_spawn), и разбор
//     уедет в сторону от причины.
#define KZ_ZONE_SHOW_MAX_EDGES_PLAYER 96
#define KZ_ZONE_SHOW_MAX_EDGES_SERVER 576
// Кулдаун на команду: показ создаёт и сносит десятки энтити, спам этим — дешёвый способ
// нагрузить сервер, а прав команда не спрашивает.
#define KZ_ZONE_SHOW_COOLDOWN 3.0f

// Рёбер в контуре AABB. Именованная константа, потому что размер массивов хендлов и граница
// цикла отрисовки обязаны совпадать.
#define KZ_ZONE_BOX_EDGES 12

// targetname трёх слоёв рисования. Они РАЗНЫЕ намеренно и лежат рядом, чтобы это было видно:
// снятие фильтрует и по списку хендлов, и по имени, поэтому код, гасящий один слой, не может
// тронуть другой даже по ошибке. Постоянный виден всем, два других адресные.
#define KZ_ZONE_HIGHLIGHT_NAME "cyb_zone_hl"      // постоянный контур start/end, всем
#define KZ_ZONE_SHOW_NAME      "cyb_zone_show"    // !zone show, только вызвавшему
#define KZ_ZONE_PREVIEW_NAME   "cyb_zone_preview" // превью редактора, только автору зоны

// Сколько ждём round_start после ответа api, прежде чем признать, что мир так и не доделан.
#define KZ_ZONES_WORLD_READY_TIMEOUT 60.0f

enum KzCyberZoneType
{
	KZ_CYBER_ZONE_START = 0,
	KZ_CYBER_ZONE_END,
	KZ_CYBER_ZONE_MODIFIER,
	KZ_CYBER_ZONE_STAGE,
	KZ_CYBER_ZONE_CHECKPOINT,
	// В enum есть, но НЕ разбирается ParseZoneType и не выставляется редактором: api отбивает
	// split четырёхсотым. Заведён, чтобы номера типов не поехали, когда его включат.
	KZ_CYBER_ZONE_SPLIT,
	KZ_CYBER_ZONE_TYPE_COUNT,
};

// Курс карты, как его видит платформа. Приходит тем же ответом, что и зоны: курс обязан
// существовать раньше зоны, которая на него ссылается, а два асинхронных запроса порядок не
// гарантируют.
struct KzCyberCourse
{
	char descriptor[128] {}; // entityTargetname; у own — наш, у override — родной курс карты
	bool own {};             // курс завели мы (kind=own), а не переопределяем родной
	bool disabled {};        // курс «удалён» с нашего сервера
	i32 courseNumber {};     // 0 = main, 1..99 = бонус
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
	// Дескриптор курса, которому принадлежит зона. Пустая строка = зона вне курсов (бустеры и
	// одиночные start/end на картах без курсов) — прежнее поведение первой итерации.
	char courseDescriptor[128] {};
	i32 stageNumber {}; // только у stage/checkpoint; нумерация приходит от api готовой
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

	// Цвет зоны по типу. Совпадает с палитрой отладочного kz_showtriggers (kz_misc.cpp):
	// два разных языка цветов для одних и тех же типов зон в одном плагине читались бы неверно.
	Color ZoneColor(KzCyberZoneType type);

	// Ключ приоритета зоны при упоре в потолок отрисовки: номер курса, которому она принадлежит
	// (главный курс = 0, бонусы дальше по номеру, неизвестный курс — в самый хвост). И постоянный
	// слой, и !zone show обрезают хвост ПО ЭТОМУ ключу: пропасть должна подсветка дальнего
	// бонуса, а не главного курса.
	i32 CourseRank(const KzCyberZone &zone);

	// Контур AABB двенадцатью беам-частицами. Штатный kz_showtriggers наши зоны не рисует: он
	// берёт форму из VPhysX-меша энтити, а у зоны геометрия задана bbox'ом и меша нет вовсе.
	//
	// ownerOnly=true помечает частицы плагинной командой (CUSTOM_PARTICLE_SYSTEM_TEAM): такие
	// KZ::quiet::OnCheckTransmit вычёркивает из трансмита ВСЕМ, кроме тех, кто назван в его белом
	// списке. То есть метка — это «видит только владелец, и только если он в белом списке»,
	// а не просто «наша частица». Для видимого всем контура метку ставить НЕЛЬЗЯ.
	//
	// targetname — по нему снятие отличает свои частицы от чужих info_particle_system (их в форке
	// хватает: kz_beam, kz_measure, ztopwatch, худ). У каждого слоя он СВОЙ, чтобы код гашения
	// одного слоя не мог дотянуться до другого.
	// out принимает KZ_ZONE_BOX_EDGES хендлов; на месте несозданных остаётся пустой хендл.
	// Возвращает число созданных рёбер: ноль значит «движок не отдал энтити», и вызывающий не
	// вправе считать зону нарисованной.
	i32 DrawBoxEdges(const Vector &mins, const Vector &maxs, const Color &color, const char *targetname, bool ownerOnly, CEntityHandle *out);

	// Снять рёбра, созданные DrawBoxEdges, и занулить хендлы. Удаляет только энтити с ЭТИМ
	// targetname: чужое (и рёбра соседнего слоя) не трогает никогда.
	// keepEntities=true — только занулить хендлы: на смене карты мир перестраивается, прежние
	// энтити уже не наши, и RemoveEntity по ним лез бы в перестраивающийся мир.
	void RemoveBoxEdges(CEntityHandle *handles, i32 count, const char *targetname, bool keepEntities = false);
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
	CEntityHandle previewBeams[KZ_ZONE_BOX_EDGES] {};
	// Есть ли сейчас превью. Отдельный флаг, а не «посмотреть в массив»: он гейтит скан хендлов
	// в белом списке kz_quiet, который крутится на каждый CheckTransmit для каждого получателя.
	bool previewActive {};
	// Растёт на каждое новое превью: таймер снятия гасит только своё поколение, иначе таймер
	// от первой зоны стирал бы превью второй, поставленной в те же 15 секунд.
	u32 previewGeneration {};

	// !zone show — временный показ ВСЕХ зон, видимый только вызвавшему.
	//
	// Хранилище СВОЁ и отдельное от постоянной подсветки старта с финишем (та живёт в
	// kz_zones.cpp, видна всем и переживает эту команду). Разведение структурное: разные списки
	// хендлов И разные targetname, поэтому код гашения показа не может дотянуться до постоянного
	// слоя даже ошибкой. Повторный !zone show гасит показ — и только его.
	//
	// ДЕРЖИТСЯ ОТСОРТИРОВАННЫМ по возрастанию хендла, и это не косметика: список ищется в
	// KZ::quiet::OnCheckTransmit — на каждую помеченную частицу, на каждого получателя, каждый
	// CheckTransmit. Линейный скан здесь квадратичен по числу рёбер, двоичный поиск — семь
	// сравнений. Кто добавляет элементы, тот и восстанавливает порядок (см. ShowAllZones).
	std::vector<CEntityHandle> showBeams;
	// Время последнего показа (curtime) — для кулдауна KZ_ZONE_SHOW_COOLDOWN.
	f32 lastShowTime {};
	// Своё поколение у показа: за 12 секунд игрок может выключить и включить показ снова, и
	// таймер от прошлого не должен гасить новый.
	u32 showGeneration {};

	// Дисконнект игрока (PlayerManager::OnClientDisconnect) — и он же путь смены карты на
	// listen-сервере (Hook_ActivateServer зовёт Reset слоту 0). Поэтому снятие идёт с
	// keepEntities=false, то есть по перестраивающемуся миру: защита здесь — проверка хендла и
	// NameMatches внутри RemoveBoxEdges, они и не дают тронуть чужую энтити с переиспользованным
	// индексом. Так было и до показа (ClearPreview стоит здесь с cyb.118); на выделенном сервере
	// этот путь не срабатывает вовсе.
	virtual void Reset() override
	{
		this->ClearPreview();
		this->ClearShow();
		this->lastShowTime = {};
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

	// Показать все зоны карты этому игроку на KZ_ZONE_SHOW_SECONDS. Повторный вызов гасит показ.
	void ShowAllZones();

	// Рисование AABB беамом: штатный kz_showtriggers наши зоны не покажет — он берёт форму
	// из VPhysX-меша энтити, а у наших её нет (объём задан bbox'ом).
	void DrawBox(const Vector &mins, const Vector &maxs, Color color, f32 duration);
	// keepEntities=true — только занулить хендлы, не трогая мир: на смене карты энтити уже
	// не наши, и RemoveEntity по ним лез бы в перестраивающийся мир.
	void ClearPreview(bool keepEntities = false);
	void ClearShow(bool keepEntities = false);

	// Белый список KZ::quiet::OnCheckTransmit: частицы этого игрока, которые ему показываем.
	// Дешёвый гейт вынесен отдельно и зовётся ПЕРВЫМ: цикл трансмита перебирает все частицы для
	// каждого получателя, и у игрока без превью и без показа не должно быть ни одного сравнения.
	bool HasOwnedParticles() const
	{
		return this->previewActive || !this->showBeams.empty();
	}

	// Двенадцать сравнений на превью (гейтится флагом) плюс двоичный поиск по showBeams.
	// Линейный скан здесь стоил бы десятков тысяч сравнений за тик на одного показывающего.
	bool OwnsParticle(const CEntityHandle &handle) const;
};
