"""SDK-free integration of production evidence policy, detector gate and SQLite SQL."""
import json
import pathlib
import sqlite3
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
fixture = json.loads((root / 'tests/fixtures/ahk_input_v2.json').read_text())
positive = next(case['features'] for case in fixture['cases'] if case['flagged'])
with tempfile.TemporaryDirectory(prefix='kz-evidence-') as tmp:
    binary = str(pathlib.Path(tmp) / 'evidence-test')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    str(root / 'tests/movement_evidence_test.cpp'), '-o', binary], check=True)
    sql = subprocess.check_output([binary], input=' '.join(map(str, positive)).encode()).decode().split('\x1e')

db = sqlite3.connect(':memory:')
db.execute(sql[0])
db.execute(sql[1])
db.execute('CREATE TABLE JumptopReplays (ReplayUUID TEXT PRIMARY KEY, Data TEXT)')
db.execute("INSERT INTO JumptopReplays VALUES ('pb-untouched', 'original')")
insert = 'INSERT INTO KzMovementEvidenceReplays VALUES (?, ?, ?, ?, ?, ?)'
uuid = '019abcde-1234-7000-8000-000000000001'
# Non-PB evidence needs no leaderboard row; durable commit then available by UUID.
db.execute(insert, (uuid, 'cmVwbGF5', 76561198000000001, 'kz_test', 900, 1001))
db.commit()
assert db.execute(sql[2]).fetchone() == ('cmVwbGF5', 1001)
db.execute('UPDATE KzMovementEvidenceReplays SET ExpiresAt = 1000 WHERE ReplayUUID = ?', (uuid,))
assert db.execute(sql[2]).fetchone() is None  # Exact expiry boundary, even if a downloaded file exists.
# A failed transaction cannot expose a new replay UUID.
db.commit()
db.execute(insert, ('rolled-back', 'data', 76561198000000001, 'kz_test', 901, 2000))
db.rollback()
assert db.execute("SELECT 1 FROM KzMovementEvidenceReplays WHERE ReplayUUID = 'rolled-back'").fetchone() is None
for i in range(130):
    db.execute(insert, (str(i), 'data', 76561198000000001, 'kz_test', i, 999))
before = db.total_changes
db.execute(sql[3])
assert db.total_changes - before == 64
assert db.execute('SELECT COUNT(*) FROM KzMovementEvidenceReplays').fetchone()[0] == 67
assert db.execute('SELECT Data FROM JumptopReplays').fetchone() == ('original',)
plan = db.execute('EXPLAIN QUERY PLAN SELECT ReplayUUID FROM KzMovementEvidenceReplays WHERE ExpiresAt <= 1000 ORDER BY ExpiresAt LIMIT 64').fetchall()
assert any('IDX_KzMovementEvidence_Expiry' in row[3] for row in plan)
print('PASS evidence native policy, current-positive guard, non-PB SQL, expiry boundary/index, rollback, bounded purge and PB isolation')
