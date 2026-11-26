#include "kz_beam.h"
#include "player_trail_appearance.h"
#include "kz/spec/kz_spec.h"
#include "kz/noclip/kz_noclip.h"
#include "kz/language/kz_language.h"
#include "kz/option/kz_option.h"

#include "utils/simplecmds.h"
#include "utils/gamesystem.h"
#include "sdk/entity/cparticlesystem.h"
#include "entitykeyvalues.h"
#include "filesystem.h"

namespace
{
	constexpr char StockTrail[] = "particles/ui/hud/ui_map_def_utility_trail.vpcf";
	constexpr char PersonalTrail[] = "particles/gymstrike/beam_trail.vpcf";
	constexpr char PersonalTrailCompiled[] = "particles/gymstrike/beam_trail.vpcf_c";
	bool personalTrailAvailable = false;

	CParticleSystem *CreateTrail(const Vector &origin, const PlayerTrailAppearance &appearance)
	{
		auto *beam = utils::CreateEntityByName<CParticleSystem>("info_particle_system");
		if (!beam)
		{
			return nullptr;
		}

		const bool personal = personalTrailAvailable && (appearance.scale != 1.f || trailappearance::HasPersonalColor(appearance.color));
		auto *values = new CEntityKeyValues();
		values->SetString("effect_name", personal ? PersonalTrail : StockTrail);
		values->SetVector("origin", origin);
		values->SetBool("start_active", true);
		if (personal)
		{
			values->SetInt("tint_cp", 16);
			values->SetColor("tint_cp_color", appearance.ColorOr(Color(236, 108, 24, 255)));
			values->SetInt("data_cp", 20);
			values->SetVector("data_cp_value", Vector(appearance.scale, 0, 0));
		}
		beam->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
		beam->DispatchSpawn(values);
		beam->m_iTeamNum(CUSTOM_PARTICLE_SYSTEM_TEAM);
		return beam;
	}
}

void KZBeamService::PrecacheTrail(IEntityResourceManifest *manifest)
{
	manifest->AddResource(StockTrail);
	personalTrailAvailable = g_pFullFileSystem && g_pFullFileSystem->FileExists(PersonalTrailCompiled, "GAME");
	if (personalTrailAvailable)
	{
		manifest->AddResource(PersonalTrail);
	}
}

static_global class KZOptionServiceEventListener_Beam : public KZOptionServiceEventListener
{
	virtual void OnPlayerPreferencesLoaded(KZPlayer *player)
	{
		player->beamService->OnPlayerPreferencesLoaded();
	}

	void OnPlayerPreferenceChanged(KZPlayer *player, const char *name) override
	{
		if (!strcmp(name, trailappearance::BeamColor) || !strcmp(name, trailappearance::BeamWidth))
		{
			player->beamService->ClearTrail();
		}
	}
} optionEventListener;

void KZBeamService::OnPlayerPreferencesLoaded()
{
	this->ClearTrail();
	this->SetBeamType(this->player->optionService->GetPreferenceInt("desiredBeamType"));
	this->playerBeamOffset = this->player->optionService->GetPreferenceVector("beamOffset", KZBeamService::defaultOffset);
}

void KZBeamService::Init()
{
	KZOptionService::RegisterEventListener(&optionEventListener);
}

SCMD(kz_beam, SCFL_MISC | SCFL_PREFERENCE | SCFL_HELP)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	u8 newDesiredBeamType = (player->beamService->desiredBeamType + 1) % KZBeamService::BEAM_COUNT;
	if (args->ArgC() >= 2)
	{
		if (KZ_STREQI(args->Arg(1), "off"))
		{
			newDesiredBeamType = KZBeamService::BEAM_NONE;
		}
		else if (KZ_STREQI(args->Arg(1), "feet"))
		{
			newDesiredBeamType = KZBeamService::BEAM_FEET;
		}
		else if (KZ_STREQI(args->Arg(1), "ground"))
		{
			newDesiredBeamType = KZBeamService::BEAM_GROUND;
		}
		else
		{
			player->languageService->PrintChat(true, false, "Beam Command Usage");
			return true;
		}
	}
	player->beamService->SetBeamType(newDesiredBeamType);
	switch (player->beamService->desiredBeamType)
	{
		case KZBeamService::BEAM_NONE:
		{
			player->languageService->PrintChat(true, false, "Beam Changed (None)");
			break;
		}
		case KZBeamService::BEAM_FEET:
		{
			player->languageService->PrintChat(true, false, "Beam Changed (Feet)");
			break;
		}
		case KZBeamService::BEAM_GROUND:
		{
			player->languageService->PrintChat(true, false, "Beam Changed (Ground)");
			break;
		}
	}
	player->optionService->SetPreferenceInt("desiredBeamType", player->beamService->desiredBeamType);
	return true;
}

SCMD(kz_beamoffset, SCFL_MISC | SCFL_PREFERENCE)
{
	KZPlayer *player = g_pKZPlayerManager->ToPlayer(controller);
	if (args->ArgC() < 4 || !utils::IsNumeric(args->Arg(1)) || !utils::IsNumeric(args->Arg(2)) || !utils::IsNumeric(args->Arg(3)))
	{
		player->languageService->PrintChat(true, false, "Beam Offset Command Usage");
		player->languageService->PrintChat(true, false, "Current Beam Offset", player->beamService->playerBeamOffset.x,
										   player->beamService->playerBeamOffset.y, player->beamService->playerBeamOffset.z);
		return true;
	}
	player->beamService->playerBeamOffset = Vector(atof(args->Arg(1)), atof(args->Arg(2)), atof(args->Arg(3)));

	player->optionService->SetPreferenceVector("beamOffset", player->beamService->playerBeamOffset);
	player->languageService->PrintChat(true, false, "Current Beam Offset", player->beamService->playerBeamOffset.x,
									   player->beamService->playerBeamOffset.y, player->beamService->playerBeamOffset.z);
	return true;
}

void KZBeamService::Update()
{
	KZPlayer *newTarget = this->player->IsAlive() ? this->player : this->player->specService->GetSpectatedPlayer();

	if (this->target != newTarget)
	{
		this->ClearTrail();
		this->target = newTarget;
	}

	// It is possible that the player is in game has no player pawn on game frame loop.
	// This is probably the case for CSTV bots.
	if (!this->player->GetPlayerPawn())
	{
		this->validBeam = false;
	}
	else
	{
		if ((this->player->GetPlayerPawn()->m_fFlags() & FL_ONGROUND && this->player->GetMoveType() == MOVETYPE_WALK)
			|| this->player->GetMoveType() == MOVETYPE_LADDER)
		{
			this->validBeam |= g_pKZUtils->GetServerGlobals()->curtime - this->player->landingTime >= 0.04f;
		}

		if (this->player->noclipService->JustNoclipped() || this->teleportedThisTick)
		{
			this->validBeam = false;
		}
	}
	this->teleportedThisTick = false;
	this->UpdatePlayerBeam();
}

void KZBeamService::UpdatePlayerBeam()
{
	bool shouldDraw = this->desiredBeamType != KZBeamService::BEAM_NONE;

	// clang-format off
	shouldDraw &= this->target && this->target->GetPlayerPawn() && this->target->GetPlayerPawn()->IsAlive()
	 	&& this->target->beamService->validBeam
	 	&& this->target->takeoffTime > 0.0f
	 	&& (!(this->target->GetPlayerPawn()->m_fFlags() & FL_ONGROUND) || g_pKZUtils->GetServerGlobals()->curtime - this->target->landingTime < 0.04f);
	// clang-format on
	if (!shouldDraw)
	{
		this->ClearTrail();
		return;
	}
	Vector origin = this->target->moveDataPre.m_vecAbsOrigin;

	if (this->desiredBeamType == KZBeamService::BEAM_GROUND)
	{
		origin.z = this->target->takeoffOrigin.z;
	}
	origin += this->playerBeamOffset;
	auto *beam = static_cast<CParticleSystem *>(this->playerBeam.Get());
	if (!beam)
	{
		const auto appearance = ReadPlayerTrailAppearance(*this->player->optionService, trailappearance::BeamColor, trailappearance::BeamWidth);
		beam = CreateTrail(origin, appearance);
		if (!beam)
		{
			return;
		}
		this->playerBeam = beam->GetRefEHandle();
	}
	beam->Teleport(&origin, nullptr, &vec3_origin);

	// Поддерживаем непрерывность, пока следующий эмиттер набирает хвост.
	auto *nextBeam = static_cast<CParticleSystem *>(this->playerBeamNew.Get());
	const float age = g_pKZUtils->GetServerGlobals()->curtime - beam->m_flStartTime().GetTime();
	if (nextBeam)
	{
		nextBeam->Teleport(&origin, nullptr, &vec3_origin);
		if (age > 3.2f)
		{
			g_pKZUtils->RemoveEntity(beam);
			this->playerBeam = this->playerBeamNew;
			this->playerBeamNew = {};
		}
	}
	else if (age > 3.0f)
	{
		const auto appearance = ReadPlayerTrailAppearance(*this->player->optionService, trailappearance::BeamColor, trailappearance::BeamWidth);
		if (auto *next = CreateTrail(origin, appearance))
		{
			this->playerBeamNew = next->GetRefEHandle();
		}
	}
}

void KZBeamService::ClearTrail()
{
	if (GameEntitySystem() && g_pKZUtils)
	{
		if (auto *beam = this->playerBeam.Get())
		{
			g_pKZUtils->RemoveEntity(beam);
		}
		if (auto *nextBeam = this->playerBeamNew.Get())
		{
			g_pKZUtils->RemoveEntity(nextBeam);
		}
	}
	this->playerBeam = {};
	this->playerBeamNew = {};
}

void KZBeamService::Reset()
{
	this->ClearTrail();
	this->playerBeamOffset = KZBeamService::defaultOffset;
	this->target = {};
	this->validBeam = {};
	this->desiredBeamType = {};
	this->teleportedThisTick = false;
}

void KZBeamService::UpdateBeams()
{
	for (i32 i = 0; i < MAXPLAYERS + 1; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(i);
		if (!player || !player->IsInGame())
		{
			continue;
		}
		player->beamService->Update();
	}
}
