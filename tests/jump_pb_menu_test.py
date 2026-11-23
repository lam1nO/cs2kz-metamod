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
}
'''
with tempfile.TemporaryDirectory(prefix="kz-jump-pb-") as tmp:
    query_bin = Path(tmp) / "query"
    subprocess.run(
        ["c++", "-std=c++17", "-I", str(root / "src"), "-x", "c++", "-", "-o", query_bin],
        input=source, text=True, check=True,
    )
    normal_sql, styled_sql, blocks_sql = subprocess.check_output([query_bin], text=True).split("\x1e")

db = sqlite3.connect(":memory:")
db.execute("CREATE TABLE Players (SteamID64 INTEGER PRIMARY KEY, Alias TEXT)")
db.execute("INSERT INTO Players VALUES (7, 'nova'), (8, 'stonewave')")
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
print("PASS jump PB filters, tie, block priority and exact replay storage")
