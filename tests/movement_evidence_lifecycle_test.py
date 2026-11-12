"""Compile actual writer implementation with a minimal engine stub and lifecycle regression."""
import pathlib
import re
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[1]
plugin = (root / 'src/cs2kz.cpp').read_text()
unload = plugin[plugin.index('bool KZPlugin::Unload('):]
assert unload.index('KZ::analytics::FlushPendingEvidence();') < unload.index('KZ::zones::DestroyEditorMenus();')
assert unload.index('KZ::analytics::FlushPendingEvidence();') < unload.index('KZRecordingService::Shutdown();')
assert unload.index('KZ::analytics::FlushPendingEvidence();') < unload.index('KZDatabaseService::Cleanup();')
analytics = re.sub(r'\s+', '', (root / 'src/kz/analytics/kz_analytics.cpp').read_text())
assert 'pendingEvidence.Poll(EvidenceNow());' in analytics
assert analytics.index('pendingEvidence.Add(delivery,EvidenceNow());') < analytics.index('recordingService->RecordMovementEvidence(jump,')
with tempfile.TemporaryDirectory(prefix='kz-evidence-lifecycle-') as tmp:
    source = pathlib.Path(tmp) / 'lifecycle.cpp'
    writer = (root / 'src/kz/recording/filewriter.cpp').read_text()
    writer = '\n'.join(line for line in writer.splitlines() if not line.startswith('#include'))
    source.write_text((root / 'tests/fixtures/recording_writer_stub.h').read_text() + '\n' + writer + '\n'
                      + (root / 'tests/movement_evidence_lifecycle_test.cpp').read_text())
    binary = str(pathlib.Path(tmp) / 'lifecycle')
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + str(root), str(source), '-o', binary], check=True)
    subprocess.run([binary], check=True)
