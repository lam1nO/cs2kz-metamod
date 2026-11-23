"""Exercise the production personal Jumpstats query with legacy and styled rows."""
from pathlib import Path
import sqlite3
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <cstdio>
#include "kz/db/queries/jumptop.h"
int main() {
    std::printf(sql_jumptop_getpersonal, "JumptopReplays", "Jumptop", 3, 0, 0, 0, 7ULL);
    std::printf("\x1e");
    std::printf(sql_jumptop_getpersonal, "JumptopStyledReplays", "JumptopStyled", 3, 0, 0, 0, 7ULL);
    std::printf("\x1e");
    std::printf(sql_jumptop_getpersonal, "JumptopStyledReplays", "JumptopStyled", 3, 0, 1, 2, 7ULL);
    std::printf("\x1e");
    std::printf(sql_jumptop_getown, "JumptopStyledReplays", "JumptopStyled", 3, 0, 0, 0, 7ULL);
    std::printf("\x1e");
    std::printf(sql_jumptop_gettop, "JumptopReplays", "Jumptop", 3, 0, 0, 0, 20);
    std::printf("\x1e%s\x1e%s\x1e%s\x1e%s\x1e%s",
        sqlite_jumptop_personal_replay_owners, sqlite_jumptop_replay_insert,
        sql_jumptop_replay_mark_personal, sql_jumptop_replay_prune, sql_jumptop_replay_prune_owners);
}
'''
with tempfile.TemporaryDirectory(prefix="kz-jump-pb-") as tmp:
    query_bin = Path(tmp) / "query"
    subprocess.run(
        ["c++", "-std=c++17", "-I", str(root / "src"), "-x", "c++", "-", "-o", query_bin],
        input=source, text=True, check=True,
    )
    normal_sql, styled_sql, blocks_sql, own_sql, top_sql, owners_sql, insert_sql, mark_sql, prune_sql, prune_owners_sql = (
        subprocess.check_output([query_bin], text=True).split("\x1e")
    )

db = sqlite3.connect(":memory:")
db.execute("CREATE TABLE Players (SteamID64 INTEGER PRIMARY KEY, Alias TEXT)")
db.execute("INSERT INTO Players VALUES (7, 'nova'), (8, 'stonewave')")
db.execute("CREATE TABLE Bans (SteamID64 INTEGER, ExpiresAt TEXT)")
for table in ("Jumptop", "JumptopStyled"):
    db.execute(f"""CREATE TABLE {table} (
        ID INTEGER PRIMARY KEY, SteamID64 INTEGER, Mode INTEGER, JumpType INTEGER, IsBlockJump INTEGER,
        Block INTEGER, Distance REAL, Strafes INTEGER, Sync REAL, Pre REAL, Max REAL, Airtime REAL,
        ReplayUUID TEXT, MapName TEXT, Direction INTEGER, Removed INTEGER)""")
    db.execute(f"CREATE TABLE {table}Replays (ReplayUUID TEXT PRIMARY KEY, Data TEXT)")

def row(table, id, distance, direction=None, block=0, replay="same", removed=0, mode=3, steam=7):
    db.execute(
        f"INSERT INTO {table} VALUES (?,?,?,0,?,?,?,7,.91,250,350,.78,?,'kz_victoria',?,?)",
        (id, steam, mode, int(bool(block)), block, distance, replay, direction, removed),
    )

row("Jumptop", 1, 261.148)
row("Jumptop", 2, 261.148)  # Ties retain the earlier public record.
row("Jumptop", 3, 272.0, steam=8)
row("Jumptop", 4, 273.0, removed=1)
row("Jumptop", 5, 274.0, direction=1)
row("Jumptop", 6, 275.0, mode=4)
row("JumptopStyled", 1, 245.62)  # Same storage ID and UUID, different namespace.
row("JumptopStyled", 2, 260.00, direction=2, block=250, replay="empty")
row("JumptopStyled", 3, 259.00, direction=2, block=255, replay="styled")
db.execute("INSERT INTO JumptopReplays VALUES ('same', 'data')")
db.execute("INSERT INTO JumptopStyledReplays VALUES ('empty', ''), ('styled', 'data')")

normal = db.execute(normal_sql).fetchall()
assert len(normal) == 1 and normal[0][0:5] == (1, 7, "nova", 0, 261.148)
assert normal[0][10:13] == ("same", "kz_victoria", 1)
styled = db.execute(styled_sql).fetchall()
assert len(styled) == 1 and styled[0][0:5] == (1, 7, "nova", 0, 245.62)
assert styled[0][10:13] == ("same", "kz_victoria", 0)
blocks = db.execute(blocks_sql).fetchall()
assert len(blocks) == 1 and blocks[0][0:5] == (3, 7, "nova", 255, 259.00)
assert blocks[0][10:13] == ("styled", "kz_victoria", 1)
db.execute("DELETE FROM JumptopStyledReplays WHERE ReplayUUID = 'styled'")
assert db.execute(blocks_sql).fetchone()[12] == 0
assert db.execute(own_sql).fetchone()[10:13] == ("same", "kz_victoria", 0)
assert [(row[0], row[12]) for row in db.execute(top_sql)] == [(3, 1), (1, 1)]

db.execute(owners_sql)

def store_replay(jumps, uuid, *, style=0, top=False, steam_id=7):
    replays = jumps + "Replays"
    db.execute(insert_sql % (replays, uuid, "data", jumps, uuid, steam_id))
    if not top:
        db.execute(mark_sql.replace("%llu", "%d") % ("OR IGNORE", steam_id, style, uuid, replays, uuid))
    db.execute(prune_sql.replace("%llu", "%d") % (replays, steam_id, style, jumps, steam_id, jumps, steam_id))
    db.execute(prune_owners_sql.replace("%llu", "%d") % (steam_id, style, replays))

row("Jumptop", 10, 263.3, replay="first-personal")
row("Jumptop", 11, 263.3, block=252, replay="first-personal")
store_replay("Jumptop", "first-personal")
assert db.execute(normal_sql).fetchone()[10:13] == ("first-personal", "kz_victoria", 1)

row("Jumptop", 12, 264.2, replay="better-distance")
store_replay("Jumptop", "better-distance")
assert db.execute("SELECT ReplayUUID FROM JumptopReplays WHERE ReplayUUID='first-personal'").fetchone()
row("Jumptop", 13, 261.0, block=254, replay="better-block")
store_replay("Jumptop", "better-block")
store_replay("Jumptop", "first-personal")  # An out-of-order payload must not resurrect a superseded PB.
assert set(db.execute("SELECT ReplayUUID FROM JumptopReplays")) == {
    ("same",), ("better-distance",), ("better-block",),
}
assert set(db.execute("SELECT ReplayUUID FROM JumptopPersonalReplayOwners")) == {
    ("better-distance",), ("better-block",),
}

row("JumptopStyled", 10, 266.0, replay="styled-current")
store_replay("JumptopStyled", "styled-current", style=1)
assert db.execute(styled_sql).fetchone()[10:13] == ("styled-current", "kz_victoria", 1)
row("JumptopStyled", 11, 269.0, replay="styled-new-top")
store_replay("JumptopStyled", "styled-new-top", style=1, top=True)
row("JumptopStyled", 12, 270.0, replay="styled-new-pb")
store_replay("JumptopStyled", "styled-new-pb", style=1)
assert set(db.execute("SELECT ReplayUUID FROM JumptopStyledReplays")) == {
    ("empty",), ("styled-new-top",), ("styled-new-pb",),
}
row("Jumptop", 14, 270.0, replay="removed", removed=1)
store_replay("Jumptop", "removed")
row("Jumptop", 15, 275.0, replay="sideways", direction=1)
store_replay("Jumptop", "sideways")
db.execute("UPDATE Jumptop SET Removed=0 WHERE ID=14")
assert db.execute(normal_sql).fetchone()[10:13] == ("removed", "kz_victoria", 1)
db.execute("DELETE FROM Jumptop WHERE ID=14")
db.execute("DELETE FROM JumptopReplays WHERE ReplayUUID='removed'")
store_replay("Jumptop", "removed", top=True)
assert not db.execute("SELECT ReplayUUID FROM JumptopReplays WHERE ReplayUUID='removed'").fetchone()
print("PASS filters, exact UUIDs, Normal/NOPRE PB retention, top history, supersession and soft/hard removal")
