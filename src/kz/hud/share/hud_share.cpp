// Ядро обмена настройками худа: белый список из реестра, снимок, применение с
// предохранителями, откат. Спека: docs/superpowers/specs/2026-09-09-hud-share-design.md.
//
// ФОРМАТ СНИМКА (H1) — текст, КЛЮЧЕВОЙ (не позиционный):
//
//     kzhud1.<8 hex>|<ключ>=<значение>|<ключ>=<значение>|...
//     ^^^^^^ ^^^^^^^
//     версия  отпечаток состава (CRC32 по именам ключей белого списка в порядке реестра)
//
// Почему так:
//   - КЛЮЧЕВОЙ, а не позиционный: требование §5.5 «неизвестные ключи игнорировать, битые не
//     применять» выполнимо только когда в снимке есть имена. Позиционный формат при сдвиге
//     состава даёт не «часть настроек», а случайный худ.
//   - ТЕКСТ, а не бинарь+base64: транспорт — короткий код через БД, а не консольная строка на
//     511 символов, поэтому компактность не платится читаемостью. Снимок в TEXT-поле и в
//     JSON-префе (слот отката) читается глазами при разборе жалобы, и это его главная ценность:
//     дефекты худа разбираются по логам, а не в дебаггере.
//   - ВЕРСИЯ обязательна (§ задачи): другая версия = отказ целиком, а не «применилось 3 из 64».
//   - ОТПЕЧАТОК СОСТАВА — потому что версии мало: при ключевом формате добавление ключа в худ
//     не ломает разбор, и старый снимок применился бы МОЛЧА, оставив новый ключ у получателя
//     своим. Отпечаток делает такой снимок видимым: ключи, которых в нём нет, выставляются
//     ДЕФОЛТОМ пункта (результат целиком определён снимком, а не смесью двух худов), а игрок
//     получает предупреждение. Поднимать SNAPSHOT_VERSION при добавлении ключа НЕ надо —
//     отпечаток считается из реестра сам.
//   - Набор ВСЕГДА полный: снимок несёт все ключи белого списка, включая те, что у отправителя
//     стоят на дефолте (тот же довод, что у апстрима, pref_registry.cpp:69-71) — иначе «дай
//     мне твой худ» дало бы смесь его настроек и моих.
#include "kz/hud/share/hud_share.h"
#include "kz/hud/layout/layout.h" // LAYOUT_ELEMENTS/LayoutElement — пол по видимости
#include "kz/option/kz_option.h"
#include "kz/language/kz_language.h"
#include "utils/logging.h"
#include "utils/utils.h" // g_pKZUtils->GetServerGlobals()->realtime — кулдаун выдачи кода

#include <ctype.h>

#include "checksum_crc.h"
#include "tier1/random.h" // RandomInt — генерация кода обмена
#include "tier1/strtools.h"

#include "tier0/memdbgon.h"

// Ключи, которые СТРУКТУРНО попали бы в обмен (они в категории HUD), но обмениваться не должны.
// Причина на каждый — спека §5.1.
static_global const char *const kHudShareBlocklist[] = {
	// Поведение спектейта, а не вид худа: скопировать чужой true = молча включить получателю
	// мимикрию. Тот же довод уже зафиксирован в hud/layout/defaults.cpp:81-82.
	"mhudMimicSpec",
	// Настройка СТАНДАРТНОГО (HTML) худа, а не панорамы: панорамному получателю не делает
	// ничего, HTML-получателю меняет его собственный выбор.
	"compactPanel",
	// Пустой экран у получателя: у отправителя может стоять HUD_TYPE_OFF. В реестре пункта с
	// префом нет (Choice без SetItemPref), но опираться на это молча нельзя — исключаем явно.
	"hudType",
	// Маркер одноразовой перезаписи дефолтов (§5.6): импорт его не переносит и не сбрасывает —
	// иначе либо переоткрыл бы миграцию, либо закрыл её навсегда.
	KZHUDService::HUD_DEFAULTS_REV_KEY,
};

// Один слот отката на игрока, В ПАМЯТИ (обоснование — hud_share.h): снимок «как было ДО
// последнего применения» + владелец. steamID здесь не для приватности, а от повторного
// использования слота: ClearSlotState зовётся из KZHUDService::Reset(), но проверка владельца
// делает слот безопасным даже если этот путь когда-нибудь перестанут звать.
namespace
{
	struct UndoSlot
	{
		u64 steamID {};
		std::string snapshot;
	};
} // namespace

static_global UndoSlot s_undo[MAXPLAYERS + 1];

// Штамп последней выдачи кода на слот. realtime, а не curtime: curtime обнуляется на смене
// карты, и переживший её штамп оказался бы «в будущем», заглушив команду до конца сессии (тот
// же разбор — CanRunCommand, utils/simplecmds.cpp).
static_global f32 s_lastShareTime[MAXPLAYERS + 1] {};

#define HUDSHARE_STORE_COOLDOWN 5.0f

static_function UndoSlot *GetUndoSlot(KZPlayer *player)
{
	if (!player)
	{
		return NULL;
	}
	const i32 slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return NULL;
	}
	return &s_undo[slot];
}

// Порог «элемент виден» и минимум, который выставляет пол по видимости (§5.2). Прозрачность
// хранится в процентах (0 — не видно, 100 — непрозрачно), см. layout/prefs.cpp.
#define HUDSHARE_MIN_VISIBLE_OPACITY 10
#define HUDSHARE_FLOOR_OPACITY       50

// Алфавит кода: 29 символов без похожих пар (выброшены 0/O, 1/I/L, 5/S). Регистр не значим —
// NormalizeCode приводит ввод к верхнему, поэтому код регистронезависим на ЛЮБОЙ базе, а не
// за счёт коллации MySQL. 29^6 = 594 млн вариантов на код длиной 6.
static_global const char kCodeAlphabet[] = "2346789ABCDEFGHJKMNPQRTUVWXYZ";
static_global const i32 kCodeAlphabetSize = (i32)(sizeof(kCodeAlphabet) - 1);

// Ограничители разбора: снимок приходит из БД и (в соседней задаче) из консоли игрока.
#define HUDSHARE_MAX_SNAPSHOT_LEN 8192
#define HUDSHARE_MAX_PAIRS        256
// Сколько отдельных ключей печатать в лог при разборе битого снимка, прежде чем ограничиться
// сводкой: подробность нужна для разбора, но не 64 строки на одну команду.
#define HUDSHARE_MAX_KEY_LOGS 8

static_global std::vector<KZ::prefs::Entry> s_keys;
static_global bool s_keysBuilt = false;
static_global u32 s_fingerprint = 0;

static_function bool IsBlocked(const char *key)
{
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(kHudShareBlocklist); i++)
	{
		if (V_strcmp(key, kHudShareBlocklist[i]) == 0)
		{
			return true;
		}
	}
	return false;
}

const std::vector<KZ::prefs::Entry> &KZ::hudshare::GetKeys()
{
	if (s_keysBuilt)
	{
		return s_keys;
	}

	std::vector<KZ::prefs::Entry> all;
	if (!KZ::prefs::CollectCategory(KZ::hudshare::HUD_CATEGORY_KEY, all))
	{
		// Категория ещё не зарегистрирована (или ключ разошёлся с InitMenuPrefs) — обмен
		// физически пуст. Отказ громкий: тихий пустой список выглядел бы как «применено 0 из 0».
		// Флаг НЕ латчим: иначе один неудачный вызов (например до InitMenuPrefs) похоронил бы
		// обмен до выгрузки плагина, а GetSchemaFingerprint() навсегда отдавал бы 0.
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_registry_missing reason=category_not_found key=%s\n", KZ::hudshare::HUD_CATEGORY_KEY);
		return s_keys;
	}
	s_keysBuilt = true;

	std::string fingerprintSource;
	for (const KZ::prefs::Entry &entry : all)
	{
		if (IsBlocked(entry.key))
		{
			continue;
		}
		// Векторных настроек в худе нет, а кодировать их в снимке (пробелы внутри значения)
		// значило бы усложнить формат ради несуществующего пункта. Появится — увидим в логе.
		if (entry.storage == KZOptStorage::Vector)
		{
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_key_skipped reason=storage_unsupported key=%s\n", entry.key);
			continue;
		}
		s_keys.push_back(entry);
		fingerprintSource += entry.key;
		fingerprintSource += ';';
	}
	s_fingerprint = CRC32_ProcessSingleBuffer(fingerprintSource.c_str(), (int)fingerprintSource.length());
	KZ_LOG_INFO(LogChannel::Option, "[cyb] hud_share_registry_built keys=%i schema_version=%i fingerprint=%08x\n", (i32)s_keys.size(),
				(i32)KZ::hudshare::SNAPSHOT_VERSION, s_fingerprint);
	return s_keys;
}

u32 KZ::hudshare::GetSchemaFingerprint()
{
	KZ::hudshare::GetKeys();
	return s_fingerprint;
}

void KZ::hudshare::Cleanup()
{
	s_keys.clear();
	s_keysBuilt = false;
	s_fingerprint = 0;
	for (i32 i = 0; i <= MAXPLAYERS; i++)
	{
		s_undo[i] = UndoSlot();
		s_lastShareTime[i] = 0.0f;
	}
}

void KZ::hudshare::ClearSlotState(CPlayerSlot slot)
{
	const i32 index = slot.Get();
	if (index < 0 || index > MAXPLAYERS)
	{
		return;
	}
	s_undo[index] = UndoSlot();
	s_lastShareTime[index] = 0.0f;
}

f32 KZ::hudshare::TakeShareCooldown(KZPlayer *player)
{
	if (!player)
	{
		return 0.0f;
	}
	const i32 slot = player->GetPlayerSlot().Get();
	if (slot < 0 || slot > MAXPLAYERS)
	{
		return 0.0f;
	}
	const f32 now = g_pKZUtils->GetServerGlobals()->realtime;
	const f32 last = s_lastShareTime[slot];
	// now < last — часы пошли назад (смена карты): кулдаун считаем снятым, а не гигантским.
	if (last != 0.0f && now >= last)
	{
		const f32 remaining = HUDSHARE_STORE_COOLDOWN - (now - last);
		if (remaining > 0.0f)
		{
			return remaining;
		}
	}
	s_lastShareTime[slot] = now;
	return 0.0f;
}

// === Код обмена ==============================================================================

bool KZ::hudshare::NormalizeCode(const char *in, char *out, i32 outLen)
{
	if (!in || !out || outLen <= KZ::hudshare::CODE_LENGTH)
	{
		return false;
	}
	i32 written = 0;
	for (const char *c = in; *c; c++)
	{
		if (*c == ' ' || *c == '-' || *c == '_')
		{
			continue; // игрок мог переписать код с разделителем
		}
		if (written >= KZ::hudshare::CODE_LENGTH)
		{
			return false;
		}
		const char upper = (char)toupper((unsigned char)*c);
		if (!V_strnchr(kCodeAlphabet, upper, kCodeAlphabetSize))
		{
			return false;
		}
		out[written++] = upper;
	}
	out[written] = '\0';
	return written == KZ::hudshare::CODE_LENGTH;
}

void KZ::hudshare::GenerateCode(char *out, i32 outLen)
{
	// RandomInt — игровой RNG (тот же, что у подсказок, kz/tip/kz_tip.cpp). Секретности от кода
	// не требуется: он не даёт доступа ни к чему, кроме публично показанного худа, а
	// подобрать чужой код проще, чем угадать — попросить.
	const i32 length = MIN(KZ::hudshare::CODE_LENGTH, outLen - 1);
	for (i32 i = 0; i < length; i++)
	{
		out[i] = kCodeAlphabet[RandomInt(0, kCodeAlphabetSize - 1)];
	}
	out[length] = '\0';
}

// === Снимок ==================================================================================

// Разделители формата внутри значения/ключа сделали бы снимок неразбираемым. Все реальные
// значения белого списка — числа и слаги шрифтов, так что это проверка инварианта, а не фильтр.
static_function bool IsCleanToken(const char *token)
{
	for (const char *c = token; *c; c++)
	{
		if (*c == '|' || *c == '=' || (unsigned char)*c < 0x20)
		{
			return false;
		}
	}
	return true;
}

bool KZ::hudshare::Capture(KZPlayer *from, std::string &out)
{
	out.clear();
	if (!from || !from->optionService)
	{
		return false;
	}
	// Fail-closed: у игрока, чьи префы ещё не приехали из БД, prefKV пуст, и «снимок» был бы
	// набором дефолтов, выданным за его худ (тот же класс дефекта, что fail-open CyberSkins).
	if (!from->optionService->IsLoaded())
	{
		return false;
	}
	const std::vector<KZ::prefs::Entry> &keys = KZ::hudshare::GetKeys();
	if (keys.empty())
	{
		return false;
	}

	char header[32];
	V_snprintf(header, sizeof(header), "kzhud%i.%08x", (i32)KZ::hudshare::SNAPSHOT_VERSION, KZ::hudshare::GetSchemaFingerprint());
	out = header;

	char value[256];
	for (const KZ::prefs::Entry &entry : keys)
	{
		if (!KZ::prefs::ReadValue(from, entry, value, sizeof(value)))
		{
			// Ни значения, ни дефолта — пункт в снимок не попадает, у получателя останется
			// дефолт (см. шапку файла). Отказ видимый, не молчаливый.
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_capture_skipped reason=no_value key=%s steam_id=%llu\n", entry.key,
						from->GetSteamId64(false));
			continue;
		}
		// Пустое значение писателю и читателю нельзя: ParseSnapshot считает `|ключ=` битым
		// снимком и отвергает ВЕСЬ снимок (писатель и читатель обязаны сходиться). Единственный
		// источник пустого — строковый преф, сохранённый как "" (шрифт), поэтому подставляем
		// дефолт пункта.
		if (!value[0])
		{
			KZ::prefs::ReadDefaultValue(entry, value, sizeof(value));
		}
		if (!value[0])
		{
			// И дефолта нет — ключ в снимок не попадает, у получателя он встанет дефолтом сам.
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_capture_skipped reason=empty key=%s steam_id=%llu\n", entry.key,
						from->GetSteamId64(false));
			continue;
		}
		if (!IsCleanToken(entry.key) || !IsCleanToken(value))
		{
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_capture_skipped reason=token_charset key=%s steam_id=%llu\n", entry.key,
						from->GetSteamId64(false));
			continue;
		}
		out += '|';
		out += entry.key;
		out += '=';
		out += value;
	}
	return true;
}

namespace
{
	struct SnapshotPair
	{
		std::string key;
		std::string value;
	};
} // namespace

// Разбор снимка ЦЕЛИКОМ до первой записи: отказ на любом уровне (версия, длина, синтаксис)
// не должен оставить получателя с половиной чужого худа.
static_function bool ParseSnapshot(const char *snapshot, u32 &fingerprint, std::vector<SnapshotPair> &pairs, const char *&reason)
{
	reason = "";
	pairs.clear();
	fingerprint = 0;
	if (!snapshot || !snapshot[0])
	{
		reason = "empty";
		return false;
	}
	const i32 length = (i32)V_strlen(snapshot);
	if (length > HUDSHARE_MAX_SNAPSHOT_LEN)
	{
		reason = "too_long";
		return false;
	}
	static const char kMagic[] = "kzhud";
	const i32 magicLen = (i32)(sizeof(kMagic) - 1);
	if (V_strncmp(snapshot, kMagic, magicLen) != 0)
	{
		reason = "bad_magic";
		return false;
	}
	const char *cursor = snapshot + magicLen;
	i32 version = 0;
	if (*cursor < '0' || *cursor > '9')
	{
		reason = "bad_version";
		return false;
	}
	while (*cursor >= '0' && *cursor <= '9')
	{
		version = version * 10 + (*cursor - '0');
		cursor++;
	}
	if (version != KZ::hudshare::SNAPSHOT_VERSION)
	{
		reason = "version_unsupported";
		return false;
	}
	if (*cursor != '.')
	{
		reason = "bad_header";
		return false;
	}
	cursor++;
	// Ровно 8 hex-символов отпечатка.
	for (i32 i = 0; i < 8; i++)
	{
		const char c = cursor[i];
		i32 digit;
		if (c >= '0' && c <= '9')
		{
			digit = c - '0';
		}
		else if (c >= 'a' && c <= 'f')
		{
			digit = 10 + (c - 'a');
		}
		else if (c >= 'A' && c <= 'F')
		{
			digit = 10 + (c - 'A');
		}
		else
		{
			reason = "bad_fingerprint";
			return false;
		}
		fingerprint = (fingerprint << 4) | (u32)digit;
	}
	cursor += 8;

	while (*cursor)
	{
		if (*cursor != '|')
		{
			reason = "malformed";
			return false;
		}
		cursor++;
		const char *eq = cursor;
		while (*eq && *eq != '=' && *eq != '|')
		{
			eq++;
		}
		if (*eq != '=' || eq == cursor)
		{
			reason = "malformed";
			return false;
		}
		const char *valueStart = eq + 1;
		const char *valueEnd = valueStart;
		while (*valueEnd && *valueEnd != '|')
		{
			valueEnd++;
		}
		if (valueEnd == valueStart)
		{
			reason = "malformed";
			return false;
		}
		if ((i32)pairs.size() >= HUDSHARE_MAX_PAIRS)
		{
			reason = "too_many_keys";
			return false;
		}
		SnapshotPair pair;
		pair.key.assign(cursor, (size_t)(eq - cursor));
		pair.value.assign(valueStart, (size_t)(valueEnd - valueStart));
		// Вход НЕДОВЕРЕННЫЙ (сейчас из БД, с соседней задачей — из консоли игрока): тот же
		// инвариант, что у писателя. Иначе ключ с управляющим символом (например \n) уехал бы
		// в строку лога и сломал машинный разбор reason=/key=.
		if (!IsCleanToken(pair.key.c_str()) || !IsCleanToken(pair.value.c_str()))
		{
			reason = "token_charset";
			return false;
		}
		pairs.push_back(pair);
		cursor = valueEnd;
	}
	if (pairs.empty())
	{
		reason = "no_keys";
		return false;
	}
	return true;
}

// === Предохранители применения ===============================================================

// Альфа цвета в панораме НЕ рисуется (panorama::ResolveColorClass её отбрасывает, а значение 1
// зарезервировано как метка градиента, panorama_tables.cpp:365). Сохранённый ноль поэтому
// бессмыслен, но спека §5.2 прямо называет его способом получить пустой экран — нормализуем,
// а не отвергаем, чтобы остальной снимок всё равно применился.
static_function i32 NormalizeColorAlpha(KZPlayer *player, const std::vector<KZ::prefs::Entry> &keys)
{
	auto *opts = player->optionService;
	i32 fixed = 0;
	for (const KZ::prefs::Entry &entry : keys)
	{
		if (!entry.item || entry.item->type != KZOptItemType::Color)
		{
			continue;
		}
		const i64 packed = opts->GetPreferenceInt(entry.key, 0);
		if ((packed & 0xFF) != 0)
		{
			continue;
		}
		opts->SetPreferenceInt(entry.key, (packed & ~(i64)0xFF) | 0xFF);
		fixed++;
	}
	return fixed;
}

// Пол по видимости (§5.2): чужой набор может ЛЕГАЛЬНЫМИ значениями дать пустой экран. Порог
// применяется ТОЛЬКО на импорте: в меню игрок имеет право сделать себе невидимый худ, чужая
// строка — нет.
static_function bool ApplyVisibilityFloor(KZPlayer *player)
{
	auto *opts = player->optionService;
	for (i32 e = 0; e < (i32)LayoutElement::Count; e++)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[e];
		if (opts->GetPreferenceBool(def.enabledKey, true) && opts->GetPreferenceInt(def.opacityKey, 100) >= HUDSHARE_MIN_VISIBLE_OPACITY)
		{
			return false;
		}
	}
	// Минимум — таймер и скорость: без них экран пуст, а это два элемента, из-за которых худ
	// вообще включают.
	const LayoutElement floorElements[] = {LayoutElement::Timer, LayoutElement::Speed};
	for (i32 i = 0; i < (i32)KZ_ARRAYSIZE(floorElements); i++)
	{
		const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)floorElements[i]];
		opts->SetPreferenceBool(def.enabledKey, true);
		if (opts->GetPreferenceInt(def.opacityKey, 100) < HUDSHARE_FLOOR_OPACITY)
		{
			opts->SetPreferenceInt(def.opacityKey, HUDSHARE_FLOOR_OPACITY);
		}
	}
	return true;
}

static_function const char *SourceName(KZ::hudshare::Source source)
{
	switch (source)
	{
		case KZ::hudshare::Source::ShareCode:
			return "share_code";
		case KZ::hudshare::Source::SpecTake:
			return "spec_take";
		case KZ::hudshare::Source::Undo:
			return "undo";
		case KZ::hudshare::Source::Console:
			return "console";
	}
	return "unknown";
}

// === Применение ==============================================================================

KZ::hudshare::ApplyStats KZ::hudshare::Apply(KZPlayer *to, const char *snapshot, KZ::hudshare::Source source, const char *sourceDetail)
{
	KZ::hudshare::ApplyStats stats;
	const char *detail = sourceDetail ? sourceDetail : "-";
	if (!to || !to->optionService || !to->hudService)
	{
		stats.reason = "no_player";
		return stats;
	}
	KZOptionService *opts = to->optionService;
	const u64 steamID = to->GetSteamId64(false);

	// Fail-closed по гейту записи префов. isSetUp (db/save_prefs.cpp) открывается ровно по
	// IsLoaded() (db/setup_client.cpp), поэтому проверяем его же: без этой ветки запись ушла бы
	// в пустой prefKV, флаш молча съелся бы гейтом, а игрок увидел бы «применено» без
	// применения — и потерял бы настоящие настройки при следующем сохранении.
	if (!opts->IsLoaded())
	{
		stats.reason = "prefs_not_loaded";
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_apply_failed reason=%s source=%s detail=%s steam_id=%llu\n", stats.reason,
					SourceName(source), detail, steamID);
		to->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return stats;
	}

	const std::vector<KZ::prefs::Entry> &keys = KZ::hudshare::GetKeys();
	if (keys.empty())
	{
		stats.reason = "registry_empty";
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_apply_failed reason=%s source=%s detail=%s steam_id=%llu\n", stats.reason,
					SourceName(source), detail, steamID);
		to->languageService->PrintChat(true, false, "HUD Share - Not Ready");
		return stats;
	}

	u32 fingerprint = 0;
	std::vector<SnapshotPair> pairs;
	const char *reason = "";
	if (!ParseSnapshot(snapshot, fingerprint, pairs, reason))
	{
		stats.reason = reason;
		KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_apply_failed reason=%s source=%s detail=%s steam_id=%llu\n", stats.reason,
					SourceName(source), detail, steamID);
		if (V_strcmp(reason, "version_unsupported") == 0)
		{
			to->languageService->PrintChat(true, false, "HUD Share - Version Unsupported");
		}
		else
		{
			to->languageService->PrintChat(true, false, "HUD Share - Apply Failed", reason);
		}
		return stats;
	}
	stats.schemaDrift = fingerprint != KZ::hudshare::GetSchemaFingerprint();

	// Отбор и валидация ДО первой записи. Индексы parallel к keys: -1 — ключа в снимке нет.
	std::vector<i32> chosen(keys.size(), -1);
	std::vector<bool> matched(pairs.size(), false);
	i32 keyLogs = 0;
	for (size_t k = 0; k < keys.size(); k++)
	{
		for (size_t p = 0; p < pairs.size(); p++)
		{
			if (matched[p] || V_strcmp(pairs[p].key.c_str(), keys[k].key) != 0)
			{
				continue;
			}
			matched[p] = true;
			const char *invalidReason = "";
			if (!KZ::prefs::ValidateValue(to, keys[k], pairs[p].value.c_str(), invalidReason))
			{
				stats.invalid++;
				if (keyLogs++ < HUDSHARE_MAX_KEY_LOGS)
				{
					KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_key_rejected reason=%s key=%s source=%s steam_id=%llu\n", invalidReason,
								keys[k].key, SourceName(source), steamID);
				}
				break;
			}
			chosen[k] = (i32)p;
			break;
		}
	}
	// Ключи снимка, которых в белом списке нет: старая/чужая сборка, либо правленный руками
	// снимок. Пропускаем, но видимо.
	for (size_t p = 0; p < pairs.size(); p++)
	{
		if (matched[p])
		{
			continue;
		}
		stats.unknown++;
		if (keyLogs++ < HUDSHARE_MAX_KEY_LOGS)
		{
			KZ_LOG_WARN(LogChannel::Option, "[cyb] hud_share_key_unknown reason=not_in_whitelist key=%s source=%s steam_id=%llu\n",
						pairs[p].key.c_str(), SourceName(source), steamID);
		}
	}

	// Слот отката снимается ДО записи и живёт В ПАМЯТИ сессии (обоснование — hud_share.h):
	// в Players.Preferences ему нельзя, поле общее на все настройки и уже близко к лимиту.
	// Откат сам себе слота не пишет: иначе !hudundo стал бы переключателем двух состояний.
	std::string previous;
	const bool savePrevious = (source != KZ::hudshare::Source::Undo) && KZ::hudshare::Capture(to, previous);

	i32 colorAlphaFixed = 0;
	bool floored = false;
	{
		// Один пакет на всё применение: без него 64 ключа дают 64 полных сериализации prefKV и
		// 64 UPDATE в ОБЩУЮ MySQL флота (одна на 12 инстансов). Флаш делает деструктор.
		KZOptionService::BatchScope batch(opts);

		char defaultValue[256];
		for (size_t k = 0; k < keys.size(); k++)
		{
			// Ключ снимка не прошёл валидацию (уже посчитан в invalid) — дефолт ему ставим, но
			// в defaulted НЕ считаем: одна и та же настройка не должна попасть и в «пропущено»,
			// и в «сброшено на стандарт» отчёта игроку.
			bool countAsDefaulted = chosen[k] < 0;
			if (chosen[k] >= 0)
			{
				if (KZ::prefs::ApplyValue(to, keys[k], pairs[(size_t)chosen[k]].value.c_str()))
				{
					stats.applied++;
					continue;
				}
				// Валидация прошла, а запись нет — расхождение внутри реестра, а не вина снимка.
				stats.invalid++;
				countAsDefaulted = false;
				KZ_LOG_ERROR(LogChannel::Option, "[cyb] hud_share_key_write_failed reason=apply_value key=%s steam_id=%llu\n", keys[k].key, steamID);
			}
			// Ключа в снимке нет (или он не применился) — ставим ДЕФОЛТ пункта, а не оставляем
			// своё значение: иначе получатель получил бы смесь двух худов (см. шапку файла).
			if (KZ::prefs::ReadDefaultValue(keys[k], defaultValue, sizeof(defaultValue)) && KZ::prefs::ApplyValue(to, keys[k], defaultValue)
				&& countAsDefaulted)
			{
				stats.defaulted++;
			}
		}

		colorAlphaFixed = NormalizeColorAlpha(to, keys);
		floored = ApplyVisibilityFloor(to);
	}

	// Слот отката — после пакета: он вне префов, в БД не уезжает, и записывать его надо только
	// когда применение реально состоялось.
	if (savePrevious)
	{
		if (UndoSlot *undo = GetUndoSlot(to))
		{
			undo->steamID = steamID;
			undo->snapshot = previous;
		}
	}
	else if (source == KZ::hudshare::Source::Undo)
	{
		// Слот израсходован: второй !hudundo обязан сказать «нечего откатывать», а не вернуть
		// худ, который только что откатили. Гасим ТОЛЬКО снимок — кулдаун выдачи кода к откату
		// отношения не имеет.
		if (UndoSlot *undo = GetUndoSlot(to))
		{
			*undo = UndoSlot();
		}
	}

	stats.floored = floored;
	stats.colorAlphaFixed = colorAlphaFixed;
	stats.ok = true;

	// Интерн-пул сущности custom_hud_layout не освобождает строки (лимит 1024, ~22 строки на
	// смену раскладки, отказ SetHasClass молчаливый) — импорт эффективный источник префов НЕ
	// меняет, поэтому штатный триггер пересоздания (layoutMimicSource, layout/entity.cpp) здесь
	// не сработает, и звать снос надо явно. Он же обнуляет кэши классов элементов.
	to->hudService->DestroyOwnedLayout();
	// Кэш префов: без него игрок увидит новый худ только после перезахода.
	to->hudService->RefreshLayoutPrefs();

	KZ_LOG_INFO(LogChannel::Option,
				"[cyb] hud_share_applied source=%s detail=%s steam_id=%llu applied=%i defaulted=%i unknown=%i invalid=%i "
				"schema_drift=%i floored=%i alpha_fixed=%i keys=%i\n",
				SourceName(source), detail, steamID, stats.applied, stats.defaulted, stats.unknown, stats.invalid, stats.schemaDrift ? 1 : 0,
				floored ? 1 : 0, colorAlphaFixed, (i32)keys.size());

	// Отчёт игроку. Честный: «применено 64 из 64» и «применено 60 из 64» — разные строки.
	const i32 total = (i32)keys.size();
	if (stats.invalid > 0 || stats.unknown > 0 || stats.defaulted > 0)
	{
		to->languageService->PrintChat(true, false, "HUD Share - Applied Partial", stats.applied, total, stats.invalid + stats.unknown,
									   stats.defaulted);
	}
	else if (source == KZ::hudshare::Source::Undo)
	{
		to->languageService->PrintChat(true, false, "HUD Share - Undone");
	}
	else
	{
		to->languageService->PrintChat(true, false, "HUD Share - Applied", stats.applied, total);
	}
	if (stats.schemaDrift)
	{
		to->languageService->PrintChat(true, false, "HUD Share - Schema Drift");
	}
	if (stats.floored)
	{
		// Только реальный пол по видимости. Нормализация нулевой альфы цвета сюда НЕ входит:
		// альфа в панораме не рисуется вовсе (ResolveColorClass её отбрасывает), и сообщать
		// игроку «не было видно ни одного элемента» из-за неё значило бы рассказать о том,
		// чего не было. Она остаётся в логе (alpha_fixed=N).
		to->languageService->PrintChat(true, false, "HUD Share - Visibility Floor");
	}
	if (savePrevious)
	{
		to->languageService->PrintChat(true, false, "HUD Share - Undo Hint");
	}
	return stats;
}

bool KZ::hudshare::HasUndo(KZPlayer *player)
{
	const UndoSlot *undo = GetUndoSlot(player);
	// Владелец обязан совпасть: слот индексируется номером игрового слота, а его переиспользует
	// следующий игрок (ClearSlotState из KZHUDService::Reset это и делает — проверка держит
	// инвариант, а не лечит известный случай).
	return undo && !undo->snapshot.empty() && undo->steamID == player->GetSteamId64(false);
}

KZ::hudshare::ApplyStats KZ::hudshare::ApplyUndo(KZPlayer *player)
{
	KZ::hudshare::ApplyStats stats;
	if (!KZ::hudshare::HasUndo(player))
	{
		stats.reason = "undo_empty";
		return stats;
	}
	// КОПИЯ, а не ссылка в слот: Apply на своём пути гасит слот, и строка из-под нас была бы
	// очищена прямо во время применения.
	const std::string stored = GetUndoSlot(player)->snapshot;
	return KZ::hudshare::Apply(player, stored.c_str(), KZ::hudshare::Source::Undo, "session_slot");
}
