"""Run the production mixed-style top and personal place queries on SQLite."""
from pathlib import Path
import sqlite3
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <cstdio>
#include "kz/db/queries/course_top.h"
int main() {
    // ABH is bit 1, AUD bit 2, LGJ bit 4, NOPRE bit 8. CKZ's NOPRE record mode is 12.
    std::printf(sql_get_course_styles_top, "kz_victoria", "Main", 11ULL, 1, 8ULL, 12, 8ULL, 0, 10, 0);
    std::printf("\x1e");
    std::printf(sql_get_course_styles_top, "kz_victoria", "Main", 11ULL, 1, 8ULL, 12, 8ULL, 1, 10, 0);
    std::printf("\x1e");
    std::printf(sql_get_course_styles_personal, "kz_victoria", "Main", 11ULL, 1, 8ULL, 12, 8ULL, 0, 7ULL);
    std::printf("\x1e");
    std::printf(sql_get_course_styles_top, "kz_victoria", "Main", 7ULL, 2, 8ULL, 0, 8ULL, 0, 10, 0);
    std::printf("\x1e");
    std::printf(sql_getcoursetop, "kz_victoria", "Main", 1, 3ULL, 10, 0);
    std::printf("\x1e");
    std::printf(sql_getcoursetop, "kz_victoria", "Main", 12, 8ULL, 10, 0);
    std::printf("\x1e");
    std::printf(sql_getcoursetop, "kz_victoria", "Main", 1, 0ULL, 10, 0);
}
'''
with tempfile.TemporaryDirectory(prefix="kz-run-styles-") as tmp:
    query_bin = Path(tmp) / "query"
    subprocess.run(
        ["c++", "-std=c++17", "-I", str(root / "src"), "-x", "c++", "-", "-o", query_bin],
        input=source, text=True, check=True,
    )
    all_sql, pro_sql, personal_sql, vnl_sql, exact_sql, nopre_sql, normal_sql = subprocess.check_output([query_bin], text=True).split("\x1e")

db = sqlite3.connect(":memory:")
db.executescript("""
CREATE TABLE Maps (ID INTEGER PRIMARY KEY, Name TEXT);
CREATE TABLE MapCourses (ID INTEGER PRIMARY KEY, MapID INTEGER, Name TEXT);
CREATE TABLE Players (SteamID64 INTEGER PRIMARY KEY, Alias TEXT);
CREATE TABLE Bans (ID INTEGER PRIMARY KEY, SteamID64 INTEGER, ExpiresAt TEXT);
CREATE TABLE Times (ID TEXT PRIMARY KEY, SteamID64 INTEGER, MapCourseID INTEGER,
                    ModeID INTEGER, StyleIDFlags INTEGER, RunTime REAL, Teleports INTEGER, Created TEXT);
INSERT INTO Maps VALUES (1, 'kz_victoria');
INSERT INTO MapCourses VALUES (1, 1, 'Main'), (2, 1, 'Bonus 1');
INSERT INTO Players VALUES (7, 'nova'), (8, 'stonewave'), (9, 'banned');
INSERT INTO Bans VALUES (1, 9, NULL);
""")

def insert(id, player, mode, mask, time, tp=0, course=1):
    db.execute("INSERT INTO Times VALUES (?,?,?,?,?,?,?,'2026-11-23')", (id, player, course, mode, mask, time, tp))

insert("normal", 7, 1, 0, 15)
insert("abh-nub", 7, 1, 1, 30, tp=2)
insert("abh-pro", 7, 1, 1, 32)
insert("abh-pro-tie", 7, 1, 1, 32)
insert("abh-aud", 7, 1, 3, 40)
insert("nopre", 8, 12, 8, 34)
insert("invalid-style-in-alias", 8, 12, 1, 10)
insert("old-nopre", 8, 1, 8, 12)
insert("lgj-old", 8, 1, 4, 13)
insert("lgj-valid", 8, 2, 4, 20)
insert("nopre-vnl", 7, 2, 9, 11)
insert("unknown", 8, 1, 16, 14)
insert("banned-styled", 9, 1, 3, 16)
insert("bonus", 7, 1, 1, 12, course=2)

expected_all = [("abh-nub", 1), ("nopre", 8), ("abh-aud", 3)]
expected_pro = [("abh-pro", 1), ("nopre", 8), ("abh-aud", 3)]
assert [(row[0], row[7]) for row in db.execute(all_sql)] == expected_all
assert [(row[0], row[7]) for row in db.execute(pro_sql)] == expected_pro
personal = db.execute(personal_sql).fetchall()
assert len(personal) == 1 and (personal[0][0], personal[0][6], personal[0][7]) == ("abh-nub", 1, 1)
assert [(row[0], row[7]) for row in db.execute(vnl_sql)] == [("lgj-valid", 4)]
assert [(row[0], row[6]) for row in db.execute(exact_sql)] == [("abh-aud", 3)]
assert [(row[0], row[6]) for row in db.execute(nopre_sql)] == [("nopre", 8)]
assert [(row[0], row[6]) for row in db.execute(normal_sql)] == [("normal", 0)]
print("PASS mixed and exact run tops, mode compatibility, PRO, and personal rank")
