#!/usr/bin/env bash
# Локальная сборка форка cs2kz-metamod в Docker (steamrt sniper SDK), без CI.
#
# Идемпотентно: сабмодули инициализируются один раз, тяжёлые слои образа берутся из кэша
# docker, а исходники заново копируются в образ на каждом запуске и пересобираются ambuild.
#
# Что чинит поверх штатного Dockerfile (сам Dockerfile в репозитории не трогаем,
# патчим только временную копию):
#   1. apt 404 на debian-security (пакет ушёл из пула bullseye-security) —
#      см. такой же патч в .github/workflows/cs2kz-build.yml для vendor/mm-cs2menus.
#   2. BuildKit ломает git smart-HTTP поверх HTTP/2 при клонировании ambuild
#      ("could not read Username for 'https://github.com'"), хотя curl тем же
#      URL отрабатывает нормально — форсируем git на HTTP/1.1.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_TAG="kz-hud-build"
WORKDIR="${KZ_BUILD_LOGDIR:-$HOME/kzwork}"
PATCHED_DOCKERFILE="$WORKDIR/Dockerfile.local-build"
RUN_LOG="$WORKDIR/kz-hud-build-run.log"

mkdir -p "$WORKDIR"
cd "$REPO_ROOT"

echo "==> Репозиторий: $REPO_ROOT"

# 1. Сабмодули (hl2sdk-cs2, metamod-source, vendor/*) — только если ещё не инициализированы.
if git submodule status | grep -q '^-'; then
  echo "==> Инициализирую сабмодули (может занять несколько минут, hl2sdk-cs2 большой)..."
  git submodule update --init --recursive
else
  echo "==> Сабмодули уже инициализированы, пропускаю."
fi

# 2. Патченная копия Dockerfile — вне репозитория, каждый запуск перегенерируем
#    (дёшево, гарантирует актуальность патча).
cat > "$PATCHED_DOCKERFILE" <<'DOCKERFILE'
FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk

WORKDIR /app
VOLUME /app/build

# Acquire::Check-Valid-Until=false: база steamrt sniper стоит на Debian bullseye, тот ушёл
# в архив, и apt валит сборку на просроченном InRelease (E: Release file ... is expired).
# Пакеты берём те же; проверку срока снимаем только для этого шага сборки образа.
#
# debian-security пул периодически теряет старые пакеты (404 на python3-pip и т.п.) —
# тот же симптом, что CI патчит для сабмодуля vendor/mm-cs2menus. Те же пакеты есть
# в основном bullseye, поэтому убираем debian-security строки из apt sources перед update.
RUN sed -i "/debian-security/d" /etc/apt/sources.list; \
    find /etc/apt/sources.list.d -name "*.list" -exec sed -i "/debian-security/d" {} + 2>/dev/null; \
    apt -o Acquire::Check-Valid-Until=false update && apt install -y git python3-pip

# BuildKit's build-time network proxy ломает git smart-HTTP поверх HTTP/2 (curl тем же
# запросом отрабатывает нормально): git clone падает с "could not read Username" сразу
# после "Cloning into". Workaround — только HTTP/1.1 для git в этом build-контейнере.
RUN git config --global http.version HTTP/1.1
RUN git clone https://github.com/alliedmodders/ambuild
RUN pip install ./ambuild
RUN git config --global --add safe.directory /app

COPY . .
CMD [ "/bin/bash", "./docker-entrypoint.sh" ]
DOCKERFILE

# 3. Образ — КАЖДЫЙ запуск: исходники попадают в него через COPY, и закэшированный по тегу
#    образ собирал бы снимок исходников на момент своего создания ("no changes"). Слои apt и
#    ambuild берутся из кэша docker, пересобирается только COPY.
echo "==> Собираю образ $IMAGE_TAG (слои до COPY — из кэша)..."
docker build -q -f "$PATCHED_DOCKERFILE" -t "$IMAGE_TAG" . >/dev/null

# 4. Сама сборка плагина через штатный docker-entrypoint.sh (configure.py --enable-optimize + ambuild).
mkdir -p build
echo "==> Собираю плагин, лог: $RUN_LOG"
docker run --rm -v "$REPO_ROOT/build:/app/build" "$IMAGE_TAG" 2>&1 | tee "$RUN_LOG"

SO_PATH="$REPO_ROOT/build/package/addons/cs2kz/bin/linuxsteamrt64/cs2kz.so"
if [ -f "$SO_PATH" ]; then
  echo "==> Готово: $SO_PATH"
else
  echo "==> cs2kz.so не найден, смотри лог: $RUN_LOG" >&2
  exit 1
fi
