
#pragma once

#define KZ_MAPPING_INTERFACE "KZMappingInterface"

#define KZ_NO_MAPAPI_VERSION           0
#define KZ_NO_MAPAPI_COURSE_DESCRIPTOR "Default"
#define KZ_NO_MAPAPI_COURSE_NAME       "Main"

#define KZ_MAPAPI_VERSION 2

#define KZ_MAX_SPLIT_ZONES        100
#define KZ_MAX_CHECKPOINT_ZONES   100
#define KZ_MAX_STAGE_ZONES        100
#define KZ_MAX_COURSE_COUNT       128
#define KZ_MAX_COURSE_NAME_LENGTH 65

// Диапазоны идентификаторов под курсы платформы (KZ::mapapi::CreateExternalCourse). Родной
// курс маппера нумеруется с 1 и получает hammerId энтити-дескриптора (>= 0), дефолтный курс
// карты без Mapping API — hammerId -1. Мы уезжаем заведомо в сторону от обоих; совпадение всё
// равно проверяется явно, потому что id родного курса задаёт маппер и запретить ему 1000 нечем.
#define KZ_PLATFORM_COURSE_ID_BASE        1000
#define KZ_PLATFORM_COURSE_HAMMER_ID_BASE (-1000)
#define KZ_PLATFORM_COURSE_MAX_NUMBER     99

#define INVALID_SPLIT_NUMBER      0
#define INVALID_CHECKPOINT_NUMBER 0
#define INVALID_STAGE_NUMBER      0
#define INVALID_COURSE_NUMBER     0

struct KZCourse;
class KZPlayer;

enum KzTriggerType
{
	KZTRIGGER_DISABLED = 0,
	KZTRIGGER_MODIFIER,
	KZTRIGGER_RESET_CHECKPOINTS,
	KZTRIGGER_SINGLE_BHOP_RESET,
	KZTRIGGER_ANTI_BHOP,

	KZTRIGGER_ZONE_START,
	KZTRIGGER_ZONE_END,
	KZTRIGGER_ZONE_SPLIT,
	KZTRIGGER_ZONE_CHECKPOINT,
	KZTRIGGER_ZONE_STAGE,

	KZTRIGGER_TELEPORT,
	KZTRIGGER_MULTI_BHOP,
	KZTRIGGER_SINGLE_BHOP,
	KZTRIGGER_SEQUENTIAL_BHOP,

	KZTRIGGER_PUSH,
	KZTRIGGER_COUNT,
};

// KZTRIGGER_MODIFIER
struct KzMapModifier
{
	bool disablePausing;
	bool disableCheckpoints;
	bool disableTeleports;
	bool disableJumpstats;
	bool enableSlide;
	f32 gravity;
	f32 jumpFactor;
	bool forceDuck;
	bool forceUnduck;
};

// KZTRIGGER_ANTI_BHOP
struct KzMapAntibhop
{
	f32 time;
};

// KZTRIGGER_ZONE_*
struct KzMapZone
{
	char courseDescriptor[128];
	i32 number; // not used on start/end zones
};

// KZTRIGGER_TELEPORT/_MULTI_BHOP/_SINGLE_BHOP/_SEQUENTIAL_BHOP
struct KzMapTeleport
{
	char destination[128];
	f32 delay;
	bool useDestinationAngles;
	bool resetSpeed;
	bool reorientPlayer;
	bool relative;
};

// KZTRIGGER_PUSH
struct KzMapPush
{
	// Cannot use Vector here as it is not a POD type.
	f32 impulse[3];

	enum KzMapPushCondition : u32
	{
		KZ_PUSH_START_TOUCH = 1,
		KZ_PUSH_TOUCH = 2,
		KZ_PUSH_END_TOUCH = 4,
		KZ_PUSH_JUMP_EVENT = 8,
		KZ_PUSH_JUMP_BUTTON = 16,
		KZ_PUSH_ATTACK = 32,
		KZ_PUSH_ATTACK2 = 64,
		KZ_PUSH_USE = 128,
	};

	u32 pushConditions;
	bool setSpeed[3];
	bool cancelOnTeleport;
	f32 cooldown;
	f32 delay;
};

struct KZCourseDescriptor

{
	KZCourseDescriptor(i32 hammerId = -1, const char *targetName = "", bool disableCheckpoints = false, u32 guid = 0,
					   i32 courseID = INVALID_COURSE_NUMBER, const char *courseName = "")
		: hammerId(hammerId), disableCheckpoints(disableCheckpoints), guid(guid), id(courseID)
	{
		V_snprintf(entityTargetname, sizeof(entityTargetname), "%s", targetName);
		V_snprintf(name, sizeof(name), "%s", courseName);
	}

	char entityTargetname[128] {};
	i32 hammerId = -1;
	bool disableCheckpoints = false;

	// Курс отключён платформой («удалён с нашего сервера»). НЕ удаление из вектора: FastRemove
	// ломает g_sortedCourses (дубль + фантом + сорванная сортировка), и мы этим путём не ходим
	// вовсе. Отключённый курс не попадает в g_sortedCourses, поэтому исчезает из !courses,
	// !main, !b*, HUD и SetupLocalCourses, а GetCourseDescriptorFromTrigger отдаёт по его зонам
	// nullptr — БЕЗ Mapi_Error, иначе каждое касание сыпало бы в общий чат.
	// Сам дескриптор остаётся в courseDescriptors, чтобы курс можно было включить обратно.
	bool disabled = false;

	bool hasStartPosition = false;
	Vector startPosition;
	QAngle startAngles;

	void SetStartPosition(Vector origin, QAngle angles)
	{
		hasStartPosition = true;
		startPosition = origin;
		startAngles = angles;
	}

	bool hasEndPosition = false;
	Vector endPosition;
	QAngle endAngles;

	i32 splitCount {};
	i32 checkpointCount {};
	i32 stageCount {};

	// Shared identifiers
	u32 guid {};

	// Mapper assigned course ID.
	i32 id;
	// Mapper assigned course name.
	char name[KZ_MAX_COURSE_NAME_LENGTH] {};

	bool HasMatchingIdentifiers(i32 id, const char *name) const
	{
		return this->id == id && (!V_stricmp(this->name, name));
	}

	CUtlString GetName() const
	{
		return name;
	}

	// ID used for local database.
	u32 localDatabaseID {};

	// ID used for global database.
	u32 globalDatabaseID {};
};

struct KzTrigger
{
	KzTriggerType type;
	CEntityHandle entity;
	i32 hammerId;

	// Триггер заспавнен ПЛАТФОРМОЙ (зона из нашего набора), а не картой. Признак структурный и
	// ставится в момент регистрации по окну BeginExternalTriggerSpawn — запрет на расширение
	// этого окна читайте в шапке той функции, признак держится ровно на его ширине.
	// Структурный, потому что по данным триггера наши от родных не отличить никак:
	// hammerId у мапперского триггера без hammerUniqueId тоже -1, а дескриптор и тип у нашей
	// зоны совпадают с родной НАМЕРЕННО (в этом и смысл переопределения).
	// Без него DisableCourseZones, фильтрующий по паре (курс, тип), гасит наши же зоны.
	bool platformSpawned;

	// Зона погашена платформой: type подменён на KZTRIGGER_DISABLED, а исходный лежит здесь,
	// чтобы RestorePlatformDisabledZones мог вернуть его обратно в пределах того же раунда.
	// Обе записи обнуляются вместе со всей таблицей на round_prestart.
	bool platformDisabled;
	KzTriggerType platformDisabledFrom;

	union
	{
		KzMapModifier modifier;
		KzMapAntibhop antibhop;
		KzMapZone zone;
		KzMapTeleport teleport;
		KzMapPush push;
	};
};

namespace KZ::mapapi
{
	// These namespace'd functions are called when relevant game events happen, and are somewhat in order.
	void Init();
	void OnCreateLoadingSpawnGroupHook(const CUtlVector<const CEntityKeyValues *> *pKeyValues);
	void OnSpawn(int count, const EntitySpawnInfo_t *info);
	void OnRoundPreStart();
	void OnRoundStart();

	// Окно регистрации триггеров открыто только между round_prestart и round_start
	// (см. гейт в Mapi_OnTriggerMultipleSpawn). Зонам платформы этого мало: поставленная
	// админом зона обязана ожить сразу, а рестарт раунда срубил бы раны всем на сервере.
	//
	// Окно открывается РОВНО вокруг цикла спавна зон платформы — ни одного кадра шире.
	// Это уже не только разрешение регистрировать: с введением KzTrigger::platformSpawned флаг
	// стал ЕДИНСТВЕННЫМ источником признака «зона наша, а не карты». Всё, что зарегистрируется
	// внутри окна, будет считаться нашим навсегда — до конца раунда.
	// Отсюда запрет: не расширять окно на «весь ApplyLoadedZones» или иной участок, где может
	// случиться движковый спавн. Попавшая внутрь родная зона получит platformSpawned, перестанет
	// гаситься в DisableCourseZones, и переопределение курса молча не сработает — при здоровом
	// с виду логе. Отличить её потом будет нечем: hammerId у мапперского триггера без
	// hammerUniqueId тоже -1, а дескриптор и тип у переопределяющей зоны совпадают с родными
	// намеренно.
	void BeginExternalTriggerSpawn();
	void EndExternalTriggerSpawn();

	// Сколько триггеров зарегистрировано сейчас. Вектор фиксированный (2048) и AddToTail
	// не проверяет ёмкость, поэтому все, кто спавнит триггеры вне разбора карты, обязаны
	// смотреть на потолок сами.
	i32 RegisteredTriggerCount();
	i32 MaxRegisteredTriggers();

	// Разобрала ли карта свои дескрипторы курсов. Истинно с того момента, как отработал хук
	// загрузки spawn-группы: там же читается версия Mapping API и создаются ВСЕ дескрипторы
	// курсов карты (на карте без Mapping API — дефолтный курс форка).
	// Тем, кто решает «а есть ли на карте курсы», спрашивать обязательно: до разбора
	// GetCourseCount() == 0 не означает «курсов нет», и решение по нему завело бы лишний главный
	// курс на карте С курсами, а занятый нами дескриптор заставил бы карту бить Mapi_Error —
	// то есть спам в общий чат всем игрокам раз в минуту.
	bool IsMapParsed();

	// Есть ли на карте дескриптор курса с таким targetname. Зона start/end платформы обязана
	// ссылаться на существующий дескриптор: иначе КАЖДОЕ касание бьёт Mapi_Error, а тот раз в
	// минуту высыпает накопленное в ОБЩИЙ ЧАТ всем игрокам.
	bool HasCourseDescriptor(const char *targetname);

	// Принадлежит ли курс с таким дескриптором платформе, то есть вправе ли мы вешать на него
	// свои зоны. Истина в двух случаях: курс завела CreateExternalCourse (id в платформенном
	// диапазоне) либо это дефолтный курс, который форк заводит сам на карте без Mapping API.
	//
	// Проверять здесь ИМЯ дескриптора недостаточно, и это не теория: "Default" ничем не
	// зарезервировано, Mapi_OnInfoTargetSpawn разрешает мапперу назвать так свой курс. Гейт по
	// имени пустил бы нашу зону в ЧУЖОЙ курс — второй старт-зоной поверх мапперской и с записью
	// рекордов в его курс.
	bool IsPlatformOwnedCourse(const char *descriptorName);

	// Локальный id курса в БД плагина, найденный по ДЕСКРИПТОРУ. Именно по нему, а не по имени
	// курса: имя не уникально между картой и платформой, а лукап по имени отдаёт первое
	// совпадение в порядке сортировки по id — то есть при сожительстве двух «Main» вернул бы
	// ЧУЖОЙ id. Тот, кто по этому значению решает «курс заведён в БД», принял бы чужой успех за
	// свой и оставил свой курс с нулём.
	// false — дескриптора нет; 0 в out — курс есть, но в локальную БД ещё не попал.
	bool GetCourseLocalDatabaseId(const char *descriptorName, u32 &out);

	// Дескриптор курса ровно в том написании, в каком его держит карта, либо nullptr.
	// Зона платформы ОБЯЗАНА спавниться с этой строкой, а не с той, что пришла из api: поиск
	// курса идёт без учёта регистра (KZ_STREQI), а подсчёт зон курса — С учётом (KZ_STREQ, см.
	// Mapi_CountCourseZones). Зона, чей регистр разошёлся, исправно запускает таймер, но не
	// попадает в счётчики стейджей — а те гейтят финиш. Отказ был бы полностью тихим.
	//
	// Указатель смотрит ВНУТРЬ courseDescriptors: копируйте строку сразу, между кадрами хранить
	// нельзя — валидация на round_start делает FastRemove, и содержимое слота меняется.
	// Отключённый курс тоже отдаёт своё имя: функция про написание, а не про доступность.
	const char *GetCanonicalCourseDescriptor(const char *descriptorName);

	// Погасить родные зоны курса заданного типа: тип записи в таблице триггеров меняется на
	// KZTRIGGER_DISABLED, и она проваливается через все switch обработчиков касаний.
	// Возвращает число погашенных ЖИВЫХ зон, или -1 при отказе (с reason в логе).
	//
	// РАЗРЕШЕНЫ ТОЛЬКО ЗОНЫ ТАЙМЕРА. Модификаторы отключать нельзя никогда — причина в
	// реализации, прочитайте её перед тем, как «обобщать» на все типы.
	//
	// Зовётся ПОСЛЕ регистрации родных триггеров и ДО спавна своих: ключ (курс, тип) не
	// различает наши зоны и родные, поэтому обратный порядок погасил бы свои же.
	//
	// ЖИВЁТ ДО БЛИЖАЙШЕГО round_prestart, не дольше. OnRoundPreStart чистит таблицу триггеров
	// целиком, движок пересоздаёт энтити карты, и на round_start родные зоны регистрируются
	// заново с ИСХОДНЫМ типом — погашение откатывается само и молча. Поэтому вызывающий обязан
	// переприменять набор на каждом round_start, там же, где он спавнит свои зоны. Один вызов
	// «на загрузке карты» даст тихий откат.
	//
	// Счётчики зон курса (splitCount/checkpointCount/stageCount) после погашения устаревают, а
	// stageCount гейтит финиш в TimerEnd. Вызывающий обязан позвать RecountCourseZones() после
	// применения набора.
	i32 DisableCourseZones(const char *descriptorName, KzTriggerType type);

	// Вернуть исходный тип всем зонам, погашенным DisableCourseZones. Возвращает число
	// восстановленных.
	// Зовётся в начале КАЖДОГО применения набора, до погашений: набор мог измениться (админ снял
	// подмену зоны), и без восстановления родная зона осталась бы погашенной до смены карты.
	// Так применение становится идемпотентным — «восстановить всё, затем погасить по текущему
	// набору» — вместо накопления состояния между применениями.
	i32 RestorePlatformDisabledZones();

	// Отключить/включить курс целиком. Отключённый исчезает из витрины g_sortedCourses (то есть
	// из !courses, !main, !b*, HUD, SetupLocalCourses), но остаётся в courseDescriptors, чтобы
	// его можно было включить обратно. При отключении таймер игроков, бегущих по этому курсу,
	// останавливается явно с причиной в лог и сообщением в чат.
	// false — дескриптора нет либо он уже в этом состоянии.
	bool SetCourseDisabled(const char *descriptorName, bool disabled);

	// Завести курс платформы в рантайме. Штатного пути нет: дескрипторы создаются только из
	// Mapi_OnInfoTargetSpawn, а тот зовётся исключительно из хука загрузки spawn-группы карты и
	// один раз за карту. Отсюда эта обёртка — по образцу BeginExternalTriggerSpawn.
	//
	// platformNumber (0..KZ_PLATFORM_COURSE_MAX_NUMBER) задаёт id и hammerId курса из
	// платформенных диапазонов выше; 0 — главный курс.
	// Возвращает nullptr при успехе, иначе машинно-читаемую причину для reason= в логе.
	// Проверяет столкновения по id/hammerId/имени/дескриптору — см. комментарии в реализации,
	// совпадение id с родным курсом переименовало бы родной курс в локальной БД. Столкновение по
	// guid не отказ: оно чинится переприсвоением сразу после создания.
	const char *CreateExternalCourse(i32 platformNumber, const char *courseName, const char *descriptorName);

	// Стартовая позиция курса из объёма триггера (та же utils::FindValidPositionForTrigger, что
	// у конечной позиции). Нужна там, где родного info_teleport_destination "timer_start" нет:
	// без неё !main / меню !courses / рестарт не знают, куда телепортить.
	// overwrite=false не трогает уже заданную позицию — на карте без Mapping API её задаёт маппер
	// через info_teleport_destination "timer_start", и молча перебивать её мы не вправе.
	bool SetCourseStartPositionFromTrigger(const char *descriptorName, CBaseTrigger *trigger, bool overwrite);

	// Пересчитать split/checkpoint/stage-счётчики всех курсов по текущей таблице триггеров.
	// Валидация в OnRoundStart считает их ОДИН раз и до того, как платформа поставит свои зоны
	// (порядок в hooks.cpp), а счётчики гейтят финиш: KZTimerService::TimerEnd отбивает ран,
	// если currentStage != courseDesc->stageCount. Зовётся после каждого применения набора.
	// Ошибки пишет в лог сервера, а не через Mapi_Error (тот идёт в общий чат) и курсы не дропает.
	void RecountCourseZones();

	void CheckEndTimerTrigger(CBaseTrigger *trigger);
	// This is const, unlike the trigger returned from Mapi_FindKzTrigger.
	const KzTrigger *GetKzTrigger(CBaseTrigger *trigger);

	const KZCourseDescriptor *GetCourseDescriptorFromTrigger(CBaseTrigger *trigger);
	const KZCourseDescriptor *GetCourseDescriptorFromTrigger(const KzTrigger *trigger);

	inline bool IsBhopTrigger(KzTriggerType triggerType)
	{
		return triggerType == KZTRIGGER_MULTI_BHOP || triggerType == KZTRIGGER_SINGLE_BHOP || triggerType == KZTRIGGER_SEQUENTIAL_BHOP;
	}

	inline bool IsTimerTrigger(KzTriggerType triggerType)
	{
		static_assert(KZTRIGGER_ZONE_START == 5 && KZTRIGGER_ZONE_STAGE == 9,
					  "Don't forget to change this function when changing the KzTriggerType enum!!!");
		return triggerType >= KZTRIGGER_ZONE_START && triggerType <= KZTRIGGER_ZONE_STAGE;
	}

	inline bool IsPushTrigger(KzTriggerType triggerType)
	{
		return triggerType == KZTRIGGER_PUSH;
	}

	inline bool IsTeleportTrigger(KzTriggerType triggerType)
	{
		return triggerType == KZTRIGGER_TELEPORT;
	}
} // namespace KZ::mapapi

// Exposed interface to modes.
class MappingInterface
{
public:
	virtual bool IsTriggerATimerZone(CBaseTrigger *trigger);
	bool GetJumpstatArea(Vector &pos, QAngle &angles);
};

extern MappingInterface *g_pMappingApi;

namespace KZ::course
{
	// Clear the list of current courses.
	void ClearCourses();

	// Get the number of courses on this map.
	u32 GetCourseCount();

	// Get a course's information given its map-defined course id.
	const KZCourseDescriptor *GetCourseByCourseID(i32 id);

	// Get a course's information given its local course id.
	const KZCourseDescriptor *GetCourseByLocalCourseID(u32 id);

	// Get a course's information given its global course id.
	const KZCourseDescriptor *GetCourseByGlobalCourseID(u32 id);

	// Get a course's information given its name.
	const KZCourseDescriptor *GetCourse(const char *courseName, bool caseSensitive = true, bool matchPartial = false);

	// Get a course's information given its GUID.
	const KZCourseDescriptor *GetCourse(u32 guid);

	// Get the first course's information sorted by map-defined ID.
	const KZCourseDescriptor *GetFirstCourse();

	// Номер курса для ingest cyber: 0 = main (первый курс по mapper id),
	// N = bonus N (по имени вида "Bonus N"), иначе — mapper id курса.
	i32 GetCyberCourseNumber(const KZCourseDescriptor *course);

	// Обратный резолв: курс, чей GetCyberCourseNumber() == n (см. FindBonusCourse в
	// kz_mappingapi.cpp, зеркалит ту же конвенцию для n == 0 / main). Task 4 (SavedRuns):
	// восстановление рана хранит только cyber-номер курса, не guid/имя (карта могла
	// обновиться между сессиями). nullptr, если курса с таким номером на карте больше нет.
	const KZCourseDescriptor *GetCourseByCyberNumber(i32 n);

	// Отдать платформе список курсов этой карты: cyber-номер + имя курса + дескриптор +
	// признак «отключён платформой». Платформе имена курсов взять больше негде, а без них она
	// показывает бонус как «C2» вместо «B1»: cyber-номер бонуса, чьё имя не «bonusN», уходит в
	// диапазон 100+ (см. GetCyberCourseNumber — нумерацию менять НЕЛЬЗЯ, по ней уже лежат
	// рекорды), и различить «бонус» от «стейджа» по одному числу невозможно.
	//
	// Точка вызова — round_start, ПОСЛЕ KZ::mapapi::OnRoundStart и KZ::zones::OnRoundStart, и
	// ровно один раз за карту. Раньше нельзя: до валидации на round_start список курсов ещё
	// может сократиться (FastRemove невалидного курса + Mapi_RebuildSortedCourses), а это меняет,
	// какой курс «первый», то есть чей cyber-номер 0. С этого момента РОДНЫЕ курсы карты и их
	// cyber-номера не двигаются до смены карты — а отчёт нужен именно ради них: имён родных
	// курсов у платформы нет нигде.
	//
	// Чего в снимке может не быть, осознанно: курсы, которые платформа заводит сама
	// (CreateExternalCourse из ApplyLoadedZones), и отключения курсов (SetCourseDisabled) —
	// набор зон приезжает асинхронным ответом api и применяется как на round_start, так и
	// позже. Переотправлять на каждое такое изменение не нужно: и номер, и имя таких курсов
	// платформа задала сама, то есть они у неё уже есть; поле disabled в отчёте — снимок на
	// момент отправки, а не источник истины.
	//
	// POST {cybEmitUrl}/ingest/v1/kz/courses/report, fire-and-forget. NO-OP при выключенном
	// cybEmitUrl и на картах без Mapping API.
	void ReportCoursesToPlatform();

	// Setup all the courses to the local database.
	void SetupLocalCourses();

	// Update the course's database ID given its name.
	bool UpdateCourseLocalID(const char *courseName, u32 databaseID);

	// Update the course's global ID given its map-defined name and ID.
	bool UpdateCourseGlobalID(const char *courseName, u32 globalID);

	// Print the list of courses to the console.
	void PrintCourses(KZPlayer *player);

}; // namespace KZ::course
