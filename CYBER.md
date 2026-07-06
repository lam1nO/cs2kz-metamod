# Наши отличия от апстрима (KZGlobalTeam/cs2kz-metamod)

Форк для non-global серверов cyber. Версии тегаются `cyb.N`, профиль kz
в монорепо пинит их в `gameops/plugins/cs2kz-fork.json` (ref+cyb_build) →
CI `cs2kz-build.yml` монорепо собирает и льёт артефакт в S3 →
`gameops/profiles/kz/plugins.lock.yaml` пинит url+sha256.
Дизайн текущего пакета доработок — в монорепо:
`docs/superpowers/specs/2026-07-06-kz-ux-pack-design.md`.

## Локальная сборка (macOS) — две ловушки

1. AMBuildScript ищет каталог `hl2sdk-manifests` вверх от cwd; в репо его нет.
   Один раз: `cp -R metamod-source/hl2sdk-manifests ./hl2sdk-manifests && rm
./hl2sdk-manifests/.git` (untracked, в .git/info/exclude).
2. Исходники живут в docker-ОБРАЗЕ (`COPY . .`), `docker run` монтирует только
   `build/` и `.git` — перед КАЖДОЙ сборкой: `docker build -q -t
cs2kz-linux-builder .`, иначе компилируются старые исходники, а `test -f`
   «зеленеет» от старого артефакта. Успех = новый mtime
   `build/package/addons/cs2kz/bin/linuxsteamrt64/cs2kz.so`.
   Локальная сборка — только для валидации; релизный артефакт делает CI.

## Реестр отличий

- **anticheat**: cvar `kz_anticheat` отключает детекторы (SOCD и т.п.) на
  non-global; warning/cheater-принты тоже за этим гейтом.
- **Режим KZT** (`src/kz/mode/kz_mode_kzt.*`): порт KZTimer — prestrafe по
  tick-counter velMod, gokz perf-модель (кап 380), перф-окно 1/128, высота
  бхопа через legacy jump. Jumpstats: prespeed-gated lowpre-тиры для
  BH/MBH/JB/WJ; **LJ wrecker = 284.0 — решение пользователя, не менять**.
- **Эмиттер** (`src/kz/timer/cyb_emitter.*`): `kz.run_finished` в ingest
  платформы по окончании таймера. Конвенция course: 0 = main, N = bonus N.
- **HUD**: единый `!hud` (Стандартный/MHUD), per-element тумблеры с prefKey,
  particle-mhud (скорость/клавиши/CP-TP/время), спектатор видит по данным
  наблюдаемого, досылка контент-ассета в OnPlayerActive.
- **Меню**: submodule mm-cs2menus указывает на форк lam1nO (минималистичный
  стиль без ▶).
- **Чат**: `!`-команды и серверный шум (beta join, website tip, turnbinds,
  broadcast jumpstat) скрыты; `!maps`/`!mcustom` работают через релей.
- **Ноги**: hideLegs по умолчанию true (`!hidelegs` — тумблер).

## Инварианты (не ломать при мёрже апстрима)

- Ран/PB/реплей всегда привязаны к (map, course, mode) — кросс-режимный
  перенос недопустим.
- Пауза: только при запущенном таймере; рестарт снимает паузу; телепорт на
  чекпоинт в паузе запрещён.
- `sv_subtick_movement_view_angles` форсится в false как режимный cvar всех
  трёх режимов (per-tick через ApplyModeSettings; на пустом сервере значение
  может отличаться — форс начинается с первым тиком движения игрока).
- Преференсы игроков и локальные рекорды — в общей БД флота (MySQL через
  sql_mm), не per-server SQLite.

(пауза и subtick-гард выкачены в cyb.19; пункт про MySQL — транш 2 спеки)
