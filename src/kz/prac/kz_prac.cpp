#include "kz_prac.h"

#include "kz/language/kz_language.h"
#include "kz/mappingapi/kz_mappingapi.h"
#include "kz/mode/kz_mode.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/racing/kz_racing.h"
#include "kz/recording/kz_recording.h"
#include "kz/savedrun/kz_savedrun.h"
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

void KZPracService::Reset()
{
	this->inPrac = false;
	this->noclipBeforeSpec = false;
	this->frozen = {};
	this->ClearPoints();
}

void KZPracService::ClearPoints()
{
	this->points.RemoveAll();
	this->currentIndex = 0;
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
		// Гарды входа = гарды паузы (не в воздухе, не сразу после приземления, не в
		// antipause-зоне, не под кулдауном). Сообщения печатает сам CanPause.
		// Исключение — уже стоящая пауза: CanPause отдаёт на ней false МОЛЧА, а по смыслу
		// пауза и есть нужное нам «игрок стоит», поэтому вилка явная.
		const bool wasPaused = this->player->timerService->GetPaused();
		if (!wasPaused && !this->player->timerService->CanPause(true))
		{
			return;
		}

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
	// tpCount в снапшоте уже со штрафом (+1, см. EnterPrac) — здесь только применяем.
	// SetTeleportCountForRestore не нужен: счётчик бампает KZCheckpointService::DoTeleport,
	// а мы телепортируем напрямую через KZPlayer::Teleport, который его не трогает.
	this->player->checkpointService->RestoreFromSnapshot(this->frozen.checkpoints, this->frozen.cpIndex, this->frozen.tpCount);
	this->player->Teleport(&this->frozen.origin, &this->frozen.angles, &vec3_origin);
	this->player->recordingService->OnResume();
	// ForcePause() возвращает void и молча не поставит паузу, если какой-то листенер
	// провалит OnPause() (сейчас таких нет) — а чат ниже безусловно говорит "на паузе".
	// Если появится реальное вето, эту пару придётся согласовать явно.
	this->player->timerService->ForcePause();

	this->frozen = {};
	this->ClearPoints();
	this->player->languageService->PrintChat(true, false, "Prac - Exit To Run");
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
	if (!this->RequirePrac() || !this->RequireLivePawn(true))
	{
		return;
	}
	CCSPlayerPawn *pawn = this->player->GetPlayerPawn();

	PracPoint pt = {};
	this->player->GetOrigin(&pt.origin);
	this->player->GetVelocity(&pt.velocity);
	this->player->GetAngles(&pt.angles);
	pt.onGround = (pawn->m_fFlags() & FL_ONGROUND) != 0;
	CCSPlayer_MovementServices *ms = this->player->GetMoveServices();
	if (ms)
	{
		pt.duckAmount = ms->m_flDuckAmount;
		pt.stamina = ms->m_flStamina;
		pt.ladderNormal = ms->m_vecLadderNormal();
		pt.onLadder = pawn->m_MoveType() == MOVETYPE_LADDER;
	}

	this->points.AddToTail(pt);
	this->currentIndex = this->points.Count() - 1;
	this->player->languageService->PrintChat(true, false, "Prac - Point Set", this->points.Count());
	this->player->checkpointService->PlayCheckpointSound();
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
	this->ClearPoints();
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
