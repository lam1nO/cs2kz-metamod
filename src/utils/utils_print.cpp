#include "usermessages.pb.h"
#include "public/networksystem/inetworkmessages.h"
#include "igameeventsystem.h"
#include "sdk/entity/cbaseplayercontroller.h"
#include "sdk/recipientfilters.h"
#include "utils.h"
#include <string>

#include "tier0/memdbgon.h"

/*
 * Credit to Szwagi
 */

static_function char ConvertColorStringToByte(const char *str, size_t length)
{
	switch (length)
	{
		case 3:
			if (!V_memcmp(str, "red", length))
			{
				return 7;
			}
			break;
		case 4:
			if (!V_memcmp(str, "blue", length))
			{
				return 11;
			}
			if (!V_memcmp(str, "lime", length))
			{
				return 6;
			}
			if (!V_memcmp(str, "grey", length))
			{
				return 8;
			}
			if (!V_memcmp(str, "gold", length))
			{
				return 16;
			}
			break;
		case 5:
			if (!V_memcmp(str, "green", length))
			{
				return 4;
			}
			if (!V_memcmp(str, "grey2", length))
			{
				return 13;
			}
			if (!V_memcmp(str, "olive", length))
			{
				return 5;
			}
			break;
		case 6:
			if (!V_memcmp(str, "purple", length))
			{
				return 3;
			}
			if (!V_memcmp(str, "orchid", length))
			{
				return 14;
			}
			if (!V_memcmp(str, "yellow", length))
			{
				return 9;
			}
			break;
		case 7:
			if (!V_memcmp(str, "default", length))
			{
				return 1;
			}
			if (!V_memcmp(str, "darkred", length))
			{
				return 2;
			}
			break;
		case 8:
			if (!V_memcmp(str, "darkblue", length))
			{
				return 12;
			}
			if (!V_memcmp(str, "lightred", length))
			{
				return 15;
			}
			if (!V_memcmp(str, "bluegrey", length))
			{
				return 10;
			}
			break;
	}
	return 0;
}

bool utils::IsChatColorName(const char *name)
{
	return name && name[0] != '\0' && ConvertColorStringToByte(name, V_strlen(name)) != 0;
}

enum CFormatResult
{
	CFORMAT_NOT_US,
	CFORMAT_OK,
	CFORMAT_OUT_OF_SPACE,
};

struct CFormatContext
{
	const char *current;
	char *result;
	char *result_end;
};

static_function bool HasEnoughSpace(const CFormatContext *ctx, uintptr_t space)
{
	return (uintptr_t)(ctx->result_end - ctx->result) > space;
}

static_function CFormatResult EscapeChars(CFormatContext *ctx)
{
	if (*ctx->current == '{' && *(ctx->current + 1) == '{')
	{
		if (!HasEnoughSpace(ctx, 1))
		{
			return CFORMAT_OUT_OF_SPACE;
		}

		ctx->current += 2;
		*ctx->result++ = '{';
		return CFORMAT_OK;
	}
	return CFORMAT_NOT_US;
}

static_function CFormatResult ParseColors(CFormatContext *ctx)
{
	const char *current = ctx->current;
	if (*current == '{')
	{
		current++;
		const char *start = current;
		while (*current && *current != '}')
		{
			current++;
		}
		if (*current == '}')
		{
			int length = current - start;
			current++;
			if (char byte = ConvertColorStringToByte(start, length); byte)
			{
				if (!HasEnoughSpace(ctx, 1))
				{
					return CFORMAT_OUT_OF_SPACE;
				}

				*ctx->result++ = byte;
				ctx->current = current;
				return CFORMAT_OK;
			}
		}
	}
	return CFORMAT_NOT_US;
}

static_function CFormatResult ReplaceNewlines(CFormatContext *ctx)
{
	if (*ctx->current == '\n')
	{
		if (!HasEnoughSpace(ctx, 3))
		{
			return CFORMAT_OUT_OF_SPACE;
		}

		ctx->current++;
		*ctx->result++ = '\xe2';
		*ctx->result++ = '\x80';
		*ctx->result++ = '\xa9';
		return CFORMAT_OK;
	}
	return CFORMAT_NOT_US;
}

static_function CFormatResult AddSpace(CFormatContext *ctx)
{
	if (!HasEnoughSpace(ctx, 1))
	{
		return CFORMAT_OUT_OF_SPACE;
	}
	*ctx->result++ = ' ';
	return CFORMAT_OK;
}

bool utils::CFormat(char *buffer, u64 buffer_size, const char *text)
{
	assert(buffer_size != 0);

	CFormatContext ctx;
	ctx.current = text;
	ctx.result = buffer;
	ctx.result_end = buffer + buffer_size;

	if (AddSpace(&ctx) != CFORMAT_OK)
	{
		return false;
	}

	while (*ctx.current)
	{
		auto escape_chars = EscapeChars(&ctx);
		if (escape_chars == CFORMAT_OK)
		{
			continue;
		}
		if (escape_chars == CFORMAT_OUT_OF_SPACE)
		{
			return false;
		}

		auto parse_colors = ParseColors(&ctx);
		if (parse_colors == CFORMAT_OK)
		{
			continue;
		}
		if (parse_colors == CFORMAT_OUT_OF_SPACE)
		{
			return false;
		}

		auto replace_newlines = ReplaceNewlines(&ctx);
		if (replace_newlines == CFORMAT_OK)
		{
			continue;
		}
		if (replace_newlines == CFORMAT_OUT_OF_SPACE)
		{
			return false;
		}

		// Everything else
		if (!HasEnoughSpace(&ctx, 1))
		{
			return false;
		}
		*ctx.result++ = *ctx.current++;
	}

	// Null terminate
	if (!HasEnoughSpace(&ctx, 1))
	{
		return false;
	}
	*ctx.result++ = 0;

	return true;
}

// Режим рассылки чата игроков (инцидент 10.09.2026, чат невидим после апдейта CS2 09.09).
// Диагностика saytext2_seen на канарейке: движок шлёт SayText2 id=118 (имя в реестре
// «CUserMessageSayText2 [118]» — поэтому поиск по id, а не по имени класса), chat=1,
// messagename="Cstrike_Chat_All", param1=ник, param2=текст; мы слали chat=0 с кастомным
// messagename без params. Что именно из этого клиент перестал рисовать — решает канарейка
// переключением cvar по RCON, без пересборки (каждая сборка ~40 мин):
//   0 — SayText2 как раньше, но chat=true (кастомный messagename без params);
//   1 — SayText2 как движок: messagename="Cstrike_Chat_All", param1=префикс+ник, param2=текст
//       (param3 = маркер "cyb", чтобы собственный quiet-хук не глушил наше сообщение);
//   2 — TextMsg/HUD_PRINTTALK (путь ответов !pb, доказанно рисуется; без привязки к игроку).
// Дефолт — 2: единственный путь, доказанно живой на флоте; 0/1 — гипотезы, их проверяет
// канарейка по RCON, победитель станет дефолтом отдельным коммитом (+CYBER.md).
CConVar<i32> kz_chat_mode("kz_chat_mode", FCVAR_NONE, "Player chat send mode: 0 saytext2 raw (chat=1), 1 saytext2 token (Cstrike_Chat_All), 2 textmsg (default)",
						  2);

// Сетевое сообщение по id с фолбэком на подстроку имени; nullptr + однократный warn при отказе.
// Точный FindNetworkMessage по имени protobuf-класса на живом сервере НЕ находит: реестр хранит
// имена вида «CUserMessageTextMsg [124]». Реальное имя логируем один раз на сообщение.
struct ResolveLogState
{
	bool warned = false;   // отказ уже залогирован
	bool resolved = false; // успешный резолв уже залогирован (отдельно: отказ не должен глушить будущий успех)
};

static INetworkMessageInternal *ResolveUserMessage(int id, const char *partialName, ResolveLogState &log)
{
	INetworkMessageInternal *netmsg = g_pNetworkMessages->FindNetworkMessageById(id);
	if (!netmsg)
	{
		netmsg = g_pNetworkMessages->FindNetworkMessagePartial(partialName);
	}
	if (!netmsg)
	{
		if (!log.warned)
		{
			log.warned = true;
			Warning("[cyb] print_failed reason=netmsg_not_found name=%s id=%d\n", partialName, id);
		}
		return nullptr;
	}
	if (!log.resolved)
	{
		log.resolved = true;
		NetMessageInfo_t *info = netmsg->GetNetMessageInfo();
		Msg("[cyb] print_netmsg_resolved name=%s id=%d\n", netmsg->GetUnscopedName(), info ? (int)info->m_MessageId : -1);
	}
	return netmsg;
}

void utils::ClientPrintFilter(IRecipientFilter *filter, int msg_dest, const char *msg_name, const char *param1, const char *param2,
							  const char *param3, const char *param4)
{
	static ResolveLogState textMsgLog;
	INetworkMessageInternal *netmsg = ResolveUserMessage(UM_TextMsg, "TextMsg", textMsgLog);
	if (!netmsg)
	{
		return;
	}
	auto msg = netmsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	msg->set_dest(msg_dest);
	msg->add_param(msg_name);
	msg->add_param(param1);
	msg->add_param(param2);
	msg->add_param(param3);
	msg->add_param(param4);

	interfaces::pGameEventSystem->PostEventAbstract(0, false, filter, netmsg, msg, 0);
	delete msg;
}

#define FORMAT_STRING(buffer) \
	va_list args; \
	va_start(args, format); \
	char buffer[512]; \
	vsnprintf(buffer, sizeof(buffer), format, args); \
	va_end(args);

void utils::SayChat(CBaseEntity *entity, const char *format, ...)
{
	if (!entity)
	{
		return;
	}
	FORMAT_STRING(buffer);

	char coloredBuffer[512];
	if (!CFormat(coloredBuffer, sizeof(coloredBuffer), buffer))
	{
		// CFormat при нехватке места НЕ завершает строку нулём — слать такой буфер нельзя.
		Warning("utils::SayChat did not have enough space to print: %s\n", buffer);
		return;
	}

	i32 mode = kz_chat_mode.Get();
	if (mode < 0 || mode > 2)
	{
		static bool warnedMode = false;
		if (!warnedMode)
		{
			warnedMode = true;
			Warning("[cyb] kz_chat_mode=%d out of range, using 2\n", mode);
		}
		mode = 2;
	}
	if (mode == 2)
	{
		CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
		ClientPrintFilter(filter, HUD_PRINTTALK, coloredBuffer, "", "", "", "");
		delete filter;
		return;
	}

	static ResolveLogState sayText2Log;
	INetworkMessageInternal *netmsg = ResolveUserMessage(UM_SayText2, "SayText2", sayText2Log);
	if (!netmsg)
	{
		return;
	}
	auto msg = netmsg->AllocateMessage()->ToPB<CUserMessageSayText2>();
	msg->set_entityindex(entity->entindex());
	msg->set_chat(true);
	if (mode == 1)
	{
		// Как движок: токен локализации + параметры. Строка из kz_misc.cpp имеет вид
		// «<префикс> <цвет><ник>{default}: <текст>», CFormat превращает {default} в байт 0x01 —
		// режем по ПЕРВОМУ вхождению "\x01: " (наш разделитель, а не текст игрока: «: » внутри
		// сообщения границу не сдвигает и в слот имени не попадает).
		const char *sep = V_strstr(coloredBuffer, "\x01: ");
		msg->set_messagename("Cstrike_Chat_All");
		if (sep)
		{
			std::string p1(coloredBuffer, (sep + 1) - coloredBuffer); // ник с байтом {default}
			msg->set_param1(p1);
			msg->set_param2(sep + 3);
		}
		else
		{
			msg->set_param1("");
			msg->set_param2(coloredBuffer);
		}
		// Маркер «своё»: quiet-хук глушит SayText2 с непустыми params (движковый чат) — наше
		// сообщение он должен пропустить. %s3 в Cstrike_Chat_All клиент не рисует.
		msg->set_param3(KZ_CHAT_OWN_MARKER);
	}
	else
	{
		msg->set_messagename(coloredBuffer);
	}

	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	interfaces::pGameEventSystem->PostEventAbstract(0, false, filter, netmsg, msg, 0);
	delete msg;
	delete filter;
}

void utils::PrintConsole(CBaseEntity *entity, const char *format, ...)
{
	FORMAT_STRING(buffer);
	CSingleRecipientFilter *filter = new CSingleRecipientFilter(utils::GetEntityPlayerSlot(entity).Get());
	ClientPrintFilter(filter, HUD_PRINTCONSOLE, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintChat(CBaseEntity *entity, const char *format, ...)
{
	FORMAT_STRING(buffer);
	CSingleRecipientFilter *filter = new CSingleRecipientFilter(utils::GetEntityPlayerSlot(entity).Get());
	ClientPrintFilter(filter, HUD_PRINTTALK, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintCentre(CBaseEntity *entity, const char *format, ...)
{
	FORMAT_STRING(buffer);
	CSingleRecipientFilter *filter = new CSingleRecipientFilter(utils::GetEntityPlayerSlot(entity).Get());
	ClientPrintFilter(filter, HUD_PRINTCENTER, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintAlert(CBaseEntity *entity, const char *format, ...)
{
	FORMAT_STRING(buffer);
	CSingleRecipientFilter *filter = new CSingleRecipientFilter(utils::GetEntityPlayerSlot(entity).Get());
	ClientPrintFilter(filter, HUD_PRINTALERT, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintHTMLCentre(CBaseEntity *entity, const char *format, ...)
{
	CBasePlayerController *controller = utils::GetController(entity);
	if (!controller)
	{
		return;
	}

	CUtlString buffer;
	va_list args;
	va_start(args, format);
	buffer.FormatV(format, args);

	IGameEvent *event = interfaces::pGameEventManager->CreateEvent("show_survival_respawn_status");
	if (!event)
	{
		return;
	}
	event->SetString("loc_token", buffer.Get());
	event->SetInt("duration", 1);
	event->SetInt("userid", -1);

	CPlayerSlot slot = controller->entindex() - 1;
	IGameEventListener2 *listener = g_pKZUtils->GetLegacyGameEventListener(slot);
	listener->FireGameEvent(event);
	interfaces::pGameEventManager->FreeEvent(event);
}

void utils::PrintConsoleAll(const char *format, ...)
{
	FORMAT_STRING(buffer);
	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	ClientPrintFilter(filter, HUD_PRINTCONSOLE, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintChatAll(const char *format, ...)
{
	FORMAT_STRING(buffer);
	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	ClientPrintFilter(filter, HUD_PRINTTALK, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintCentreAll(const char *format, ...)
{
	FORMAT_STRING(buffer);
	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	ClientPrintFilter(filter, HUD_PRINTCENTER, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintAlertAll(const char *format, ...)
{
	FORMAT_STRING(buffer);
	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	ClientPrintFilter(filter, HUD_PRINTALERT, buffer, "", "", "", "");
	delete filter;
}

void utils::PrintHTMLCentreAll(const char *format, ...)
{
	CUtlString buffer;
	va_list args;
	va_start(args, format);
	buffer.FormatV(format, args);

	IGameEvent *event = interfaces::pGameEventManager->CreateEvent("show_survival_respawn_status");
	if (!event)
	{
		return;
	}
	event->SetString("loc_token", buffer.Get());
	event->SetInt("duration", 1);
	event->SetInt("userid", -1);

	interfaces::pGameEventManager->FireEvent(event);
}

void utils::CPrintChat(CBaseEntity *entity, const char *format, ...)
{
	FORMAT_STRING(buffer);
	CSingleRecipientFilter *filter = new CSingleRecipientFilter(utils::GetEntityPlayerSlot(entity).Get());
	char coloredBuffer[512];
	if (CFormat(coloredBuffer, sizeof(coloredBuffer), buffer))
	{
		ClientPrintFilter(filter, HUD_PRINTTALK, coloredBuffer, "", "", "", "");
	}
	else
	{
		Warning("utils::CPrintChat did not have enough space to print: %s\n", buffer);
	}
	delete filter;
}

void utils::CPrintChatAll(const char *format, ...)
{
	FORMAT_STRING(buffer);
	CBroadcastRecipientFilter *filter = new CBroadcastRecipientFilter;
	char coloredBuffer[512];
	if (CFormat(coloredBuffer, sizeof(coloredBuffer), buffer))
	{
		ClientPrintFilter(filter, HUD_PRINTTALK, coloredBuffer, "", "", "", "");
	}
	else
	{
		Warning("utils::CPrintChatAll did not have enough space to print: %s\n", buffer);
	}
	delete filter;
}
