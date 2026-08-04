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
- **HUD**: единый `!hud` (Стандартный/MHUD/Выкл), per-element тумблеры с prefKey,
  particle-mhud (скорость/клавиши/время; particle-элемента CP/TP НЕ существует —
  CP/TP только в HTML-пути), спектатор видит по данным наблюдаемого, досылка
  контент-ассета в OnPlayerActive. HTML-панель сопровождает **нижняя панель** в plain
  centre-канале (HUD_PRINTCENTER, не HTML): CP/TP (гейт hudCpTp) + время при
  минимал-стиле; шлётся только вместе с HTML-панелью (needHtml), по изменению слепка
  состояния + heartbeat 1с (канал надёжный, каждый тик не слать, кроме минимал-стиля
  с идущим таймером — там слепок меняется каждый тик, поэтому и шлётся каждый тик).
  Приписка у скорости: `C` (crouch-jump, `KZ_HUD_C_CJ`) либо `JB` (строго
  классифицированный jumpbug, `KZ_HUD_C_JUMPBUG`, приоритетнее C) — взаимоисключающе,
  в том же окне, что и престрейф (в воздухе + grace 0.07с). Преф `hudTimerStyle`
  (0=Обновлённый деф., 1=Минималистичный) — пункт «Таймер: …» в `!options` → HUD →
  «Обычный HUD».
- **Меню**: submodule mm-cs2menus указывает на форк lam1nO (минималистичный
  стиль без ▶).
- **Чат**: `!`-команды и серверный шум (beta join, website tip, turnbinds,
  broadcast jumpstat) скрыты; `!maps`/`!mcustom` работают через релей.
- **Ноги**: hideLegs по умолчанию true (`!hidelegs` — тумблер).
- **Невидимки — режим слежки за читерами** (`src/kz/invisible/kz_invisible.*`):
  игрок из списка невидим обычным игрокам — pawn+оружие (инверсия !hide в
  `KZ::quiet::OnCheckTransmit`, спектируемая цель не гасится — краш-страховка,
  зрителя снимает ретаргет `OnGameFrame`), звуки (`FilterReceivers` в OnPostEvent),
  `!specs`/`!spec`/`!goto`, join/leave-анонсы (`bDontBroadcast` в `Hook_FireEvent`;
  `[cyb] player_join/leave`-логи целы). Видят его только он сам и другие невидимки;
  скорборд/клиентский `status` НЕ скрывают (controller не трогаем). Контракт: файл
  `csgo/cfg/cyb_invisible.json` `{"steamids":["7656...", ...]}` — кладёт **node-agent**
  до первого коннекта (отсутствует = пустой список; битый = список не меняем, warn);
  живое управление — ConCommand'ы `kz_invisible_reload` / `kz_invisible_add <sid64>` /
  `kz_invisible_remove <sid64>` (только сервер/RCON, рантайм-only). Флаг по xuid с
  эпохи OnClientConnect; возврат видимости = один сетевой `ForceFullUpdate` без
  SetAngles/Teleport.
- **SavedRuns** (`src/kz/savedrun/kz_savedrun.*`): персистентный незавершённый
  таймер — таблица `SavedRuns` в общей MySQL флота, ключ хранения
  (steam, map, course, mode, styles), поиск на заходе — БЕЗ course (курс
  определяется из найденной строки). Сейв на дисконнекте (только
  аутентифицированные, не боты, только пока таймер бежит); рестор —
  «телепорт → `ForcePause`» на первом живом спауне ПОСЛЕ Steam-auth (fetch
  ретраится, если mode/styles/карта успели измениться за время round-trip).
  Пустые чекпоинты (pro-ран без единого !cp) не восстанавливаются вовсе —
  честный skip вместо декоративного ТП-на-старт. Инвалидация сейва — на
  finish/`!r`/`!stop`/noclip; TTL 30 дней (`PurgeExpired`, раз на загрузку
  карты). Конвенция course — cyber-номер (`KZ::course::GetCyberCourseNumber`/
  `GetCourseByCyberNumber`), не hammerId/guid — переживает ребилд карты, пока
  номер курса не меняется.
- **prac — режим отработки элементов** (`src/kz/prac/*`, cyb.94; вход без ноуклипа и cp/tp→prac — cyb.95; репетиция с prac-часами — cyb.96): `!prac` замораживает ран
  (снапшот таймера+чекпоинтов в `KZPracService`, затем честный `TimerStop`). Ноуклип НЕ
  включается автоматически (решение 25.07) — игрок сам жмёт `!nc`; внутри prac ноуклип
  безопасен, «карательная» ветка `HandleNoclip` подавлена по `inPrac`;
  `!praccp`/`!practp`/`!pracprev`/`!pracnext`/`!pracreset` — свой стек точек, хранящих **вектор
  скорости** (+присед, stamina, лестница, показание prac-часов), `practp` сам снимает ноуклип.
  Второй `!prac` восстанавливает ран через тот же путь, что SavedRuns, телепортирует в точку
  заморозки **со скоростью входа** и ставит `ForcePause` только если игрок входил с земли (вошёл
  в воздухе — без паузы: она обнуляет скорость). Ключевое: **во время prac активного рана не
  существует** — настоящий таймер стоит,
  поэтому ноуклип/чекпоинты/триггеры ведут себя как в простое; ядро знает о prac только через
  вето `OnTimerStart`. Штраф вшивается в снапшот на ВХОДЕ (`tpCount + 1` → ран становится NUB),
  поэтому возврат через `!prac` и через реконнект дают одинаковый ран. Гарды входа (ревизия 2):
  живая пешка + `!pro`-предохранитель + отказ участнику активной гонки + antipause-зона
  (`CanPause` убран — входить можно в воздухе). Стек точек стирается на
  выходе и на уходе в спек; сам prac переживает спектатор, но не смерть/`!r`/рестарт
  раунда/смену карты. cvar'ы `kz_prac_enable`, `kz_prac_run_policy` (0 = ран NUB,
  1 = ран не начинать вовсе). Спека — в монорепо
  `docs/superpowers/specs/2026-07-25-kz-prac-mode-design.md`.
  **prac как репетиция (ревизия 2):** вход ставит prac-точку №1 в текущем состоянии и
  запускает **prac-часы** — отдельный счётчик `KZPracService::pracTime` (тикает в
  `KZPracService::OnPhysicsSimulatePost` рядом с таймерным хуком, тем же
  `ENGINE_FIXED_TICK_INTERVAL`), а НЕ настоящий таймер: на `timerRunning` висит весь сабмит.
  Правила: с раном часы стартуют со времени рана, без рана стоят до старт-зоны; касание старт-зоны
  (вето `OnTimerStart`) = сброс в 0 и пуск; `practp` откручивает часы к показанию точки;
  **включение ноуклипа обнуляет и останавливает** их (ловится на переходе в `MOVETYPE_NOCLIP` в
  `HandleNoclip`, а не в команде `!nc`); выход сбрасывает. Финишная зона при идущих часах
  (`KZPracService::OnEndZoneTouch` в `KZTRIGGER_ZONE_END`) печатает время **только игроку** и
  `TimerEnd` не зовёт вовсе — поэтому ни `Times`, ни реплей, ни PB/WR, ни `kz.run_finished`.
  В худе в prac показываются prac-часы (зелёные), при остановленных — `--:--.--` (DIM).
  Неочевидные связки, которые ломались при разработке: вход в prac обязан гейтить
  `KZRecordingService::OnTimerStop` (иначе `OnTimerStopped` стирает активный `RunRecorder` и
  финиш получает нулевой UUID → коллизия PK в `Times`); `TimerStopAll` prac не достаёт
  (`TimerStop` выходит по `!timerRunning` до листенеров) — поэтому свой `DropFrozenRunAll` в
  `OnRoundStart`; `TryRestoreOnSpawn` гейтится в АСИНХРОННОМ колбэке, а не на входе (у неё два
  вызывающих, второй — `db/setup_client.cpp`).
- **Центральные реплеи PB/WR** (`src/kz/replays/cyb_replay_{common,upload,download}.*`,
  cyb.26): авто-upload при новом локальном PB и серверном рекорде (WR = overall/nub;
  pro отдельно НЕ выгружается) через api `POST /replays/v1/upload` (Bearer
  cybEmitToken); `!replay pb [ник|steamid64]` / `!replay wr` — резолв
  `GET /replays/v1/resolve` + докачка с S3 в `kzreplays/downloads/`. Гейт по режиму —
  ключом (map, course, mode). ВНИМАНИЕ: upstream-ключевые слова `pb`/`wr` команды
  kz_replay ПОДМЕНЕНЫ на центральный путь (PR #556 апстрима роутил их в global API) —
  при мёрже апстрима конфликт разрешать в пользу нашей ветки; остальные 8 вариантов
  (pbpro/sr/gpb/spb…) не тронуты. Content-Type в utils/http.cpp теперь уважает
  SetHeader (имя заголовка — строго "Content-Type").
- **Звания по платформенным NUB-очкам + мост GG1** (`src/kz/profile/*`):
  upstream-`RequestRating` (cs2kz.org `/players/{id}`, гейт `IsAvailable`) ЗАМЕНЁН
  целиком на `GET {cybEmitUrl}/v1/kz/ranking/player/{sid64}?mode&category=nub`;
  `currentRating:f64` → `currentPoints:i32` (-1 = не загружено), `desiredMode` —
  строка api-режима. Лестница из 23 званий (New→Legend) вместо 10 upstream'овских;
  пороги per-mode (KZT=CKZ, VNL своя) — **дубль
  `packages/contracts/src/kz/ranks.ts` монорепо** (истина — бриф rank-constants.md;
  менять только синхронно с contracts, раскатка версией профиля). Refresh: 120с±30 +
  ретрай 5с на ранних выходах + финиш рана (`OnRunFinished` из `TimerEnd`) + смена
  режима. SCMD `kz_rank`/`!rank` (+ в whitelist !help, категория Records), переводы
  `translations/cs2kz-profile.phrases.txt`. Мост: `EmitGG1Bridge` шлёт серверную
  команду `cyb_gg1_mode <sid64> <kzt|ckz|vnl>` на входе в игру (OnPlayerActive),
  при поздней аутентификации (OnAuthorized уже в игре; ранний auth во время
  загрузки карты скипается гейтом IsInGame) и на смене режима; гейт живой сессии =
  IsInGame + контроллер PlayerConnected (один IsInGame дисконнект НЕ ловит —
  клиент остаётся SIGNONSTATE_FULL); в SwitchToMode запрос очков/эмит отсечены
  от Reset-пути (дисконнект/переиспользование слота/late load) гейтом на месте
  вызова — IsConnected + контроллер PlayerConnected (IsInGame — уже внутри
  самого эмита, `EmitGG1Bridge`); скип, пока приёмник GG1 не зарегистрировал
  команду; рубильник —
  cvar `kz_gg1_bridge`. `KZPlayer::Reset()` теперь зовёт `profileService->Reset()`.
  При мёрже апстрима конфликт в kz_profile.\* разрешать в пользу нашей ветки.

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
- **`say`/`say_team` суперсидим ТОЛЬКО для своих команд.** Соседние плагины
  (GG1MapChooser — `!rtv`/`!maps`/`!nominate`, скины — `!ws`/`!knife`,
  `!mcustom`) читают чат из игрового события `player_chat`, а оно возникает
  лишь когда `say` реально исполнился: любой `MRES_SUPERCEDE` на чужой строке
  глушит их молча и целиком. Цена нарушения: cyb.86..100 — четыре дня мёртвого
  голосования за карту на всём флоте, при живом `!nominate <карта>` (многословные
  строки не проглатывались) и живом `!mcustom <id>`.
  Проглатывать чужую строку ради «чтобы `!опечатка` не ушла в общий чат» НЕ нужно:
  невидимость держится на другом — `KZ::quiet::OnPostEvent` зануляет получателей
  `SayText/SayText2` при `overridePlayerChat=true` (профиль флота), а
  `KZ::misc::ProcessConCommand` намеренно не ре-броадкастит строки на `!`/`/`.
  При `overridePlayerChat 0` опечатки видны — это осознанный выбор конфига,
  а не повод возвращать `MRES_SUPERCEDE`.

(пауза и subtick-гард выкачены в cyb.19; пункт про MySQL — транш 2 спеки)

## Конвенции текстов (ru), проверяются на ревью

- **Подпись пункта меню — с заглавной буквы**, включая непереводимые термины:
  `Paint`, `Overlap`, `MHUD`, `Outline`, `CP/TP`. Строчная в начале подписи — баг
  (репорт 25.07 по `overlap`). ЗНАЧЕНИЕ после двоеточия — по образцу `en`:
  `on/off/custom` → `вкл/выкл/свой` (строчные, это не подпись).
- **Термины оставляем латиницей** там, где так решил пользователь (`paint`, `overlap`),
  но регистр — как у обычного пункта. Это про конкретные ключи, а не про всё меню:
  `ShowPos` переведён («Координаты (pos/ang)») и таким остаётся.
- **Строки статистики прыжка — английские в любом языке** (решение 24.07): формат-строки
  чата/консоли, заголовки колонок, сегменты, причины инвалидации ⇒ `ru == en`. Подписи
  меню jumpstats (`Jumpstats - Menu Label *`) и сервисные сообщения переводятся как обычно.
- Тип худа: подпись пункта — «Стандартный HUD», ЗНАЧЕНИЕ типа — «Стандартный»
  (`HUD - Menu Label NormalHud` vs `HUD - Menu Type Standard`); вразнобой не писать.
- Палитра стандартного HTML-худа (`BuildVersionCHud`) фиксированная — `KZ_HUD_C_*` в
  `kz_hud.cpp`; префы `mhud*Color` только для particle-MHUD, в этот путь их не тянуть
  (иначе сохранённый игроком тёмный цвет прячет текст — баг престрейфа, cyb.91).
  Апстримный `GetSpeedText` их ещё читает, но он мёртвый (нет вызовов после версии C).
