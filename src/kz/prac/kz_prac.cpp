#include "kz_prac.h"

#include "kz/language/kz_language.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/racing/kz_racing.h"
#include "kz/recording/kz_recording.h"
#include "kz/replays/kz_replaysystem.h"
#include "kz/savedrun/kz_savedrun.h"
#include "kz/spec/kz_spec.h"
#include "kz/trigger/kz_trigger.h"
#include "utils/utils.h"

CConVar<bool> kz_prac_enable("kz_prac_enable", FCVAR_NONE, "Whether the !prac practice mode is available to players.", true);
CConVar<i32> kz_prac_run_policy("kz_prac_run_policy", FCVAR_NONE,
								"What happens to a run that went through !prac: 0 = becomes NUB (counted), 1 = not counted at all.", 0);

// Канонический вид «режим + стили» для сверки на выходе из prac. Строим через тот же
// BuildStylesString, что и ключ SavedRuns, чтобы «те же стили» значило одно и то же во всём форке.
static_function void SnapshotModeStyles(KZPlayer *player, char *modeOut, int modeSize, char *stylesOut, int stylesSize)
{
	V_strncpy(modeOut, player->modeService->GetModeShortName(), modeSize);
	V_strncpy(stylesOut, KZSavedRunService::BuildStylesString(player).Get(), stylesSize);
}

// «ckz» или «ckz (abh)» — только для сообщения игроку о несовпадении, не для сверки.
static_function void FormatModeStyles(const char *mode, const char *styles, char *out, int size)
{
	if (styles && styles[0])
	{
		V_snprintf(out, size, "%s (%s)", mode, styles);
	}
	else
	{
		V_snprintf(out, size, "%s", mode);
	}
}

void KZPracService::Reset()
{
	this->inPrac = false;
	this->noclipBeforeSpec = false;
	this->lastRejectHintTime = 0.0f;
	this->frozen = {};
	this->ClearPoints();
	this->ResetPracTime();
}

void KZPracService::ClearPoints()
{
	this->points.RemoveAll();
	this->currentIndex = 0;
}

void KZPracService::ResetPracTime()
{
	this->pracTime = 0.0;
	this->pracTimeRunning = false;
	// Курс — часть попытки, а не отдельное состояние: нет часов — нет и курса.
	this->pracCourseGUID = 0;
}

void KZPracService::OnPhysicsSimulatePost()
{
	// Условие симметрично KZTimerService::OnPhysicsSimulatePost (живой игрок, часы идут, пауза
	// время не копит) и стоит тем же шагом. Пауза сюда попадает и как уход в спектатор
	// (OnPlayerJoinTeam ставит paused), поэтому отдельного правила для спека не нужно.
	if (this->inPrac && this->pracTimeRunning && this->player->IsAlive() && !this->player->timerService->GetPaused())
	{
		this->pracTime += ENGINE_FIXED_TICK_INTERVAL;
	}
}

bool KZPracService::RejectMapTeleport(const char *action)
{
	if (!this->inPrac)
	{
		return false;
	}
	// Звук — на каждое нажатие: он и есть мгновенная обратная связь «команда отбита».
	this->player->PlayErrorSound();
	// realtime, а не curtime: curtime обнуляется на смене карты, а Reset() зовётся только на
	// дисконнекте — с curtime поле пережило бы карту с большим значением, разница стала бы
	// отрицательной, и подсказка с логом замолчали бы до конца сессии. Отказ обязан остаться
	// видимым, поэтому сравнение ещё и защищено от хода часов назад.
	const f32 now = g_pKZUtils->GetServerGlobals()->realtime;
	const bool cooled = this->lastRejectHintTime == 0.0f || now < this->lastRejectHintTime
						|| now - this->lastRejectHintTime > KZ_PRAC_REJECT_HINT_COOLDOWN;
	if (cooled)
	{
		this->lastRejectHintTime = now;
		this->player->languageService->PrintChat(true, false, "Prac - No Teleport");
		// WARN: отказали пользователю. Актор + машинная причина — иначе жалоба «!r не работает»
		// неотличима от «команда не дошла». Под тем же кулдауном, что и текст: серия нажатий
		// на бинде — одно событие для разбора, а не десять строк.
		KZ_LOG_WARN(LogChannel::Timer, "[cyb] prac_teleport_rejected steam_id=%llu reason=%s\n", this->player->GetSteamId64(false), action);
	}
	return true;
}

void KZPracService::OnNoclipEnabled()
{
	if (!this->inPrac)
	{
		return;
	}
	// Пролетев участок насквозь, игрок не имеет права на время за него — попытка
	// недействительна целиком. Новую заводят !practp (время точки) и выход из стартовой зоны
	// (с нуля, см. OnStartZoneEndTouch).
	const bool hadAttempt = this->pracTimeRunning || this->pracTime > 0.0;
	this->ResetPracTime();
	if (hadAttempt)
	{
		// Иначе пропажа времени в худе выглядит багом.
		this->player->languageService->PrintChat(true, false, "Prac - Time Void Noclip");
	}
}

void KZPracService::OnStartZoneEndTouch(const KZCourseDescriptor *course)
{
	if (!this->inPrac || !this->player->GetPlayerPawn())
	{
		return;
	}
	// Ровно те же два условия, по которым здесь стартовал бы настоящий ран: флаг «коснулся земли
	// внутри зоны» (его проверяет KZTimerService::StartZoneEndTouch) и бескурсовой гард TimerStart
	// (CanStartRunHere: жив, таймер не стартовал только что, не телепортировался, не в перфе, не
	// наноклипился, валидный movetype, достаточно постоял на земле, на земле или в валидном прыжке).
	// Один список на два вызывающих — иначе репетиция старта давала бы время там, где ран бы не
	// завёлся. Сюда же попадают и «выходы», которых игрок не делал: телепорт на prac-точку из зоны
	// и EndTouchAll от ноуклипа или смерти (KZTriggerService::UpdateTriggerTouchList) — их снимают
	// JustTeleported и IsAlive/HasValidMoveType внутри гарда.
	if (!this->player->timerService->GetTouchedGroundInStartZone() || !this->player->timerService->CanStartRunHere())
	{
		return;
	}
	// Часы — С НУЛЯ: зона это начало попытки. Сброс ЧАСОВ, а не prac: замороженный ран, prac-точки,
	// живые чекпоинты и tpCount остаются нетронутыми (в точках лежат свои показания часов, !practp
	// по-прежнему откручивает к ним, а показание рана на входе — в точке №1).
	// Отрицательный субтиковый офсет — тот же приём, что в TimerStart: prac-часы тикают полными
	// тиками в OnPhysicsSimulatePost, а зону игрок пересёк внутри тика. Без офсета репетиция
	// систематически шла бы дольше рана на величину до тика — при том, что она для сравнения с ним
	// и существует. Худ отрицательного не увидит: DrawPanels идёт после инкремента (kz_player.cpp).
	this->pracTime = g_pKZUtils->GetGlobals()->curtime - g_pKZUtils->GetServerGlobals()->curtime;
	this->pracTimeRunning = true;
	// Курс попытки — чтобы финиш чужого курса не печатал время про забег, которого не было.
	this->pracCourseGUID = course ? course->guid : 0;
	// INFO: смена состояния, которой нет ни в БД, ни в событиях (в prac ничего не сабмитится) —
	// без неё жалоба «в отработке не идёт время» неразличима с «зона не сработала». Кулдауна нет
	// намеренно: строка = одна начатая попытка, а попытку игрок начинает ногами, не биндом.
	KZ_LOG_INFO(LogChannel::Timer, "[cyb] prac_attempt_start steam_id=%llu course=%s\n", this->player->GetSteamId64(false),
				course ? course->name : "unknown");
}

bool KZPracService::OnEndZoneTouch(const KZCourseDescriptor *course)
{
	// Часы стоят (или prac нет вовсе) — печатать нечего, отдаём касание обычному тракту:
	// там оно упрётся в !timerRunning и даст привычный false-end.
	if (!this->inPrac || !this->pracTimeRunning)
	{
		return false;
	}
	// Финиш ЧУЖОГО курса: попытка начата в другом месте (вышел из старта главного — забежал в
	// финиш бонуса), печатать про неё время нельзя. 0 = курс попытки неизвестен (часы пришли с
	// забранной у наблюдаемого точки) — тогда не выбираем за игрока и печатаем как раньше.
	if (this->pracCourseGUID != 0 && course && course->guid != this->pracCourseGUID)
	{
		return false;
	}
	char timeText[32];
	utils::FormatTime(this->pracTime, timeText, sizeof(timeText));
	this->player->languageService->PrintChat(true, false, "Prac - Finish", timeText);
	// И это всё: настоящий TimerEnd не зовём вовсе, поэтому ни Times, ни реплей, ни PB/WR,
	// ни kz.run_finished, ни инвалидация SavedRuns не задействованы (весь этот тракт висит
	// внутри KZTimerService::TimerEnd). Часы продолжают идти, prac и точки остаются.
	return true;
}

void KZPracService::TogglePrac()
{
	if (this->inPrac)
	{
		this->ExitPrac();
	}
	else
	{
		this->EnterPrac();
	}
}

void KZPracService::EnterPrac()
{
	if (!kz_prac_enable.Get())
	{
		this->player->languageService->PrintChat(true, false, "Prac - Disabled");
		this->player->PlayErrorSound();
		return;
	}
	if (!this->RequireLivePawn(true))
	{
		return;
	}
	// Гонка: prac заморозил бы хронометр участника, пока остальные бегут, и вернул его со
	// штрафом +1 TP, которого лимит телепортов заезда не видел.
	if (this->player->racingService->IsInActiveRace())
	{
		this->player->languageService->PrintChat(true, false, "Prac - In Race");
		this->player->PlayErrorSound();
		return;
	}

	const bool timerRunning = this->player->timerService->GetTimerRunning();
	// kz_prac_run_policy 1 - «ран не засчитывается»: не замораживаем его вовсе (засчитывать
	// нечего, если рана нет), см. ниже.
	const bool discardRun = timerRunning && kz_prac_run_policy.Get() == 1;
	if (timerRunning)
	{
		// !pro существует ровно для «не дай мне сломать PRO», а prac ломает его как телепорт
		// (при policy 1 — ещё и целиком, поэтому гард нужен тем более).
		if (!this->player->timerService->CheckSafeguardPro())
		{
			return;
		}
		// Гард CanPause убран (ревизия 2): в prac можно входить в воздухе — он стал репетицией,
		// а не паузой, и возврат умеет вернуть скорость входа. Уходят вместе с ним midair,
		// just-landed и кулдаун паузы; antipause-зона приходила оттуда же, поэтому проверяем её
		// сами, той же фразой отказа — карты ставят такие зоны намеренно.
		if (this->player->triggerService->InAntiPauseArea())
		{
			this->player->languageService->PrintChat(true, false, "Can't Pause (Anti Pause Area)");
			this->player->PlayErrorSound();
			return;
		}
		const bool wasPaused = this->player->timerService->GetPaused();

		// Публичного геттера GUID у таймера нет, но есть GetCourse() -> дескриптор (kz_timer.h:409).
		const KZCourseDescriptor *courseDesc = this->player->timerService->GetCourse();
		if (!courseDesc)
		{
			this->player->languageService->PrintChat(true, false, "No Active Course");
			this->player->PlayErrorSound();
			return;
		}

		// Пауза несовместима с полётом: ForcePause ставит MOVETYPE_NONE и gravity 0. Снимаем
		// её ДО любых мутаций prac-состояния — если листенер вето́рует OnResume, отказ остаётся
		// бесплатным (частично применённого состояния нет). force=true: кулдаун CanResume тут
		// не при чём, игрок уходит не в ран. Порядок важен и для реплея: TIMER_RESUME закрывает
		// СВОЮ паузу игрока до того, как ниже откроется prac-отрезок.
		if (wasPaused)
		{
			this->player->timerService->Resume(true);
			if (this->player->timerService->GetPaused())
			{
				// Причину напечатал сам Resume.
				return;
			}
		}

		if (discardRun)
		{
			// Ран не замораживаем и не восстановим: политика запрещает его засчитывать.
			// Сейв SavedRuns тоже сносим — иначе реконнект вернул бы ран, которого по этой
			// политике быть не должно. inPrac ещё false, поэтому TimerStop здесь честно
			// доводит ран до конца (рекордер реплея закрывается, TIMER_STOP пишется).
			this->frozen = {};
			this->player->savedRunService->InvalidateCurrent("prac_run_policy");
			this->player->timerService->TimerStop(true, "prac_discard");
		}
		else
		{
			// С этой строки и до this->inPrac = true ниже инвариант «frozen.active только при
			// inPrac» кратковременно не держится (frozen.active уже true, inPrac ещё false).
			// Безвредно: код синхронный, между ними нет колбэков/тиков, никто снаружи в это
			// окно prac-состояние не читает (см. HasActiveFrozenRun() и её потребителей).
			this->frozen = {};
			this->frozen.active = true;
			this->frozen.courseGUID = courseDesc->guid;
			this->frozen.timer = this->player->timerService->SnapshotForSave();
			const CUtlVector<KZCheckpointService::Checkpoint> &cps = this->player->checkpointService->GetCheckpointsForSave();
			FOR_EACH_VEC(cps, i)
			{
				this->frozen.checkpoints.AddToTail(cps[i]);
			}
			this->frozen.cpIndex = this->player->checkpointService->GetRawCpIndex();
			// Штраф печём в снапшот СРАЗУ, а не при возврате: тогда и возврат через !prac, и
			// восстановление после дисконнекта (Task 7) дают одинаковый результат без дублей логики.
			// +1 телепорт = ран становится NUB.
			this->frozen.tpCount = this->player->checkpointService->GetTeleportCount() + 1;
			this->player->GetOrigin(&this->frozen.origin);
			this->player->GetAngles(&this->frozen.angles);
			// Скорость входа — для возврата (ревизия 2). Пауза на возврате уместна ТОЛЬКО если
			// игрок реально стоял: она обнуляет скорость, поэтому вошедшему на бегу или в полёте
			// вернула бы не то состояние (решение пользователя 25.07 — сохранять скорость бега).
			// wasPaused — тоже «стоял»: паузу можно поставить только стоя, а скорость под
			// MOVETYPE_NONE уже нулевая.
			this->player->GetVelocity(&this->frozen.velocity);
			this->frozen.enteredStill = wasPaused || this->frozen.velocity.Length() < KZ_PRAC_STILL_SPEED;
			SnapshotModeStyles(this->player, this->frozen.modeName, sizeof(this->frozen.modeName), this->frozen.styles,
							   sizeof(this->frozen.styles));

			// inPrac поднимаем ДО TimerStop: KZRecordingService::OnTimerStop по этому флагу
			// узнаёт, что ран не кончился, и НЕ убивает рекордер рана — иначе финиш получил бы
			// нулевой UUID (коллизия PRIMARY KEY в Times, ран игрока исчезал бы молча).
			// Ноуклип включается ниже, так что «карательная» ветка HandleNoclip тоже подавлена.
			this->inPrac = true;
			this->player->timerService->TimerStop(false, "prac");
			// Граница prac в реплее: TimerStop в prac события паузы не даёт, а плеер
			// пропускает именно отрезок PAUSE..RESUME.
			this->player->recordingService->OnPause();
		}
	}
	else
	{
		this->frozen = {};
	}

	this->inPrac = true;
	this->ClearPoints();
	// Ноуклип НЕ включаем (решение пользователя 25.07): вход в prac только замораживает ран,
	// а летать игрок начинает сам через !nc. Внутри prac ноуклип безопасен — «карательная»
	// ветка HandleNoclip подавлена по inPrac, таймер уже остановлен.

	// prac-часы: с раном — продолжают время рана (репетиция идёт дальше), без рана — стоят в 0
	// до первого выхода из стартовой зоны (OnStartZoneEndTouch), которая и заводит попытку.
	// MAX: снапшот, снятый на самом тике старта рана, содержит отрицательный субтиковый офсет
	// (см. TimerStart) — в prac-часах он не нужен.
	this->pracTime = this->frozen.active ? MAX(0.0, this->frozen.timer.time) : 0.0;
	this->pracTimeRunning = this->frozen.active;
	// Курс попытки — курс замороженного рана: финиш печатается только на нём (см. pracCourseGUID).
	this->pracCourseGUID = this->frozen.active ? this->frozen.courseGUID : 0;
	// Точка №1 — текущее состояние (со скоростью и показанием часов): !practp сразу после входа
	// возвращает ровно туда, откуда игрок вошёл, включая полёт. Общий путь захвата, без гардов
	// (пешка проверена в начале) и без своего сообщения — про точку говорит фраза входа ниже.
	this->CapturePoint();

	if (this->frozen.active)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Enter With Run");
	}
	else if (discardRun)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Enter Run Discarded");
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Prac - Enter Free");
	}
}

void KZPracService::ExitPrac()
{
	// Все три шага возврата (HandleNoclip / Teleport / ForcePause) разыменовывают пешку и move
	// services без проверок, а из prac можно уйти в спек (CanSpectate сводится к CanPause, а
	// та в prac не проверяет midair — таймер стоит). Отказ ДО любых мутаций: замороженный ран
	// при этом жив, игрок вернётся в команду и повторит !prac.
	if (!this->RequireLivePawn(true))
	{
		return;
	}

	this->player->noclipService->DisableNoclip();
	this->player->noclipService->HandleNoclip();

	if (!this->frozen.active)
	{
		this->inPrac = false;
		this->ClearPoints();
		this->ResetPracTime();
		this->player->languageService->PrintChat(true, false, "Prac - Exit Free");
		return;
	}

	// Замороженный ран валиден только при неизменных режиме и стилях: RunSubmission читает
	// ТЕКУЩИЕ modeService/styleServices (submission.cpp), а SwitchToMode/AddStyle защищают ран
	// единственным TimerStop, который в prac — no-op. Без этой сверки «!prac / !mode vnl /
	// !prac / финиш» отправляло бы время с классической физики в таблицу Vanilla.
	char modeNow[sizeof(this->frozen.modeName)];
	char stylesNow[sizeof(this->frozen.styles)];
	SnapshotModeStyles(this->player, modeNow, sizeof(modeNow), stylesNow, sizeof(stylesNow));
	const bool modeChanged = !KZ_STREQI(modeNow, this->frozen.modeName);
	if (modeChanged || !KZ_STREQI(stylesNow, this->frozen.styles))
	{
		this->player->languageService->PrintChat(true, false, "Prac - Run Lost Mode Changed");
		this->player->PlayErrorSound();
		// Причины из словаря TimerStop (kz_timer.h): различаем режим и стили, иначе разбор
		// «почему ран не уехал в лидерборд» упирается в одно общее значение.
		this->DropFrozenRun(modeChanged ? "mode_change" : "style_change", nullptr);
		return;
	}

	// inPrac снимаем ДО восстановления: RestoreFromSnapshot поднимает timerRunning,
	// и дальше листенер OnTimerStart уже не должен ничего вето́ровать.
	// С этой строки и до this->frozen = {} ниже инвариант «frozen.active только при inPrac»
	// снова кратковременно не держится (inPrac уже false, frozen.active ещё true) — тот же
	// безвредный синхронный случай, что и в EnterPrac.
	this->inPrac = false;

	this->player->timerService->RestoreFromSnapshot(this->frozen.courseGUID, this->frozen.timer);
	// Тот же случай, что в SavedRuns: RestoreFromSnapshot листенеров не стреляет, поэтому UUID
	// рану никто не выдаёт. В обычном prac-ране это no-op — рекордер реплея пережил prac (гейт в
	// KZRecordingService::OnTimerStop) и UUID выдаст OnTimerEnd из него. Строка нужна для рана,
	// который сам пришёл из SavedRuns (рекордера нет) и потом ещё раз прошёл через prac.
	this->player->recordingService->EnsureRunUUIDAfterRestore("prac_exit");
	// tpCount в снапшоте уже со штрафом (+1, см. EnterPrac) — здесь только применяем.
	// SetTeleportCountForRestore не нужен: счётчик бампает KZCheckpointService::DoTeleport,
	// а мы телепортируем напрямую через KZPlayer::Teleport, который его не трогает.
	this->player->checkpointService->RestoreFromSnapshot(this->frozen.checkpoints, this->frozen.cpIndex, this->frozen.tpCount);
	// Возврат со скоростью входа (ревизия 2): вошёл стоя — она нулевая и её всё равно съест
	// пауза; вошёл на бегу или в полёте — это единственный способ вернуть то же состояние.
	const bool still = this->frozen.enteredStill;
	this->player->Teleport(&this->frozen.origin, &this->frozen.angles, &this->frozen.velocity);
	this->player->recordingService->OnResume();
	// Пауза только если игрок стоял: ForcePause обнуляет скорость и ставит MOVETYPE_NONE, то
	// есть съела бы ровно то, что мы вернули выше. Вошёл на бегу или в полёте — таймер сразу
	// идёт, игрок продолжает движение (об этом отдельная фраза ниже).
	// ForcePause() возвращает void и молча не поставит паузу, если какой-то листенер
	// провалит OnPause() (сейчас таких нет) — а чат ниже безусловно говорит "на паузе".
	// Если появится реальное вето, эту пару придётся согласовать явно.
	if (still)
	{
		this->player->timerService->ForcePause();
	}

	this->frozen = {};
	this->ClearPoints();
	this->ResetPracTime();
	this->player->languageService->PrintChat(true, false, still ? "Prac - Exit To Run" : "Prac - Exit To Run Moving");
}

void KZPracService::DropFrozenRun(const char *reason, const char *phrase)
{
	if (!this->inPrac && !this->frozen.active)
	{
		return;
	}
	const bool hadRun = this->frozen.active;
	// Курс запоминаем ДО сброса frozen ниже — иначе в лог уйдёт уже обнулённый GUID.
	const u32 lostCourseGUID = this->frozen.courseGUID;
	this->inPrac = false;
	this->frozen = {};
	this->ClearPoints();
	this->ResetPracTime();
	if (hadRun)
	{
		// Рекордер реплея переживает prac (см. KZRecordingService::OnTimerStop) — если ран
		// потерян, закрыть его надо здесь и вручную: живой таймер уже остановлен, поэтому
		// TimerStop листенеров не позовёт, а брошенный рекордер копил бы тики следующего рана
		// (и второй рекордер на его старте). inPrac уже false, так что гард prac пропускает.
		this->player->recordingService->OnTimerStop();
		if (phrase)
		{
			this->player->languageService->PrintChat(true, false, phrase);
		}
		if (reason)
		{
			// INFO, а не DEBUG: без -debug каналы регистрируются с LV_DEFAULT (utils/logging.cpp),
			// то есть на проде строки бы не было. Уничтожение рана — отказ, а последнее событие
			// игрока перед ним (run_stop reason=prac) читается как «сам приостановил», не «потерял».
			const KZCourseDescriptor *lostCourse = KZ::course::GetCourse(lostCourseGUID);
			KZ_LOG_INFO(LogChannel::Timer, "[cyb] run_lost steam_id=%llu course=%s reason=%s\n", this->player->GetSteamId64(false),
						lostCourse ? lostCourse->name : "unknown", reason);
		}
	}
}

void KZPracService::DropFrozenRunAll(const char *reason)
{
	for (int i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->pracService)
		{
			continue;
		}
		// Сам DropFrozenRun no-op вне prac, так что проходить всех дёшево и безопасно.
		player->pracService->DropFrozenRun(reason);
	}
}

bool KZPracService::RequireLivePawn(bool showError)
{
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();
	if (pawn && pawn->IsAlive() && this->player->GetMoveServices())
	{
		return true;
	}
	if (showError)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Must Be Alive");
		this->player->PlayErrorSound();
	}
	return false;
}

bool KZPracService::RequirePrac()
{
	if (!this->inPrac)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Only In Prac");
		this->player->PlayErrorSound();
		return false;
	}
	return true;
}

bool KZPracService::RequirePracPoint()
{
	if (!this->RequirePrac())
	{
		return false;
	}
	if (this->points.Count() == 0)
	{
		this->player->languageService->PrintChat(true, false, "Prac - No Points");
		this->player->PlayErrorSound();
		return false;
	}
	return true;
}

void KZPracService::SetPoint()
{
	if (!this->RequirePrac())
	{
		return;
	}
	// Развод гарда пешки. Спектатор в prac забирает состояние НАБЛЮДАЕМОГО — своей пешки у него
	// нет, и RequireLivePawn тут не «сломан», а неприменим: этот путь свою пешку не трогает
	// вовсе (ни Teleport, ни HandleNoclip, ни ForcePause), а живость проверяет у чужой.
	// Для своей пешки гард обязателен и остаётся ниже: CapturePointFrom разыменовывает pawn.
	// Условие ровно то же, что у GetSpectatedPlayer (он отдаёт цель только мёртвому/спектатору).
	if (!this->player->IsAlive())
	{
		this->StealPointFromSpectated();
		return;
	}
	if (!this->RequireLivePawn(true))
	{
		return;
	}
	this->CapturePoint();
	this->player->languageService->PrintChat(true, false, "Prac - Point Set", this->points.Count());
	this->player->checkpointService->PlayCheckpointSound();
}

void KZPracService::CapturePoint()
{
	this->CapturePointFrom(this->player, this->pracTime, this->pracTimeRunning, this->pracCourseGUID);
}

void KZPracService::CapturePointFrom(KZPlayer *source, f64 time, bool timeRunning, u32 courseGUID)
{
	CCSPlayerPawn *pawn = source->GetPlayerPawn();

	PracPoint pt = {};
	source->GetOrigin(&pt.origin);
	source->GetVelocity(&pt.velocity);
	source->GetAngles(&pt.angles);
	pt.onGround = (pawn->m_fFlags() & FL_ONGROUND) != 0;
	CCSPlayer_MovementServices *ms = source->GetMoveServices();
	if (ms)
	{
		pt.duckAmount = ms->m_flDuckAmount;
		pt.stamina = ms->m_flStamina;
		pt.ladderNormal = ms->m_vecLadderNormal();
		pt.onLadder = pawn->m_MoveType() == MOVETYPE_LADDER;
	}
	// Показание prac-часов — часть точки: !practp откручивает их назад вместе с позицией.
	// Для своей точки это свои часы, для забранной — часы наблюдаемого (см. StealPointFromSpectated).
	pt.pracTime = time;
	pt.pracTimeRunning = timeRunning;
	// Курс — часть той же попытки, что и часы, поэтому и хранится в точке (см. PracPoint::courseGUID).
	// Без часов курса нет — тот же инвариант, что в ResetPracTime.
	pt.courseGUID = timeRunning ? courseGUID : 0;

	this->points.AddToTail(pt);
	this->currentIndex = this->points.Count() - 1;
}

bool KZPracService::StealPointFromSpectated()
{
	// Свободная камера или свой труп: GetSpectatedPlayer отдаёт nullptr — забирать нечего.
	KZPlayer *target = this->player->specService->GetSpectatedPlayer();
	if (!target || target == this->player)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Steal No Target");
		this->player->PlayErrorSound();
		return false;
	}
	// Реплей-бот (и любой фейк-клиент): его «состояние» — воспроизведение чужой записи, режим и
	// стили у него не игрока, а плеера, и никакой действующей попытки за ним нет.
	if (KZ::replaysystem::IsReplayBot(target) || target->IsFakeClient())
	{
		this->player->languageService->PrintChat(true, false, "Prac - Steal Bot");
		this->player->PlayErrorSound();
		return false;
	}
	// Мёртвый/респавнящийся наблюдаемый: CapturePointFrom разыменовывает пешку, а move services
	// нужны для приседа/stamina/лестницы. modeService — для сверки ниже (SnapshotModeStyles его
	// разыменовывает). Момент между смертью цели и переключением наблюдателя реально достижим.
	CCSPlayerPawn *targetPawn = target->GetPlayerPawn();
	if (!targetPawn || !targetPawn->IsAlive() || !target->GetMoveServices() || !target->modeService)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Steal Target Dead");
		this->player->PlayErrorSound();
		return false;
	}
	// Ноуклип наблюдаемого: по правилам prac пролёт насквозь обнуляет попытку, значит и точка от
	// такого игрока бессмысленна — ни позиция (может быть внутри геометрии), ни скорость (полётная),
	// ни время (у него уже обнулено) не воспроизводимы вживую. Проверяем И флаг сервиса, И реальный
	// movetype: ноуклип от админа/другого плагина наш флаг не ставит.
	if ((target->noclipService && target->noclipService->IsNoclipping()) || targetPawn->m_MoveType() == MOVETYPE_NOCLIP)
	{
		this->player->languageService->PrintChat(true, false, "Prac - Steal Target Noclip");
		this->player->PlayErrorSound();
		return false;
	}
	// Режим И стили должны совпадать: автобхоп и legacy-jump меняют достижимое из позиции не
	// меньше режима, поэтому «сколько выбью из этого положения» иначе несравнимо. Сверка — тем же
	// каноническим видом, что у ключа SavedRuns и у выхода из prac (SnapshotModeStyles).
	// Сравниваем ТЕКУЩИЕ режим/стили обоих, а не frozen: физика, с которой забравший будет бежать
	// от точки, — это его текущая физика (а ран, переживший смену режима в prac, всё равно
	// потеряется на выходе, см. ExitPrac).
	char modeMine[sizeof(this->frozen.modeName)];
	char stylesMine[sizeof(this->frozen.styles)];
	char modeTheirs[sizeof(this->frozen.modeName)];
	char stylesTheirs[sizeof(this->frozen.styles)];
	SnapshotModeStyles(this->player, modeMine, sizeof(modeMine), stylesMine, sizeof(stylesMine));
	SnapshotModeStyles(target, modeTheirs, sizeof(modeTheirs), stylesTheirs, sizeof(stylesTheirs));
	if (!KZ_STREQI(modeMine, modeTheirs) || !KZ_STREQI(stylesMine, stylesTheirs))
	{
		char theirsText[96];
		char mineText[96];
		FormatModeStyles(modeTheirs, stylesTheirs, theirsText, sizeof(theirsText));
		FormatModeStyles(modeMine, stylesMine, mineText, sizeof(mineText));
		this->player->languageService->PrintChat(true, false, "Prac - Steal Mode Mismatch", target->GetName(), theirsText, mineText);
		this->player->PlayErrorSound();
		return false;
	}

	// Время наблюдаемого. Порядок важен: у игрока В PRAC настоящий таймер остановлен по
	// инварианту, поэтому сначала живой ран, потом prac-часы. Ни того, ни другого — точка ложится
	// без действующей попытки (pracTimeRunning=false), и !practp время из ничего не родит.
	// MAX: на самом тике старта рана currentTime содержит отрицательный субтиковый офсет.
	f64 stolenTime = 0.0;
	bool stolenRunning = false;
	if (target->timerService && target->timerService->GetTimerRunning())
	{
		stolenTime = MAX(0.0, target->timerService->GetTime());
		stolenRunning = true;
	}
	else if (target->pracService && target->pracService->IsInPrac() && target->pracService->IsPracTimeRunning())
	{
		stolenTime = MAX(0.0, target->pracService->GetPracTime());
		stolenRunning = true;
	}

	// Курс наблюдаемого — чтобы игрок видел, ОТКУДА точка: забор с чужого курса разрешён
	// (решение пользователя 25.07), поэтому курс надо показывать, а не подразумевать.
	// Резолв в том же порядке, что и время, но чуть шире: у игрока в prac курс известен из
	// замороженного рана даже когда его prac-часы стоят (попытка недействительна, курс — нет).
	const KZCourseDescriptor *targetCourse = nullptr;
	if (target->timerService && target->timerService->GetTimerRunning())
	{
		targetCourse = target->timerService->GetCourse();
	}
	else if (target->pracService && target->pracService->HasActiveFrozenRun())
	{
		targetCourse = KZ::course::GetCourse(target->pracService->GetFrozenRun().courseGUID);
	}
	// Имя курса, а не cyber-номер: номер читается только для main (0) и «Bonus N», а не-главный
	// не-бонусный курс уезжает в 100+ (GetCyberCourseNumber) и в чате выглядел бы мусором.
	// Курса может не быть вовсе — наблюдаемый просто ходит по карте; тогда фраза-фрагмент под
	// локаль игрока (тот же приём, что у Map Info в kz_mappingapi.cpp). Держим в std::string:
	// PrepareMessage возвращает по значению, .c_str() от временного объекта повис бы.
	std::string courseText =
		targetCourse ? std::string(targetCourse->name) : this->player->languageService->PrepareMessage("Prac - Course Unknown");

	// Курс наблюдаемого едет в точку вместе с его временем: !practp на неё продолжает ЕГО попытку,
	// значит и финиш должен печататься на ЕГО курсе, а не на том, где забравший был до этого.
	this->CapturePointFrom(target, stolenTime, stolenRunning, targetCourse ? targetCourse->guid : 0);
	if (stolenRunning)
	{
		char timeText[32];
		utils::FormatTime(stolenTime, timeText, sizeof(timeText));
		this->player->languageService->PrintChat(true, false, "Prac - Point Stolen", target->GetName(), this->points.Count(), courseText.c_str(),
												 timeText);
	}
	else
	{
		this->player->languageService->PrintChat(true, false, "Prac - Point Stolen No Time", target->GetName(), this->points.Count(),
												 courseText.c_str());
	}
	this->player->checkpointService->PlayCheckpointSound();
	return true;
}

void KZPracService::DoTpToPoint(const PracPoint &pt)
{
	if (!this->RequireLivePawn(true))
	{
		return;
	}
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();

	// Ноуклип снимаем всегда: смысл practp — продолжить движение по-настоящему.
	this->player->noclipService->DisableNoclip();
	this->player->noclipService->HandleNoclip();

	// В отличие от обычного чекпоинта передаём НЕнулевую скорость — это вся суть prac-точки.
	this->player->Teleport(&pt.origin, &pt.angles, &pt.velocity);

	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (ms)
	{
		ms->m_flDuckAmount(pt.duckAmount);
		ms->m_flStamina(pt.stamina);
		if (pt.onLadder)
		{
			ms->m_vecLadderNormal(pt.ladderNormal);
			this->player->SetMoveType(MOVETYPE_LADDER);
		}
		else
		{
			ms->m_vecLadderNormal(vec3_origin);
			this->player->SetMoveType(MOVETYPE_WALK);
		}
	}
	if (pt.onGround)
	{
		pawn->m_fFlags(pawn->m_fFlags() | FL_ONGROUND);
	}
	// Часы откручиваем к показанию точки и продолжаем — в этом смысл репетиции по кускам.
	// Ставим ПОСЛЕ снятия ноуклипа выше, иначе OnNoclipEnabled успел бы их обнулить.
	// Признак «шли» тоже из точки: точка, снятая без действующей попытки, не должна
	// рождать время из ничего (см. PracPoint).
	this->pracTime = pt.pracTime;
	this->pracTimeRunning = pt.pracTimeRunning;
	// Курс — из точки, вместе с часами: это возврат В ТУ попытку, из которой точка снята. Оставить
	// прежний GUID нельзя — тогда после прыжка на точку другой попытки (или на забранную у
	// наблюдаемого) финиш «своего» курса молчал бы, а финиш курса-владельца GUID печатал время
	// чужого забега. Отдельной защиты от «EndTouch стартовой зоны сразу после телепорта отсюда»
	// не нужно — его снимает JustTeleported внутри CanStartRunHere (см. OnStartZoneEndTouch).
	this->pracCourseGUID = pt.courseGUID;
	this->player->checkpointService->PlayTeleportSound();
}

void KZPracService::TpToPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::TpToPrevPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->currentIndex = MAX(0, this->currentIndex - 1);
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::TpToNextPoint()
{
	if (!this->RequirePracPoint())
	{
		return;
	}
	this->currentIndex = MIN(this->points.Count() - 1, this->currentIndex + 1);
	this->DoTpToPoint(this->points[this->currentIndex]);
}

void KZPracService::ResetPoints()
{
	if (!this->RequirePrac())
	{
		return;
	}
	this->ClearPoints();
	this->player->languageService->PrintChat(true, false, "Prac - Points Cleared");
	this->player->checkpointService->PlayCheckpointResetSound();
}

void KZPracService::OnJoinSpectator()
{
	if (!this->inPrac)
	{
		return;
	}
	// Запоминаем, летел ли игрок: с 25.07 вход в prac ноуклип НЕ включает, поэтому на
	// возврате из спека его нельзя включать безусловно — включили бы тому, кто не летал.
	this->noclipBeforeSpec = this->player->noclipService->IsNoclipping();
	// Только флаг: HandleNoclip() здесь звать НЕЛЬЗЯ - он безусловно разыменовывает
	// GetPlayerPawn() (kz_noclip.cpp), а у обсервера собственной пешки нет (null-deref).
	this->player->noclipService->DisableNoclip();
	// Точки НЕ чистим (решение пользователя 25.07): вернувшись из спектаторов, игрок хочет
	// продолжить prac'ать свой ран с теми же точками, включая авто-точку №1 со входа.
}

void KZPracService::OnPlayerSpawn()
{
	if (!this->inPrac)
	{
		return;
	}
	// Возвращаем РОВНО то состояние ноуклипа, что было при уходе в спек (вход в prac его больше
	// не включает). Флаг ставим безусловно, а применяем только по живой пешке: KZTimerService::
	// OnPlayerSpawn зовёт нас вне своей проверки пешки, а HandleNoclip разыменовывает её без
	// чеков. Если пешка на этом тике ещё не готова, флаг доиграет HandleMoveCollision на
	// следующем физическом тике (kz_player.cpp), поэтому отказ здесь ничего не теряет.
	if (!this->noclipBeforeSpec)
	{
		return;
	}
	this->player->noclipService->EnableNoclip();
	if (this->RequireLivePawn(false))
	{
		this->player->noclipService->HandleNoclip();
	}
}
