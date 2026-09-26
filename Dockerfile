FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk

WORKDIR /app
VOLUME /app/build

# Acquire::Check-Valid-Until=false: база steamrt sniper стоит на Debian bullseye, тот ушёл
# в архив, и apt валит сборку на просроченном InRelease (E: Release file ... is expired).
# Пакеты берём те же; проверку срока снимаем только для этого шага сборки образа.
# 26.09.2026 из пула bullseye-security убрали файлы, на которые ещё ссылается его индекс
# (python3-pip 20.3.4-4+deb11u2 → 404), и apt падает на install. Security-репозиторий для
# сборочного образа не нужен: те же пакеты (deb11u1) лежат в основном bullseye. Убираем его
# строки из sources перед update — только в этом образе, на игровые серверы он не едет.
RUN sed -i '/debian-security/d' /etc/apt/sources.list 2>/dev/null || true; \
    find /etc/apt/sources.list.d -name '*.list' -exec sed -i '/debian-security/d' {} + 2>/dev/null || true; \
    apt -o Acquire::Check-Valid-Until=false update; \
    apt install -y git python3-pip || { \
      command -v git >/dev/null || apt install -y git; \
      python3 -m pip --version || curl -sSf https://bootstrap.pypa.io/pip/3.9/get-pip.py | python3; }
RUN git clone https://github.com/alliedmodders/ambuild
RUN pip install ./ambuild
RUN git config --global --add safe.directory /app

COPY . .
CMD [ "/bin/bash", "./docker-entrypoint.sh" ]
