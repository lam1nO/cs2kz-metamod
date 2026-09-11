#include "cstrike15_usermessages.pb.h"
#include "usermessages.pb.h"
#include "gameevents.pb.h"
#include "cs_gameevents.pb.h"

#include "sdk/entity/cparticlesystem.h"
#include "sdk/services.h"

#include "kz_quiet.h"
#include "kz/pistol/kz_pistol.h"
#include "kz/beam/kz_beam.h"
#include "kz/measure/kz_measure.h"
#include "kz/lead/kz_lead.h"
#include "kz/ztopwatch/kz_ztopwatch.h"
#include "kz/hud/kz_hud.h"
#include "kz/option/kz_option.h"
#include "kz/paint/kz_paint.h"
#include "kz/zones/kz_zones.h" // разыменовываем zonesService (белый список рёбер зон)
#include "kz/language/kz_language.h"

#include "utils/utils.h"
#include "utils/simplecmds.h"
#include "utils/logging.h"

static_global class KZOptionServiceEventListener_Quiet : public KZOptionServiceEventListener
{
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player)
	{
		player->quietService->OnPlayerPreferencesLoaded();
	}
} optionEventListener;

// Отрезок !lead с нашим targetname, но БЕЗ метки команды: один из двух ключей владения потерян.
// Сам луч при этом остаётся личным (второй ключ отработал), но причина обязана быть в логе —
// такой отказ иначе не виден никак. Дроссель по времени: место горячее (CheckTransmit на каждого
// получателя), а часы движка на смене карты обнуляются, поэтому отметка «из будущего» = рестарт.
static_function void LeadBeamMarkerLost(int team)
{
	static f64 lastWarn = -1.0e9;
	const f64 now = g_pKZUtils->GetServerGlobals() ? g_pKZUtils->GetServerGlobals()->realtime : 0.0;
	if (now < lastWarn)
	{
		lastWarn = -1.0e9;
	}
	if (now - lastWarn < KZ_LEAD_WARN_THROTTLE_SEC)
	{
		return;
	}
	lastWarn = now;
	KZ_LOG_WARN(LogChannel::Replays, "[lead] beam_marker_lost team=%i want=%i note=identified_by_targetname_visibility_still_private\n", team,
				(int)KZ_LEAD_SEGMENT_TEAM);
}

void KZ::quiet::OnCheckTransmit(CCheckTransmitInfo **pInfo, int infoCount)
{
	for (int i = 0; i < infoCount; i++)
	{
		// Cast it to our own TransmitInfo struct because CCheckTransmitInfo isn't correct.
		TransmitInfo *pTransmitInfo = reinterpret_cast<TransmitInfo *>(pInfo[i]);

		// Find out who this info will be sent to.
		uintptr_t targetAddr = reinterpret_cast<uintptr_t>(pTransmitInfo) + g_pGameConfig->GetOffset("QuietPlayerSlot");
		CPlayerSlot targetSlot = CPlayerSlot(*reinterpret_cast<int *>(targetAddr));
		KZPlayer *targetPlayer = g_pKZPlayerManager->ToPlayer(targetSlot);
		// Make sure the target isn't CSTV.
		CCSPlayerController *targetController = targetPlayer->GetController();
		if (!targetController || targetController->m_bIsHLTV)
		{
			continue;
		}
		targetPlayer->quietService->UpdateHideState();
		CCSPlayerPawn *targetPlayerPawn = targetPlayer->GetPlayerPawn();

		EntityInstanceByClassIter_t iterParticleSystem(NULL, "info_particle_system");

		for (CParticleSystem *particleSystem = static_cast<CParticleSystem *>(iterParticleSystem.First()); particleSystem;
			 particleSystem = static_cast<CParticleSystem *>(iterParticleSystem.Next()))
		{
			if (particleSystem->m_iTeamNum() != CUSTOM_PARTICLE_SYSTEM_TEAM)
			{
				continue; // Only hide custom particle systems created by the plugin.
			}
			if (targetPlayer->beamService->playerBeam == particleSystem->GetRefEHandle()
				|| targetPlayer->beamService->playerBeamNew == particleSystem->GetRefEHandle())
			{
				// Don't hide the beam for the owner.
				continue;
			}
			if (targetPlayer->measureService->measurerHandle == particleSystem->GetRefEHandle())
			{
				// Don't hide the measure beam for the owner.
				continue;
			}
			bool isZtopwatchEdge = false;
			for (int e = 0; e < KZZtopwatchService::Zone::NUM_EDGES; e++)
			{
				if (targetPlayer->ztopwatchService->startZone.edges[e] == particleSystem->GetRefEHandle()
					|| targetPlayer->ztopwatchService->endZone.edges[e] == particleSystem->GetRefEHandle())
				{
					isZtopwatchEdge = true;
					break;
				}
			}
			if (isZtopwatchEdge)
			{
				// Don't hide zone stopwatch edges for the owner.
				continue;
			}

			// hudService больше не владеет никакими particle-системами (particle-MHUD удалён в
			// задаче 12) — гейта на него здесь больше нет.

			// Рёбра зон, адресованные ЭТОМУ игроку: превью редактора (!zone start/end) и показ
			// !zone show. Без этой ветки метка CUSTOM_PARTICLE_SYSTEM_TEAM означает «не видит
			// никто, включая владельца» — именно так превью редактора и было невидимым.
			// Постоянного контура старта и финиша здесь нет намеренно: он метку не ставит вовсе,
			// уходит всем штатно и до этого места не доходит (отсев по m_iTeamNum выше).
			// HasOwnedParticles() первым: цикл крутится на каждый CheckTransmit для каждого
			// получателя, и у игрока без превью и без показа сравнений быть не должно.
			if (targetPlayer->zonesService && targetPlayer->zonesService->HasOwnedParticles()
				&& targetPlayer->zonesService->OwnsParticle(particleSystem->GetRefEHandle()))
			{
				continue;
			}
			// Отрезки луча !lead на ПРЕЖНЕМ примитиве (cyb_lead_beam_entity 0) — ровно по той же
			// причине: метка CUSTOM_PARTICLE_SYSTEM_TEAM означает «не видит никто», и владельцу
			// его собственный луч возвращает эта ветка.
			// HasOwnedSegments() первым: у игрока без луча сравнений быть не должно.
			if (targetPlayer->leadService && targetPlayer->leadService->HasOwnedSegments()
				&& targetPlayer->leadService->OwnsSegmentEntity(particleSystem->GetRefEHandle()))
			{
				continue;
			}
			pTransmitInfo->m_pTransmitEdict->Clear(particleSystem->GetEntityIndex().Get());
		}

		// Отрезки луча !lead на ШТАТНОЙ сущности-луче (дефолт, cyb_lead_beam_entity 1). Петля
		// выше их не видит вовсе — она перебирает только info_particle_system, и проба пробником
		// показала quiet_filtered=0, то есть сущность-луч уходит ВСЕМ по общим правилам. Луч
		// обязан оставаться ЛИЧНЫМ (требование владельца серверов: у каждого свой !lead, общего
		// луча на всех быть не должно), поэтому здесь логика ОБРАТНАЯ белому списку частиц:
		// гасим наши лучи всем, КРОМЕ владельца.
		//
		// «Наш ли это отрезок» решают ДВА независимых ключа, и это не перестраховка, а выбор
		// НАПРАВЛЕНИЯ ОТКАЗА. Если бы ключ был один (метка команды), то его потеря — спавном,
		// чужим кодом, будущей правкой — означала бы `continue`, то есть луч уходит ВСЕМ: ровно
		// тот исход, который владелец серверов запретил дословно, и невидимый со стороны сервера
		// (в логах пусто, узнаём от игроков). Поэтому: метка не совпала — добиваем сверкой
		// targetname, и только совпадение ОБОИХ «не наш» оставляет энтити в покое.
		//
		// Цена в НОРМЕ нулевая: NameMatches (вызов в движок) достаётся только лучам с чужой
		// меткой, а на KZ-картах лучей класса `beam` не бывает вовсе — карты ставят env_beam,
		// другой classname, и эта петля их не видит. Штатный путь остаётся «одно сравнение int +
		// двоичный поиск». В АВАРИИ (метка потеряна у всех наших отрезков) вызов достаётся каждой
		// паре «энтити × получатель»: при потолке 384 и четырёх лучах это до ~98 тыс. вызовов в
		// движок за одну проверку передачи. Это осознанная цена: авария длится ровно до правки,
		// её видно в логе (beam_marker_lost), и она лучше, чем молча показать луч всем.
		//
		// Расхождение ключей — это уже отказ, и он обязан попасть в лог с машинной причиной:
		// иначе мы узнаем о нём тем самым способом, которого избегаем.
		//
		// Спектатор владельца луча его НЕ видит — как и на прежнем пути: отдельной ветки под
		// наблюдение у !lead нет ни там, ни здесь, поведение совпадает намеренно.
		EntityInstanceByClassIter_t iterLeadBeam(NULL, KZ_LEAD_BEAM_CLASSNAME);
		for (CBaseEntity *beamEnt = static_cast<CBaseEntity *>(iterLeadBeam.First()); beamEnt;
			 beamEnt = static_cast<CBaseEntity *>(iterLeadBeam.Next()))
		{
			bool ours = beamEnt->m_iTeamNum() == KZ_LEAD_SEGMENT_TEAM;
			if (!ours && beamEnt->m_pEntity && beamEnt->m_pEntity->NameMatches(KZ_LEAD_TARGETNAME))
			{
				ours = true;
				LeadBeamMarkerLost(beamEnt->m_iTeamNum());
			}
			if (!ours)
			{
				continue; // луч карты, не наш — не трогаем
			}
			if (targetPlayer->leadService && targetPlayer->leadService->HasOwnedSegments()
				&& targetPlayer->leadService->OwnsSegmentEntity(beamEnt->GetRefEHandle()))
			{
				continue; // свой луч владельцу
			}
			pTransmitInfo->m_pTransmitEdict->Clear(beamEnt->GetEntityIndex().Get());
		}

		// Сущности panorama-худа: каждому получателю оставляем ТОЛЬКО его собственную.
		// Без этого на 20 игроках каждый получает 20 сущностей с чужими таймерами —
		// и трафиком, и мусором на экране.
		EntityInstanceByClassIter_t iterLayout(NULL, "custom_hud_layout");
		for (CBaseEntity *layoutEnt = static_cast<CBaseEntity *>(iterLayout.First()); layoutEnt;
			 layoutEnt = static_cast<CBaseEntity *>(iterLayout.Next()))
		{
			if (targetPlayer->hudService->OwnsLayoutEntity(layoutEnt->GetRefEHandle()))
			{
				continue;
			}
			pTransmitInfo->m_pTransmitEdict->Clear(layoutEnt->GetEntityIndex().Get());
		}

		EntityInstanceByClassIter_t iter(NULL, "player");
		// clang-format off
		for (CCSPlayerPawn *pawn = static_cast<CCSPlayerPawn *>(iter.First());
			 pawn != NULL;
			 pawn = pawn->m_pEntity->m_pNextByClass ? static_cast<CCSPlayerPawn *>(pawn->m_pEntity->m_pNextByClass->m_pInstance) : nullptr)
		// clang-format on
		{
			if (targetPlayerPawn == pawn && targetPlayer->quietService->ShouldHideWeapon())
			{
				auto pVecWeapons = pawn->m_pWeaponServices->m_hMyWeapons();

				FOR_EACH_VEC(*pVecWeapons, i)
				{
					auto pWeapon = (*pVecWeapons)[i].Get();

					if (pWeapon)
					{
						pTransmitInfo->m_pTransmitEdict->Clear(pWeapon->entindex());
					}
				}
			}
			// Bit is not even set, don't bother.
			if (!pTransmitInfo->m_pTransmitEdict->IsBitSet(pawn->entindex()))
			{
				continue;
			}
			// Do not transmit a pawn without any controller to prevent crashes.
			if (!pawn->m_hController().IsValid())
			{
				pTransmitInfo->m_pTransmitEdict->Clear(pawn->entindex());
				continue;
			}
			// Respawn must be enabled or !hide will cause client crash.
#if 0
			// Never send dead players to prevent crashes.
			if (pawn->m_lifeState() != LIFE_ALIVE)
			{
				pTransmitInfo->m_pTransmitEdict->Clear(pawn->entindex());
				continue;
			}
#endif
			KZPlayer *pawnPlayer = g_pKZPlayerManager->ToPlayer(pawn);
			// Finally check if player is using !hide.
			if (!targetPlayer->quietService->ShouldHide())
			{
				continue;
			}
			if (targetPlayer->quietService->ShouldHideIndex(pawnPlayer->index))
			{
				pTransmitInfo->m_pTransmitEdict->Clear(pawn->entindex());
			}
		}
	}
}

static void FilterQuietClients(const uint64 *clients, u32 emitterPlayerIndex = 0)
{
	for (i32 recipientPlayerIndex = 1; recipientPlayerIndex < MAXPLAYERS + 1; recipientPlayerIndex++)
	{
		KZPlayer *recipient = g_pKZPlayerManager->ToPlayer(recipientPlayerIndex);
		if (recipient->quietService->ShouldHide() && (emitterPlayerIndex == 0 || recipient->quietService->ShouldHideIndex(emitterPlayerIndex)))
		{
			*(uint64 *)clients &= ~(1ull << (recipientPlayerIndex - 1));
		}
	}
}

void KZ::quiet::OnPostEvent(INetworkMessageInternal *pEvent, const CNetMessage *pData, const uint64 *clients)
{
	NetMessageInfo_t *info = pEvent->GetNetMessageInfo();

	u32 emitterEntIndex = 0;
	switch (info->m_MessageId)
	{
		case GE_PlaceDecalEvent:
		{
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CMsgPlaceDecalEvent>();
			if (msg->decal_group_name() != KZPaintService::DECAL_GROUP_NAME)
			{
				return;
			}

			u64 currentRecipients = *(const u64 *)clients;

			// Find the player who initiated this paint.
			for (i32 i = 1; i <= MAXPLAYERS; i++)
			{
				KZPlayer *painter = g_pKZPlayerManager->ToPlayer(i);
				if (!painter || !painter->paintService || !painter->paintService->pendingPaint)
				{
					continue;
				}

				u64 paintRecipients = 0;
				for (i32 recipientPlayerIndex = 1; recipientPlayerIndex <= MAXPLAYERS; recipientPlayerIndex++)
				{
					u64 recipientBit = 1ull << (recipientPlayerIndex - 1);
					if ((currentRecipients & recipientBit) == 0)
					{
						continue;
					}

					if (recipientPlayerIndex == i)
					{
						paintRecipients |= recipientBit;
						continue;
					}

					KZPlayer *recipient = g_pKZPlayerManager->ToPlayer(recipientPlayerIndex);
					if (recipient && recipient->paintService && recipient->paintService->ShouldShowAllPaint())
					{
						paintRecipients |= recipientBit;
					}
				}

				*(uint64 *)clients = paintRecipients;

				Color color = painter->paintService->GetColor();
				// Game expects ABGR; internal storage and user input are RGBA.
				u32 colorPacked = ((u32)color.a() << 24) | ((u32)color.b() << 16) | ((u32)color.g() << 8) | (u32)color.r();
				msg->set_color(colorPacked);
				msg->set_size_override(painter->paintService->GetSize());
				return;
			}

			// No painter found — suppress the decal entirely.
			*(uint64 *)clients = 0;
			return;
		}
		// Hide bullet decals, and sound.
		case GE_FireBulletsId:
		{
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CMsgTEFireBullets>();
			emitterEntIndex = msg->player() & 0x3FFF;
			break;
		}
		// Hide reload sounds.
		case CS_UM_WeaponSound:
		{
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CCSUsrMsg_WeaponSound>();
			emitterEntIndex = msg->entidx();
			break;
		}
		// Hide other sounds from player (eg. armor equipping)
		case GE_SosStartSoundEvent:
		{
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CMsgSosStartSoundEvent>();
			emitterEntIndex = msg->source_entity_index();
			break;
		}
		// Used by kz_misc to block valve's player say messages.
		case CS_UM_SayText:
		case UM_SayText:
		{
			if (!KZOptionService::GetOptionInt("overridePlayerChat", true))
			{
				return;
			}
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CUserMessageSayText>();
			i32 index = msg->playerindex();
			if (index == -1)
			{
				return;
			}
			{
				*(uint64 *)clients = 0;
			}
			return;
		}
		case CS_UM_SayText2:
		case UM_SayText2:
		{
			auto msg = const_cast<CNetMessage *>(pData)->ToPB<CUserMessageSayText2>();
			// Диагностика инцидента 10.09 (чат невидим после апдейта CS2 09.09): какие поля несут
			// SayText2, проходящие через PostEventAbstract. Два счётчика: «чужие» (движок и другие
			// плагины: params непустые и без нашего маркера) и «наши». Не в такте — только на
			// чат-сообщениях; для легаси-id 306 поля не печатаем (класса в SDK нет; чтение ниже —
			// унаследованное от апстрима).
			static i32 diagForeignLeft = 20;
			static i32 diagOwnLeft = 5;
			if (info->m_MessageId != UM_SayText2)
			{
				// Легаси-id: поля по нашему классу не читаем вовсе (src неизвестен).
				if (diagForeignLeft > 0)
				{
					diagForeignLeft--;
					Msg("[cyb] saytext2_seen src=? id=%d msg=%s (legacy id, fields not parsed)\n", (int)info->m_MessageId, pEvent->GetUnscopedName());
				}
			}
			else if (diagForeignLeft > 0 || diagOwnLeft > 0)
			{
				const bool isOwn = msg->param3() == KZ_CHAT_OWN_MARKER || (msg->param1().empty() && msg->param2().empty());
				i32 &budget = isOwn ? diagOwnLeft : diagForeignLeft;
				if (budget > 0)
				{
					budget--;
					const auto &unk = msg->unknown_fields();
					char unkBuf[128] = {};
					i32 off = 0;
					for (i32 u = 0; u < unk.field_count() && off < (i32)sizeof(unkBuf) - 24; u++)
					{
						const auto &f = unk.field(u);
						if (f.type() == google::protobuf::UnknownField::TYPE_VARINT)
						{
							off += V_snprintf(unkBuf + off, sizeof(unkBuf) - off, " f%d=%llu", f.number(), (unsigned long long)f.varint());
						}
					}
					Msg("[cyb] saytext2_seen src=%s id=%d msg=%s entidx=%d chat=%d name=\"%s\" p1=\"%s\" p2=\"%s\" unknown=%d%s\n", isOwn ? "own" : "foreign",
						(int)info->m_MessageId, pEvent->GetUnscopedName(), msg->entityindex(), (int)msg->chat(), msg->messagename().c_str(),
						msg->param1().c_str(), msg->param2().c_str(), unk.field_count(), unkBuf);
				}
			}
			if (!KZOptionService::GetOptionInt("overridePlayerChat", true))
			{
				return;
			}
			i32 index = msg->entityindex();
			if (index == -1)
			{
				return;
			}
			// Наше SayText2 в режиме kz_chat_mode 1 тоже несёт params — маркер в param3 отличает
			// его от движкового чата, который и надо глушить (инцидент 10.09.2026).
			if (info->m_MessageId == UM_SayText2 && msg->param3() == KZ_CHAT_OWN_MARKER)
			{
				return;
			}
			if (!msg->mutable_param1()->empty() || !msg->mutable_param2()->empty())
			{
				*(uint64 *)clients = 0;
			}
			return;
		}
		default:
		{
			return;
		}
	}
	CBaseEntity *emitterEnt = static_cast<CBaseEntity *>(GameEntitySystem()->GetEntityInstance(CEntityIndex(emitterEntIndex)));
	if (emitterEntIndex == 0 || !emitterEnt)
	{
		return;
	}

	// Convert this entindex into the index in the player controller.
	if (emitterEnt->IsPawn())
	{
		CBasePlayerPawn *pawn = static_cast<CBasePlayerPawn *>(emitterEnt);
		u32 emitterPlayerIndex = g_pKZPlayerManager->ToPlayer(utils::GetController(pawn))->index;
		FilterQuietClients(clients, emitterPlayerIndex);
	}
	else if (V_strstr(emitterEnt->GetClassname(), "weapon_"))
	{
		// Find the owner of the weapon if possible.
		if (emitterEnt->m_hOwnerEntity().IsValid() && emitterEnt->m_hOwnerEntity.Get()->IsPawn())
		{
			CBasePlayerPawn *ownerPawn = static_cast<CBasePlayerPawn *>(emitterEnt->m_hOwnerEntity().Get());
			u32 emitterPlayerIndex = g_pKZPlayerManager->ToPlayer(ownerPawn)->index;
			FilterQuietClients(clients, emitterPlayerIndex);
		}
		// Otherwise just hide from everyone having !hide enabled.
		else
		{
			FilterQuietClients(clients);
		}
	}
	// Special case for the armor sound upon spawning/respawning, because the emitter player is not yet known.
	else if (V_strcmp(emitterEnt->GetClassname(), "item_assaultsuit") == 0)
	{
		FilterQuietClients(clients);
	}
}

void KZQuietService::Init()
{
	KZOptionService::RegisterEventListener(&optionEventListener);
}

void KZQuietService::Reset()
{
	this->hideOtherPlayers = this->player->optionService->GetPreferenceBool("hideOtherPlayers", false);
	this->hideWeapon = this->player->optionService->GetPreferenceBool("hideWeapon", false);
}

void KZQuietService::SendFullUpdate()
{
	if (CServerSideClient *client = g_pKZUtils->GetClientBySlot(this->player->GetPlayerSlot()))
	{
		client->ForceFullUpdate();
	}
	// Keep the player's angles the same.
	QAngle angles;
	this->player->GetAngles(&angles);
	this->player->SetAngles(angles);
}

bool KZQuietService::ShouldHide()
{
	if (!this->hideOtherPlayers)
	{
		return false;
	}

	// If the player is not alive and not spectating another player, don't hide other players.
	if (!this->player->IsAlive() && !this->player->specService->GetSpectatedPlayer())
	{
		return false;
	}

	return true;
}

bool KZQuietService::ShouldHideIndex(u32 targetIndex)
{
	// Don't self-hide.
	if (this->player->index == targetIndex)
	{
		return false;
	}

	// Don't hide the player being spectated.
	if (this->player->specService->GetSpectatedPlayer() && this->player->specService->GetSpectatedPlayer()->index == targetIndex)
	{
		return false;
	}

	return true;
}

SCMD(kz_hideweapon, SCFL_PLAYER)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	player->quietService->ToggleHideWeapon();
	return MRES_SUPERCEDE;
}

void KZQuietService::ToggleHideWeapon()
{
	this->hideWeapon = !this->hideWeapon;
	this->SendFullUpdate();
	this->player->optionService->SetPreferenceBool("hideWeapon", this->hideWeapon);
	this->player->languageService->PrintChat(true, false,
											 this->hideWeapon ? "Quiet Option - Show Weapon - Disable" : "Quiet Option - Show Weapon - Enable");
	if (!this->hideWeapon)
	{
		this->player->pistolService->UpdatePistol();
	}
}

void KZQuietService::OnPhysicsSimulatePost() {}

void KZQuietService::OnPlayerPreferencesLoaded()
{
	this->hideWeapon = this->player->optionService->GetPreferenceBool("hideWeapon", false);
	if (this->hideWeapon)
	{
		this->SendFullUpdate();
	}
	bool newShouldHide = this->player->optionService->GetPreferenceBool("hideOtherPlayers", false);
	if (!newShouldHide && this->hideOtherPlayers && this->player->IsInGame())
	{
		this->SendFullUpdate();
	}
	this->hideOtherPlayers = newShouldHide;
}

void KZQuietService::ToggleHide()
{
	this->hideOtherPlayers = !this->hideOtherPlayers;
	this->player->optionService->SetPreferenceBool("hideOtherPlayers", this->hideOtherPlayers);
	if (!this->hideOtherPlayers)
	{
		this->SendFullUpdate();
	}
}

void KZQuietService::UpdateHideState()
{
	CPlayer_ObserverServices *obsServices = this->player->GetController()->m_hPawn()->m_pObserverServices;
	if (!obsServices)
	{
		this->lastObserverMode = OBS_MODE_NONE;
		this->lastObserverTarget.Term();
		return;
	}
	// Nuclear option, define this if things crash still!
#if 0
	if (obsServices->m_iObserverMode() != this->lastObserverMode || obsServices->m_hObserverTarget() != this->lastObserverTarget)
	{
		this->SendFullUpdate();
	}
#endif
	this->lastObserverMode = obsServices->m_iObserverMode();
	this->lastObserverTarget = obsServices->m_hObserverTarget();
}
