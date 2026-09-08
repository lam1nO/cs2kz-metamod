FROM registry.gitlab.steamos.cloud/steamrt/sniper/sdk

WORKDIR /app
VOLUME /app/build

# Acquire::Check-Valid-Until=false: база steamrt sniper стоит на Debian bullseye, тот ушёл
# в архив, и apt валит сборку на просроченном InRelease (E: Release file ... is expired).
# Пакеты берём те же; проверку срока снимаем только для этого шага сборки образа.
RUN apt -o Acquire::Check-Valid-Until=false update && apt install -y git python3-pip
RUN git clone https://github.com/alliedmodders/ambuild
RUN pip install ./ambuild
RUN git config --global --add safe.directory /app

COPY . .
CMD [ "/bin/bash", "./docker-entrypoint.sh" ]
