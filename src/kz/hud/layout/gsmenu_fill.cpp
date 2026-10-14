// PB и WR в плитках сетки карт GG1 (!nominate/!maps, корень gg разметки cyber/gsmenu.xml).
//
// GG1 после отрисовки сетки шлёт с консоли сервера `kz_gsm_fill <slot> <first> <карта,карта,...>`
// (first — индекс плитки gt{first} первой карты пачки, имена в нижнем регистре, пустой элемент —
// плитку не трогать) и в конце `kz_gsm_fill <slot> end <count>`. Ник рекордсмена через консоль не
// пройдёт (пробелы, юникод), поэтому плитки пишет сам форк — прямо в сущность моста (ExtLayoutOf).
//
// Плитка gt{i}: класс done — у игрока есть время главного курса в его режиме (любой стиль и TP);
// pb — лучшее время главного курса без стилей «mm:ss.cc»; gap — «+17.14» до WR или «WR» (класс
// iswr); wr/wrn — рекорд сервера главного курса без стилей и ник (до 20 символов), класс nowr —
// времён на карте нет. Главный курс — наименьший StageID карты.
//
// Кэш: WR — общий на режим, живёт до смены карты/нового рекорда, перезапрос не чаще раза в 60 с;
// PB — на слот+режим, до смены режима/своего нового времени/выхода. Пока данных нет — fill ждёт.

#include "kz/hud/layout/gsmenu.h"
#include "kz/db/kz_db.h"
#include "kz/mode/kz_mode.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "utils/utils.h"
#include "utils/logging.h"
#include "utils/interfaces.h"

#include "vendor/sql_mm/src/public/sql_mm.h"

#include <cmath>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "tier0/memdbgon.h"

// Главный курс карты в запросах: наименьший StageID её курсов.
#define KZ_FILL_MAIN_COURSE(mc) "(SELECT MIN(mcm.StageID) FROM MapCourses mcm WHERE mcm.MapID = " mc ".MapID)"

// PB игрока по всем картам: [0] имя карты, [1] лучшее время любым стилем (done), [2] без стилей (pb).
static constexpr char sql_fill_pb[] = "SELECT m.Name, MIN(t.RunTime), MIN(CASE WHEN t.StyleIDFlags = 0 THEN t.RunTime END) "
									  "FROM Times t INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID INNER JOIN Maps m ON m.ID = mc.MapID "
									  "WHERE t.SteamID64 = %llu AND t.ModeID = %d AND mc.StageID = " KZ_FILL_MAIN_COURSE("mc") " GROUP BY m.Name";

// WR сервера по всем картам без оконных функций (MySQL и SQLite): минимум главного курса без стилей
// и без забаненных (как !maptop), затем строки с этим временем — ник. Ничья — первая по t.ID.
static constexpr char sql_fill_wr[] =
	"SELECT m.Name, x.Best, p.Alias FROM ("
	"SELECT mc.MapID, MIN(t.RunTime) AS Best FROM Times t INNER JOIN MapCourses mc ON mc.ID = t.MapCourseID "
	"LEFT JOIN Bans b ON b.SteamID64 = t.SteamID64 AND (b.ExpiresAt IS NULL OR b.ExpiresAt > CURRENT_TIMESTAMP) "
	"WHERE b.ID IS NULL AND t.ModeID = %d AND t.StyleIDFlags = 0 AND mc.StageID = " KZ_FILL_MAIN_COURSE("mc") " GROUP BY mc.MapID) x "
	"INNER JOIN Maps m ON m.ID = x.MapID "
	"INNER JOIN MapCourses mc2 ON mc2.MapID = x.MapID AND mc2.StageID = " KZ_FILL_MAIN_COURSE("mc2") " "
	"INNER JOIN Times t2 ON t2.MapCourseID = mc2.ID AND t2.ModeID = %d AND t2.StyleIDFlags = 0 AND t2.RunTime = x.Best "
	"INNER JOIN Players p ON p.SteamID64 = t2.SteamID64 "
	"LEFT JOIN Bans b2 ON b2.SteamID64 = t2.SteamID64 AND (b2.ExpiresAt IS NULL OR b2.ExpiresAt > CURRENT_TIMESTAMP) "
	"WHERE b2.ID IS NULL ORDER BY m.Name, t2.ID";

#define KZ_FILL_WR_TTL   60.0 // с — перезапрос WR не чаще
#define KZ_FILL_NAME_MAX 20   // символов ника на плитке
#define KZ_FILL_TILES    400  // gt0..gt399 — ёмкость сетки разметки (GridView.MaxTiles GG1)

namespace
{
	struct WrEntry
	{
		f64 time = 0.0;
		std::string alias;
	};

	struct WrCache
	{
		std::unordered_map<std::string, WrEntry> maps;
		std::string map; // карта сервера, на которой читали: смена карты — кэш устарел
		f64 fetchedAt = 0.0;
		bool valid = false;
		bool loading = false;
		u32 gen = 0;
	};

	struct PbEntry
	{
		bool done = false;
		f64 pb = 0.0; // 0 — без стилей времени нет
	};

	// Что уже стоит в плитке — повторная запись переменной сетевое изменение даже без смены значения.
	struct TileOut
	{
		bool set = false;
		bool done = false, iswr = false, nowr = false;
		std::string pb, gap, wr, wrn;
	};

	struct SlotFill
	{
		std::vector<std::string> pending; // пачки до «end»
		std::vector<std::string> tiles;   // карта плитки gt{i}, пусто — не трогать
		bool active = false;
		std::vector<TileOut> applied;
		u64 pbSteam = 0;
		i32 pbMode = -1;
		bool pbValid = false;
		bool pbLoading = false;
		u32 pbGen = 0;
		std::unordered_map<std::string, PbEntry> pb;
	};

	std::unordered_map<i32, WrCache> g_wr;
	SlotFill g_fill[MAXPLAYERS + 1];

	f64 Now()
	{
		return g_pKZUtils->GetServerGlobals()->realtime;
	}

	std::string CurrentMap()
	{
		return g_pKZUtils->GetCurrentMapName().Get();
	}

	std::string Lower(const char *s)
	{
		std::string out = s ? s : "";
		for (char &c : out)
		{
			if (c >= 'A' && c <= 'Z')
			{
				c = (char)(c - 'A' + 'a');
			}
		}
		return out;
	}

	// Время из строки: GetFloat отдаёт float, а PB с WR сравниваются до миллисекунды.
	f64 TimeCol(ISQLResult *result, u32 col)
	{
		const char *v = result->GetString(col);
		return v ? strtod(v, nullptr) : 0.0;
	}

	// Миллисекунды с округлением (как utils::FormatTime), но в double: раны бывают по нескольку часов.
	long long Ms(f64 secs)
	{
		return (long long)floor(secs * 1000.0 + 0.5);
	}

	// Сотые усечением: «01:02.77», с часами «1:02:03.45».
	std::string FormatPb(f64 secs)
	{
		const long long cs = Ms(secs) / 10;
		const long long h = cs / 360000, m = cs / 6000 % 60, s = cs / 100 % 60, c = cs % 100;
		char buf[32];
		if (h > 0)
		{
			V_snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", h, m, s, c);
		}
		else
		{
			V_snprintf(buf, sizeof(buf), "%02lld:%02lld.%02lld", m, s, c);
		}
		return buf;
	}

	// Отставание в сотых без ведущих нулей: «+17.14», «+1:10.97», «+1:02:03.45».
	std::string FormatGap(long long cs)
	{
		const long long h = cs / 360000, m = cs / 6000 % 60, s = cs / 100 % 60, c = cs % 100;
		char buf[40];
		if (h > 0)
		{
			V_snprintf(buf, sizeof(buf), "+%lld:%02lld:%02lld.%02lld", h, m, s, c);
		}
		else if (m > 0)
		{
			V_snprintf(buf, sizeof(buf), "+%lld:%02lld.%02lld", m, s, c);
		}
		else
		{
			V_snprintf(buf, sizeof(buf), "+%lld.%02lld", s, c);
		}
		return buf;
	}

	// Ник: до KZ_FILL_NAME_MAX символов UTF-8 целиком, управляющие — пробел; ведущий # панорама
	// приняла бы за ключ локализации (как UiBatch.Clean в GG1).
	std::string CleanAlias(const char *alias)
	{
		std::string out;
		i32 chars = 0;
		for (const unsigned char *p = (const unsigned char *)(alias ? alias : ""); *p;)
		{
			i32 len = 1;
			if (*p >= 0xF0)
			{
				len = 4;
			}
			else if (*p >= 0xE0)
			{
				len = 3;
			}
			else if (*p >= 0xC0)
			{
				len = 2;
			}
			else if (*p >= 0x80)
			{
				p++; // оборванный хвост чужого символа — пропуск
				continue;
			}
			i32 have = 1;
			while (have < len && (p[have] & 0xC0) == 0x80)
			{
				have++;
			}
			if (have < len)
			{
				p += have; // битая последовательность
				continue;
			}
			if (++chars > KZ_FILL_NAME_MAX)
			{
				break;
			}
			if (len == 1 && *p < ' ')
			{
				out += ' ';
			}
			else
			{
				out.append((const char *)p, len);
			}
			p += len;
		}
		if (!out.empty() && out[0] == '#')
		{
			out.replace(0, 1, "\xEF\xBC\x83"); // ＃
		}
		return out;
	}

	void Refresh(i32 slot);

	void RefreshAll()
	{
		for (i32 i = 0; i < MAXPLAYERS; i++)
		{
			if (g_fill[i].active)
			{
				Refresh(i);
			}
		}
	}

	void RequestWr(i32 modeID)
	{
		WrCache &w = g_wr[modeID];
		if (w.loading)
		{
			return;
		}
		w.loading = true;
		const u32 gen = ++w.gen;
		char query[2048];
		V_snprintf(query, sizeof(query), sql_fill_wr, modeID, modeID);
		Transaction txn;
		txn.queries.push_back(query);
		const std::string map = CurrentMap();
		auto onSuccess = [modeID, gen, map](std::vector<ISQLQuery *> queries)
		{
			WrCache &w = g_wr[modeID];
			if (w.gen != gen)
			{
				return; // кэш сброшен (новый рекорд/выгрузка), пока шёл запрос
			}
			w.loading = false;
			w.maps.clear();
			ISQLResult *result = queries.empty() ? nullptr : queries[0]->GetResultSet();
			while (result && result->FetchRow())
			{
				std::string name = Lower(result->GetString(0));
				if (name.empty() || result->IsNull(1) || w.maps.count(name))
				{
					continue; // ничья — первая строка
				}
				w.maps[name] = {TimeCol(result, 1), CleanAlias(result->GetString(2))};
			}
			w.valid = true;
			w.map = map;
			w.fetchedAt = Now();
			RefreshAll();
		};
		auto onFailure = [modeID, gen](std::string error, int)
		{
			WrCache &w = g_wr[modeID];
			if (w.gen == gen)
			{
				w.loading = false;
			}
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsm_fill_wr_failed mode=%d err=%s\n", modeID, error.c_str());
		};
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
	}

	void RequestPb(KZPlayer *player, i32 slot, i32 modeID)
	{
		SlotFill &f = g_fill[slot];
		const u64 steam = player->GetSteamId64();
		if (f.pbLoading && f.pbMode == modeID && f.pbSteam == steam)
		{
			return;
		}
		f.pbLoading = true;
		f.pbValid = false;
		f.pbMode = modeID;
		f.pbSteam = steam;
		const u32 gen = ++f.pbGen;
		char query[1024];
		V_snprintf(query, sizeof(query), sql_fill_pb, steam, modeID);
		Transaction txn;
		txn.queries.push_back(query);
		const CPlayerUserId uid = player->GetClient()->GetUserID();
		auto onSuccess = [slot, uid, gen](std::vector<ISQLQuery *> queries)
		{
			KZPlayer *p = g_pKZPlayerManager->ToPlayer(uid);
			SlotFill &f = g_fill[slot];
			if (!p || p->GetPlayerSlot().Get() != slot || f.pbGen != gen)
			{
				return; // игрок ушёл/слот чужой или кэш сброшен, пока шёл запрос
			}
			f.pbLoading = false;
			f.pb.clear();
			ISQLResult *result = queries.empty() ? nullptr : queries[0]->GetResultSet();
			while (result && result->FetchRow())
			{
				std::string name = Lower(result->GetString(0));
				if (name.empty() || result->IsNull(1))
				{
					continue;
				}
				f.pb[name] = {true, result->IsNull(2) ? 0.0 : TimeCol(result, 2)};
			}
			f.pbValid = true;
			Refresh(slot);
		};
		auto onFailure = [slot, gen](std::string error, int)
		{
			SlotFill &f = g_fill[slot];
			if (f.pbGen == gen)
			{
				f.pbLoading = false;
			}
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsm_fill_pb_failed slot=%i err=%s\n", slot, error.c_str());
		};
		KZDatabaseService::GetDatabaseConnection()->ExecuteTransaction(txn, onSuccess, onFailure);
	}

	TileOut Compute(const SlotFill &f, const WrCache &w, const std::string &map)
	{
		TileOut t;
		t.set = true;
		auto pb = f.pb.find(map);
		auto wr = w.maps.find(map);
		const bool hasWr = wr != w.maps.end();
		t.done = pb != f.pb.end() && pb->second.done;
		t.nowr = !hasWr;
		if (hasWr)
		{
			t.wr = FormatPb(wr->second.time);
			t.wrn = wr->second.alias;
		}
		if (t.done && pb->second.pb > 0.0)
		{
			t.pb = FormatPb(pb->second.pb);
			if (hasWr)
			{
				// Своё время не лучше рекорда сервера (забаненный не входит в WR — тоже «WR»).
				if (Ms(pb->second.pb) <= Ms(wr->second.time))
				{
					t.iswr = true;
					t.gap = "WR";
				}
				else
				{
					t.gap = FormatGap(Ms(pb->second.pb) / 10 - Ms(wr->second.time) / 10);
				}
			}
		}
		return t;
	}

	void Apply(KZPlayer *player, i32 slot, const WrCache &w)
	{
		// Сущность берём ДО чтения fill: пересоздание по интернам сносит fill (DestroyExt), и GG1
		// после css_ui_reset пришлёт его заново.
		CCSCustomHudLayout *layout = KZ::gsmenu::ExtLayoutOf(player);
		SlotFill &f = g_fill[slot];
		if (!layout || !f.active)
		{
			return;
		}
		if (f.applied.size() < f.tiles.size())
		{
			f.applied.resize(f.tiles.size());
		}
		bool classDirty = false;
		i32 dropped = 0;
		char panel[16];
		for (size_t i = 0; i < f.tiles.size(); i++)
		{
			if (f.tiles[i].empty())
			{
				continue;
			}
			const TileOut want = Compute(f, w, f.tiles[i]);
			TileOut &cur = f.applied[i];
			V_snprintf(panel, sizeof(panel), "gt%i", (i32)i);
			auto cls = [&](const char *name, bool on, bool was)
			{
				if (cur.set && on == was)
				{
					return;
				}
				dropped += !layout->SetHasClass(panel, name, on ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
				classDirty = true;
			};
			auto var = [&](const char *name, const std::string &value, const std::string &was)
			{
				if (cur.set && value == was)
				{
					return;
				}
				dropped += !layout->SetDialogVariableString(panel, name, value.c_str());
			};
			cls("done", want.done, cur.done);
			cls("iswr", want.iswr, cur.iswr);
			cls("nowr", want.nowr, cur.nowr);
			var("pb", want.pb, cur.pb);
			var("gap", want.gap, cur.gap);
			var("wr", want.wr, cur.wr);
			var("wrn", want.wrn, cur.wrn);
			cur = want;
		}
		if (classDirty)
		{
			layout->GetGlobalLayoutState()->MarkFullChanged();
		}
		if (dropped > 0)
		{
			KZ_LOG_WARN(LogChannel::General, "[cyb] gsm_fill_dropped reason=intern_limit ops=%i slot=%i\n", dropped, slot);
		}
	}

	void Refresh(i32 slot)
	{
		if (slot < 0 || slot >= MAXPLAYERS || !g_fill[slot].active)
		{
			return;
		}
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(slot));
		if (!player || !player->IsInGame() || !player->GetClient() || !player->modeService || !KZDatabaseService::IsReady())
		{
			return;
		}
		SlotFill &f = g_fill[slot];
		const i32 modeID = KZ::mode::GetModeInfo(player->modeService).databaseID;
		const bool pbOk = f.pbValid && f.pbMode == modeID && f.pbSteam == player->GetSteamId64();
		if (!pbOk)
		{
			RequestPb(player, slot, modeID);
		}
		WrCache &w = g_wr[modeID];
		if (w.valid && w.map != CurrentMap())
		{
			w.valid = false; // карта сменилась
			w.maps.clear();
		}
		const f64 now = Now();
		if (!w.valid || now - w.fetchedAt >= KZ_FILL_WR_TTL || now < w.fetchedAt)
		{
			RequestWr(modeID);
		}
		// Устаревший по времени WR показываем, пока идёт перезапрос; сброшенный — ждём.
		if (pbOk && w.valid)
		{
			Apply(player, slot, w);
		}
	}
} // namespace

void KZ::gsmenu::FillReset(i32 slot, bool leaving)
{
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return;
	}
	SlotFill &f = g_fill[slot];
	f.pending.clear();
	f.tiles.clear();
	f.applied.clear();
	f.active = false;
	if (leaving)
	{
		f.pb.clear();
		f.pbValid = false;
		f.pbLoading = false;
		f.pbMode = -1;
		f.pbSteam = 0;
		f.pbGen++; // летящий ответ ушедшему — выбросить
	}
}

void KZ::gsmenu::FillRefresh(KZPlayer *player)
{
	if (player)
	{
		Refresh(player->GetPlayerSlot().Get());
	}
}

void KZ::gsmenu::FillOnTimeSaved(u64 steamID, i32 modeID, f64 time)
{
	// Своё новое время — PB игрока перечитать (слот ищем по SteamID: ран мог записаться из outbox).
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		SlotFill &f = g_fill[i];
		if (f.pbSteam == steamID && f.pbMode == modeID)
		{
			f.pbValid = false;
			f.pbLoading = false;
			f.pbGen++;
		}
	}
	// Новый рекорд сервера — WR режима перечитать. Время пишется на текущей карте; курс тут не
	// известен, поэтому сброс при любом времени лучше рекорда карты (бонус лишь лишний перезапрос).
	auto it = g_wr.find(modeID);
	if (it != g_wr.end() && it->second.valid)
	{
		auto wr = it->second.maps.find(Lower(CurrentMap().c_str()));
		if (wr == it->second.maps.end() || time < wr->second.time)
		{
			it->second.valid = false;
			it->second.loading = false;
			it->second.gen++;
		}
	}
	RefreshAll();
}

CON_COMMAND_F(kz_gsm_fill, "kz_gsm_fill <slot> <first> <карта,карта,...> | <slot> end <count> — PB/WR в плитки сетки GG1", FCVAR_NONE)
{
	if (utils::GetController(context.GetPlayerSlot()) || args.ArgC() < 3)
	{
		return;
	}
	const i32 slot = V_StringToInt32(args.Arg(1), -1);
	if (slot < 0 || slot >= MAXPLAYERS)
	{
		return;
	}
	SlotFill &f = g_fill[slot];
	if (KZ_STREQ(args.Arg(2), "end"))
	{
		const i32 count = V_StringToInt32(args.Arg(3), -1);
		if (count < 0 || count > KZ_FILL_TILES)
		{
			f.pending.clear();
			return;
		}
		f.pending.resize(count);
		f.tiles.swap(f.pending);
		f.pending.clear();
		f.applied.resize(f.tiles.size());
		f.active = true;
		Refresh(slot);
		return;
	}
	const i32 first = V_StringToInt32(args.Arg(2), -1);
	if (first < 0 || first >= KZ_FILL_TILES)
	{
		return;
	}
	if (first == 0)
	{
		f.pending.clear(); // новая серия пачек
	}
	// Список — сырой хвост после двух слов: токенайзер консоли рвёт по части знаков.
	const char *rest = args.ArgS();
	for (i32 skip = 0; skip < 2 && *rest; skip++)
	{
		while (*rest == ' ')
		{
			rest++;
		}
		while (*rest && *rest != ' ')
		{
			rest++;
		}
	}
	while (*rest == ' ' || *rest == '"')
	{
		rest++;
	}
	size_t idx = (size_t)first;
	std::string item;
	for (const char *p = rest;; p++)
	{
		if (*p == ',' || *p == '\0' || *p == '"' || *p == ' ')
		{
			if (idx < KZ_FILL_TILES)
			{
				if (f.pending.size() <= idx)
				{
					f.pending.resize(idx + 1);
				}
				f.pending[idx] = Lower(item.c_str());
			}
			idx++;
			item.clear();
			if (*p != ',')
			{
				break;
			}
			continue;
		}
		item += *p;
	}
}
