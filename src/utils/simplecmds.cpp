
#include "common.h"
#include "utils/utils.h"
#include "simplecmds.h"
#include "../kz/kz.h"
#include "../kz/language/kz_language.h"
#include "../kz/option/kz_option.h"
#include "utils/tables.h"

#include <cctype>

#include "tier0/memdbgon.h"
// private structs
#define SCMD_MAX_NAME_LEN 128
// Буфер под ремап ЦЕЛОЙ чат-строки (имя команды + аргументы), см. OnDispatchConCommand.
// Движок режет say длиннее 512 байт, а ремап кириллицы только СОКРАЩАЕТ строку (2 байта UTF-8
// на 1 байт латиницы), так что усечения здесь быть не может.
#define SCMD_MAX_CHAT_LEN 512

struct Scmd
{
	bool hasConsolePrefix;
	i32 nameLength;
	char name[SCMD_MAX_NAME_LEN];
	scmd::Callback_t *callback;
	char descKey[SCMD_MAX_NAME_LEN];
	u64 flags;
};

// clang-format off
const char* cmdFlagNames[] = {
	"Checkpoint",
	"Record",
	"Jumpstats",
	"Measure",
	"ModeStyle",
	"Preference",
	"Racing",
	"Replay",
	"Saveloc",
	"Spec",
	"Status",
	"Timer",
	"Player",
	"Global",
	"Misc",
	"Map",
	"HUD"
};

static_global const char *columnKeys[] = {
	"Command List Header - Name",
	"Command List Header - Description"
};

// clang-format on

struct ScmdManager
{
	i32 cmdCount;
	Scmd cmds[SCMD_MAX_CMDS];
};

static_global ScmdManager g_cmdManager = {};

static_global void PrintCategoryCommands(KZPlayer *player, i32 category, bool printEmpty)
{
	char tableName[64];
	V_snprintf(tableName, sizeof(tableName), "Command List - %s", cmdFlagNames[category]);
	CUtlString headers[KZ_ARRAYSIZE(columnKeys)];
	for (u32 i = 0; i < KZ_ARRAYSIZE(columnKeys); i++)
	{
		headers[i] = player->languageService->PrepareMessage(columnKeys[i]).c_str();
	}
	Scmd *cmds = g_cmdManager.cmds;
	utils::Table<KZ_ARRAYSIZE(columnKeys)> table(player->languageService->PrepareMessage(tableName).c_str(), headers);

	u32 cmdCount = 0;
	CUtlVector<CUtlString> uniqueCallbacks;
	CUtlVector<i32> callbackRowIndices;
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (cmds[i].flags & (1ull << category))
		{
			i32 existingIndex = uniqueCallbacks.Find(cmds[i].descKey);
			if (existingIndex == -1)
			{
				uniqueCallbacks.AddToTail(cmds[i].descKey);
				callbackRowIndices.AddToTail(cmdCount);
				table.SetRow(cmdCount, cmds[i].name, player->languageService->PrepareMessage(cmds[i].descKey).c_str());
				cmdCount++;
			}
			else
			{
				i32 rowIndex = callbackRowIndices[existingIndex];
				CUtlString newEntry = table.GetEntry(rowIndex).data[0];
				// Remove the trailing space that exists in each column
				newEntry.SetLength(newEntry.Length() - strlen("ᅟ"));
				newEntry.Append("/");
				newEntry.Append(cmds[i].name);
				table.Set(rowIndex, 0, newEntry);
			}
		}
	}
	if (!printEmpty && cmdCount == 0)
	{
		return;
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
	player->PrintConsole(false, false, table.GetTitle());
	player->PrintConsole(false, false, table.GetHeader());

	for (u32 i = 0; i < table.GetNumEntries(); i++)
	{
		player->PrintConsole(false, false, table.GetLine(i));
	}
	player->PrintConsole(false, false, table.GetSeparator("="));
}

// Whitelist чат-версии !help: только отобранные команды, сгруппированные по
// категориям. Имена — консольные (kz_...), именно тех вариантов, что хотим видеть
// в чате (для алиасов — сам алиас, напр. kz_cp / kz_maptop / kz_sg). Показывается
// строка, только если команда есть в реестре И помечена флагом SCFL_HELP; иначе
// (режим не загружен / флаг снят) молча пропускается. Порядок и группировку задаёт
// таблица ниже, а видимость — флаг: снятие SCFL_HELP убирает команду из !help.
// clang-format off
static_global const char *helpCheckpoint[] = {"kz_cp", "kz_tp", "kz_undo", "kz_pcp", "kz_ncp", "kz_ssp", "kz_csp"};
static_global const char *helpTimer[]      = {"kz_stop", "kz_pause", "kz_r"};
static_global const char *helpRecords[]    = {"kz_pb", "kz_wr", "kz_maptop", "kz_rank"};
static_global const char *helpReplay[]     = {"kz_replay", "kz_rpmenu"};
static_global const char *helpMap[]        = {"kz_courses", "kz_mapinfo", "kz_tier", "kz_end", "kz_lj"};
static_global const char *helpMode[]       = {"kz_kzt", "kz_ckz", "kz_vnl"};
static_global const char *helpSpec[]       = {"kz_spec", "kz_goto"};
static_global const char *helpMeasure[]    = {"kz_measure", "kz_measurestart", "kz_measureend", "kz_measureblock", "kz_ztopwatch"};
static_global const char *helpSafeguard[]  = {"kz_sg", "kz_pro"};
static_global const char *helpMisc[]       = {"kz_fov", "kz_beam", "kz_options", "kz_language", "kz_globalcheck", "kz_help"};

struct HelpCategory
{
	const char *titleKey; // ключ фразы заголовка категории
	const char **names;   // консольные имена команд в порядке показа
	i32 count;
};

static_global const HelpCategory helpCategories[] = {
	{"Help Category - Checkpoints", helpCheckpoint, (i32)KZ_ARRAYSIZE(helpCheckpoint)},
	{"Help Category - Timer",       helpTimer,      (i32)KZ_ARRAYSIZE(helpTimer)},
	{"Help Category - Records",     helpRecords,    (i32)KZ_ARRAYSIZE(helpRecords)},
	{"Help Category - Replay",      helpReplay,     (i32)KZ_ARRAYSIZE(helpReplay)},
	{"Help Category - Map",         helpMap,        (i32)KZ_ARRAYSIZE(helpMap)},
	{"Help Category - Mode",        helpMode,       (i32)KZ_ARRAYSIZE(helpMode)},
	{"Help Category - Spectate",    helpSpec,       (i32)KZ_ARRAYSIZE(helpSpec)},
	{"Help Category - Measure",     helpMeasure,    (i32)KZ_ARRAYSIZE(helpMeasure)},
	{"Help Category - Safeguard",   helpSafeguard,  (i32)KZ_ARRAYSIZE(helpSafeguard)},
	{"Help Category - Misc",        helpMisc,       (i32)KZ_ARRAYSIZE(helpMisc)},
};
// clang-format on

// Ищет команду в реестре по консольному имени (регистронезависимо). nullptr — нет.
static_function Scmd *FindCmdByName(const char *name)
{
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (!V_stricmp(g_cmdManager.cmds[i].name, name))
		{
			return &g_cmdManager.cmds[i];
		}
	}
	return nullptr;
}

// Печатает игроку whitelist-список команд в чат: заголовок категории, затем
// по строке на команду в формате «!команда — описание» (команда зелёным 0x04,
// описание белым/default 0x01). Одна команда = одна строка, поэтому в 512-байтный
// буфер PrintChat помещается с запасом.
static_function void PrintChatCommandList(KZPlayer *player)
{
	if (!player)
	{
		return;
	}
	player->languageService->PrintChat(true, false, "Command List - Chat Header");

	for (i32 c = 0; c < (i32)KZ_ARRAYSIZE(helpCategories); c++)
	{
		const HelpCategory &cat = helpCategories[c];
		bool headerPrinted = false;
		for (i32 n = 0; n < cat.count; n++)
		{
			Scmd *cmd = FindCmdByName(cat.names[n]);
			if (!cmd || !(cmd->flags & SCFL_HELP))
			{
				continue; // не зарегистрирована или не в whitelist
			}
			if (!headerPrinted)
			{
				player->languageService->PrintChat(false, false, cat.titleKey);
				headerPrinted = true;
			}
			const char *chatName = cmd->hasConsolePrefix ? cmd->name + strlen(SCMD_CONSOLE_PREFIX) : cmd->name;
			std::string desc = player->languageService->PrepareMessage(cmd->descKey);
			// Описание уходит аргументом %s — любые '%' внутри него безопасны.
			player->PrintChat(false, false, "{green}!%s{default} — %s", chatName, desc.c_str());
		}
	}
}

SCMD(kz_help, SCFL_MISC | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	// Голый !help — быстрый список всех !-команд прямо в чат этому игроку
	// (детальные таблицы по категориям по-прежнему уходят в консоль ниже).
	if (args->ArgC() < 2)
	{
		PrintChatCommandList(player);
	}
	player->languageService->PrintChat(true, false, "Command Help Response (Chat)");
	player->languageService->PrintConsole(false, false, "Command Help Response (Console)");
	u64 category = 0;
	bool foundCategory {};
	if (args->ArgC() >= 2)
	{
		for (i32 i = 1; i < args->ArgC(); i++)
		{
			for (i32 j = 0; j < KZ_ARRAYSIZE(cmdFlagNames); j++)
			{
				if (!V_stricmp(args->Arg(i), cmdFlagNames[j]))
				{
					PrintCategoryCommands(player, j, true);
					foundCategory = true;
				}
			}
		}
	}

	if (!foundCategory)
	{
		player->languageService->PrintConsole(false, false, "Command Help Response Category Hint (Console)");
		for (i32 i = 0; i < KZ_ARRAYSIZE(cmdFlagNames); i++)
		{
			PrintCategoryCommands(player, i, false);
		}
	}
	return MRES_SUPERCEDE;
}

bool scmd::RegisterCmd(const char *name, scmd::Callback_t *callback, const char *descKey, u64 flags)
{
	Assert(name);
	Assert(callback);
	if (!name || !callback || g_cmdManager.cmdCount >= SCMD_MAX_CMDS)
	{
		// TODO: print warning? error? segfault?
		Assert(0);
		return false;
	}

	i32 nameLength = strlen(name);

	if (nameLength == 0)
	{
		// TODO: print warning? error? segfault?
		Assert(0);
		return false;
	}

	i32 conPrefixLen = strlen(SCMD_CONSOLE_PREFIX);
	bool hasConPrefix = false;
	if (nameLength >= conPrefixLen && V_strnicmp(name, SCMD_CONSOLE_PREFIX, conPrefixLen) == 0)
	{
		if (nameLength == conPrefixLen)
		{
			// name is just the console prefix
			// TODO: print warning? error? segfault?
			Assert(0);
			return false;
		}
		hasConPrefix = true;
	}

	// Check if command with this name already exists
	Scmd *cmds = g_cmdManager.cmds;
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (nameLength != g_cmdManager.cmds[i].nameLength)
		{
			continue;
		}

		if (!V_stricmp(g_cmdManager.cmds[i].name, name))
		{
			// TODO: print warning? error? segfault?
			// Command already exists
			// Assert(0);
			return false;
		}
	}

	// Command name is unique!
	Scmd cmd = {hasConPrefix, nameLength, "", callback, "", flags};
	V_snprintf(cmd.name, SCMD_MAX_NAME_LEN, "%s", name);

	// Check if we want to override the command description.
	if (!descKey)
	{
		V_snprintf(cmd.descKey, SCMD_MAX_NAME_LEN, "Command Description - %s", cmd.name);
	}
	else
	{
		V_snprintf(cmd.descKey, SCMD_MAX_NAME_LEN, "%s", descKey);
	}

	g_cmdManager.cmds[g_cmdManager.cmdCount++] = cmd;

	return true;
}

bool scmd::LinkCmd(const char *name, const char *linkedName, u64 extraFlags)
{
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (!V_stricmp(g_cmdManager.cmds[i].name, linkedName))
		{
			return scmd::RegisterCmd(name, g_cmdManager.cmds[i].callback, g_cmdManager.cmds[i].descKey,
									 g_cmdManager.cmds[i].flags | extraFlags);
		}
	}
	return false;
}

bool scmd::UnregisterCmd(const char *name)
{
	i32 indexToDelete = -1;
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (!V_stricmp(g_cmdManager.cmds[i].name, name))
		{
			indexToDelete = i;
			break;
		}
	}
	if (indexToDelete != -1)
	{
		for (i32 i = indexToDelete; i < g_cmdManager.cmdCount; i++)
		{
			g_cmdManager.cmds[i] = g_cmdManager.cmds[i + 1];
		}
		g_cmdManager.cmdCount--;
		return true;
	}
	return false;
}

META_RES scmd::OnClientCommand(CPlayerSlot &slot, const CCommand &args)
{
	META_RES result = MRES_IGNORED;
	if (!GameEntitySystem())
	{
		return result;
	}

	CCSPlayerController *controller = (CCSPlayerController *)GameEntitySystem()->GetEntityInstance(CEntityIndex((i32)slot.Get() + 1));

	if (!controller || !g_pKZPlayerManager->ToPlayer(controller))
	{
		return MRES_IGNORED;
	}

	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (!g_cmdManager.cmds[i].callback)
		{
			// TODO: error?
			Assert(g_cmdManager.cmds[i].callback);
			continue;
		}

		if (!V_stricmp(g_cmdManager.cmds[i].name, args[0]))
		{
			result = g_cmdManager.cmds[i].callback(controller, &args);
			if (result == MRES_SUPERCEDE)
			{
				return result;
			}
		}
	}
	return result;
}

// Ремап кириллицы в латиницу по ПОЗИЦИИ КЛАВИШ (раскладка ЙЦУКЕН → QWERTY):
// й→q, ц→w, у→e, ... — чтобы команда, набранная в русской раскладке, сматчилась
// как латинская (пример: «ыыз» → «ssp», «рудз» → «help»). ASCII-символы
// копируются как есть (в нижнем регистре). Возвращает true ТОЛЬКО если во входе
// была хотя бы одна кириллическая буква и вся строка успешно перекодирована;
// иначе повторный матч бессмыслен (латиница уже пробовалась как есть).
// Работает и на ЦЕЛОЙ чат-строке с аргументами: пробел и '!' — ASCII, проходят как есть
// («!тщьштфеу ля_лшцше» → «!nominate kz_kiwit»). Приведение к нижнему регистру аргументов
// безвредно: имена режимов и карт всюду сравниваются регистронезависимо.
static_function bool RemapCyrillicToLatin(const char *in, char *out, int outSize)
{
	// Таблица для строчных а(U+0430)..я(U+044F) по позиции клавиши в QWERTY.
	static const char kLayout[32] = {
		/* а */ 'f',  /* б */ ',', /* в */ 'd', /* г */ 'u', /* д */ 'l',  /* е */ 't', /* ж */ ';', /* з */ 'p',
		/* и */ 'b',  /* й */ 'q', /* к */ 'r', /* л */ 'k', /* м */ 'v',  /* н */ 'y', /* о */ 'j', /* п */ 'g',
		/* р */ 'h',  /* с */ 'c', /* т */ 'n', /* у */ 'e', /* ф */ 'a',  /* х */ '[', /* ц */ 'w', /* ч */ 'x',
		/* ш */ 'i',  /* щ */ 'o', /* ъ */ ']', /* ы */ 's', /* ь */ 'm',  /* э */ '\'', /* ю */ '.', /* я */ 'z',
	};

	bool hadCyrillic = false;
	int o = 0;
	const unsigned char *p = (const unsigned char *)in;

	while (*p)
	{
		if (o >= outSize - 1)
		{
			return false; // не влезает — не рискуем частичным матчем
		}
		unsigned char c = *p;
		if (c < 0x80)
		{
			// ASCII (латиница/цифры/подчёркивание) — как есть, в нижний регистр.
			out[o++] = (char)tolower(c);
			p++;
			continue;
		}
		// Двухбайтовая кириллица UTF-8 (ведущий байт 0xD0/0xD1).
		if ((c == 0xD0 || c == 0xD1) && p[1])
		{
			unsigned int cp = ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
			char latin = 0;
			if (cp >= 0x410 && cp <= 0x42F) // прописные А..Я → та же клавиша
			{
				latin = kLayout[cp - 0x410];
			}
			else if (cp >= 0x430 && cp <= 0x44F) // строчные а..я
			{
				latin = kLayout[cp - 0x430];
			}
			else if (cp == 0x401 || cp == 0x451) // Ё/ё
			{
				latin = '`';
			}
			if (latin == 0)
			{
				return false; // кириллица вне таблицы — не наш случай
			}
			out[o++] = latin;
			hadCyrillic = true;
			p += 2;
			continue;
		}
		// Прочий не-ASCII (3+ байта: эмодзи и т.п.) — точно не команда.
		return false;
	}
	out[o] = '\0';
	return hadCyrillic;
}

// Проходит реестр и вызывает колбэки всех команд с чат-именем cmdName (имя без
// kz_-префикса и без триггера). Возвращает true, если хоть одна сматчилась.
// suppress ← true, если чат-строку надо проглотить (тихий триггер '/' либо колбэк
// вернул MRES_SUPERCEDE); на первом же supercede обход прекращается — как в исходной
// логике диспатча.
static_function bool DispatchChatByName(CCSPlayerController *controller, const CCommand &cmdArgs, const char *cmdName, char trigger, bool &suppress)
{
	bool matched = false;
	Scmd *cmds = g_cmdManager.cmds;
	for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
	{
		if (!cmds[i].callback)
		{
			Assert(cmds[i].callback);
			continue;
		}
		const char *name = cmds[i].hasConsolePrefix ? cmds[i].name + strlen(SCMD_CONSOLE_PREFIX) : cmds[i].name;
		if (!V_stricmp(cmdName, name))
		{
			matched = true;
			META_RES res = cmds[i].callback(controller, &cmdArgs);
			if (trigger == SCMD_CHAT_SILENT_TRIGGER || res == MRES_SUPERCEDE)
			{
				suppress = true;
				break;
			}
		}
	}
	return matched;
}

META_RES scmd::OnDispatchConCommand(ConCommandRef cmd, const CCommandContext &ctx, const CCommand &args)
{
	META_RES result = MRES_IGNORED;
	if (!GameEntitySystem())
	{
		return result;
	}
	CPlayerSlot slot = ctx.GetPlayerSlot();

	CCSPlayerController *controller = (CCSPlayerController *)utils::GetController(slot);

	if (!cmd.IsValidRef() || !controller || !g_pKZPlayerManager->ToPlayer(controller))
	{
		return MRES_IGNORED;
	}
	const char *commandName = cmd.GetName();

	if (!V_stricmp(commandName, "say") || !V_stricmp(commandName, "say_team"))
	{
		if (args.ArgC() < 2)
		{
			// no argument somehow
			return MRES_IGNORED;
		}

		if (args[1][0] != SCMD_CHAT_TRIGGER && args[1][0] != SCMD_CHAT_SILENT_TRIGGER)
		{
			// no chat command trigger
			return MRES_IGNORED;
		}

		i32 argLen = strlen(args[1]);
		if (argLen < 1)
		{
			// arg is too short!
			return MRES_IGNORED;
		}

		CCommand cmdArgs;
		cmdArgs.Tokenize(args[1]);

		const char trigger = args[1][0];
		const char *cmdName = cmdArgs[0] + 1; // skip chat trigger

		bool suppress = false;
		bool matched = DispatchChatByName(controller, cmdArgs, cmdName, trigger, suppress);

		// Русская раскладка: если как есть не сматчилось — ремапнуть кириллицу в
		// латиницу по позиции клавиш (ЙЦУКЕН→QWERTY) и попробовать снова. Латинские
		// команды матчатся на первом проходе, поэтому не ломаются (RemapCyrillicToLatin
		// без кириллицы вернёт false).
		//
		// Ремапим ВСЮ строку и токенизируем её заново, а не только имя для поиска в реестре.
		// Причина (баг 21.08, «!сля → этот режим недоступен»): колбэку доставался ИСХОДНЫЙ
		// cmdArgs, а Command_KzModeShort (kz_mode_manager.cpp) берёт имя режима из Arg(0) —
		// то есть из сырой строки чата. Команда находилась, запускалась и уходила в
		// SwitchToMode("сля"). Тем же ремапом закрываются аргументы: «!тщьштфеу ля_лшцше»
		// доезжает до колбэка как «!nominate kz_kiwit».
		// Аргументы ремапятся ТОЛЬКО в этой ветке, то есть когда имя команды само потребовало
		// ремапа (игрок заведомо в русской раскладке). Иначе кириллический аргумент у
		// латинской команды — например ник в «!ban Вася» — превратился бы в мусор.
		// Результат второго прохода дальше не нужен: решение принимает только suppress
		// (его выставляет сама команда), поэтому возврат намеренно не сохраняем.
		if (!matched)
		{
			char remappedLine[SCMD_MAX_CHAT_LEN];
			if (RemapCyrillicToLatin(args[1], remappedLine, sizeof(remappedLine)))
			{
				CCommand remappedArgs;
				remappedArgs.Tokenize(remappedLine);
				// Пустой токенайз (строка из одного триггера) оставил бы cmdArgs[0] нулевым.
				if (remappedArgs.ArgC() > 0 && remappedArgs[0][0] != '\0')
				{
					(void)DispatchChatByName(controller, remappedArgs, remappedArgs[0] + 1, trigger, suppress);
				}
			}
		}

		if (suppress)
		{
			// don't send chat message
			return MRES_SUPERCEDE;
		}

		// Неизвестную команду НЕ проглатываем: `!`-команды есть и у соседних плагинов
		// (GG1MapChooser — !rtv/!maps/!nominate, скины — !ws/!knife, !mcustom), а они
		// разбирают чат из игрового события player_chat, которое возникает только если
		// say реально исполнится. MRES_SUPERCEDE здесь глушил их все разом (cyb.86..100:
		// голосование за карту на флоте было мертво). Своих команд это не касается —
		// они уже обработаны выше. Список !-команд игрок получает по !help.
	}
	else // Are we overriding a console command?
	{
		for (i32 i = 0; i < g_cmdManager.cmdCount; i++)
		{
			Scmd *cmds = g_cmdManager.cmds;
			if (!cmds[i].callback)
			{
				// TODO: error?
				Assert(cmds[i].callback);
				continue;
			}

			const char *cmdName = cmds[i].hasConsolePrefix ? cmds[i].name + strlen(SCMD_CONSOLE_PREFIX) : cmds[i].name;
			if (!V_stricmp(commandName, cmdName))
			{
				META_RES result = g_cmdManager.cmds[i].callback(controller, &args);
				if (result == MRES_SUPERCEDE)
				{
					return result;
				}
			}
		}
	}

	return MRES_IGNORED;
}
