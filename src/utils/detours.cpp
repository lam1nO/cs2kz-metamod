#include "common.h"
#include "utils.h"
#include "cdetour.h"
#include "detours.h"
#include "kz/kz.h"
#include "sdk/entity/ccsplayerpawn.h"
#include "movement/movement.h"
#include "fmtstr.h"
#include "tier0/memdbgon.h"
#include "sdk/physicsgamesystem.h"

CUtlVector<CDetourBase *> g_vecDetours;
extern CGameConfig *g_pGameConfig;

DECLARE_DETOUR(RecvServerBrowserPacket, Detour_RecvServerBrowserPacket);
// Unused
DECLARE_DETOUR(TraceShape, Detour_TraceShape);
DECLARE_DETOUR(CPhysicsGameSystemFrameBoundary, Detour_CPhysicsGameSystemFrameBoundary);

DECLARE_MOVEMENT_DETOUR(PhysicsSimulate);
DECLARE_MOVEMENT_DETOUR(ProcessUsercmds);
DECLARE_MOVEMENT_DETOUR(SetupMove);
DECLARE_MOVEMENT_DETOUR(ProcessMovement);
DECLARE_MOVEMENT_DETOUR(PlayerMove);
DECLARE_MOVEMENT_DETOUR(CheckParameters);
// CanMove и MoveInit СНОВА УСТАНАВЛИВАЮТСЯ (23.09.2026, см. movement::InitDetours).
// Комментарий о том, что они выключены, устарел и был снят: он относился к периоду, когда
// их сигнатуры не резолвились на билде 25470087.
DECLARE_MOVEMENT_DETOUR(CanMove);
DECLARE_MOVEMENT_DETOUR(FullWalkMove);
DECLARE_MOVEMENT_DETOUR(MoveInit);
DECLARE_MOVEMENT_DETOUR(CheckWater);
DECLARE_MOVEMENT_DETOUR(WaterMove);
DECLARE_MOVEMENT_DETOUR(CheckVelocity);
DECLARE_MOVEMENT_DETOUR(Duck);
DECLARE_MOVEMENT_DETOUR(CanUnduck);
DECLARE_MOVEMENT_DETOUR(LadderMove);
DECLARE_MOVEMENT_DETOUR(CheckJumpButtonLegacy);
DECLARE_MOVEMENT_DETOUR(CheckJumpButtonModern);
DECLARE_MOVEMENT_DETOUR(OnJumpLegacy);
DECLARE_MOVEMENT_DETOUR(OnJumpModern);
DECLARE_MOVEMENT_DETOUR(AirMove);
DECLARE_MOVEMENT_DETOUR(AirAccelerate);
DECLARE_MOVEMENT_DETOUR(Friction);
DECLARE_MOVEMENT_DETOUR(WalkMove);
DECLARE_MOVEMENT_DETOUR(TryPlayerMove);
DECLARE_MOVEMENT_DETOUR(CategorizePosition);
DECLARE_MOVEMENT_DETOUR(CheckFalling);
DECLARE_MOVEMENT_DETOUR(PostThink);

// Те же две фазы и тот же принцип «всё-или-ничего», что и в movement::InitDetours.
// 23.09.2026 я закрыл этим только детуры движения, а здешние так и остались на INIT_DETOUR,
// который результат не проверяет вовсе: не разрешись сигнатура — плагин грузился дальше, а
// фича молча пропадала. Апстрим пришёл к тому же выводу в 2a81023 «Make sure to stop loading
// if signatures fail»; таблицу оттуда не переносим — у нас уже есть макросы с проверкой, и
// повторять их чужой формой значило бы трогать сильно разошедшийся файл ради стиля.
bool InitDetours()
{
	g_vecDetours.RemoveAll();
	bool ok = true;

	// Фаза 1 — только резолв и подготовка трамплина, игра не тронута.
	CREATE_DETOUR(g_pGameConfig, RecvServerBrowserPacket, ok);
	CREATE_DETOUR(g_pGameConfig, CPhysicsGameSystemFrameBoundary, ok);
#ifdef DEBUG_TPM
	CREATE_DETOUR(g_pGameConfig, TraceShape, ok);
#endif
	if (!ok)
	{
		FlushAllDetours();
		return false;
	}

	// Фаза 2 — установка.
	ENABLE_DETOUR(RecvServerBrowserPacket, ok);
	ENABLE_DETOUR(CPhysicsGameSystemFrameBoundary, ok);
#ifdef DEBUG_TPM
	ENABLE_DETOUR(TraceShape, ok);
	TraceShape.DisableDetour();
#endif
	if (!ok)
	{
		// Часть трамплинов уже стоит — снимаем здесь, чтобы вызывающему было нечего
		// доворачивать (Load(), вернувший false, Unload() не получит).
		FlushAllDetours();
		return false;
	}
	return true;
}

void FlushAllDetours()
{
	FOR_EACH_VEC(g_vecDetours, i)
	{
		g_vecDetours[i]->FreeDetour();
	}

	g_vecDetours.RemoveAll();
}

int FASTCALL Detour_RecvServerBrowserPacket(RecvPktInfo_t &info, void *pSock)
{
	int retValue = RecvServerBrowserPacket(info, pSock);
	// KZ_LOG_DEBUG(LogChannel::Movement, "Detour_RecvServerBrowserPacket: Message received from %i.%i.%i.%i:%i, returning %i\nPayload: %s\n",
	// 	info.m_adrFrom.m_IPv4Bytes.b1, info.m_adrFrom.m_IPv4Bytes.b2, info.m_adrFrom.m_IPv4Bytes.b3, info.m_adrFrom.m_IPv4Bytes.b4,
	// 	info.m_adrFrom.m_usPort, retValue, (char*)info.m_pPkt);
	return retValue;
}

bool Detour_TraceShape(const void *physicsQuery, const Ray_t &ray, const Vector &start, const Vector &end, const CTraceFilter *pTraceFilter,
					   trace_t *pm)
{
	bool ret = TraceShape(physicsQuery, ray, start, end, pTraceFilter, pm);
#ifdef DEBUG_TPM
	f32 error;
	Vector velocity;
	for (u32 i = 0; i < 2; i++)
	{
		if (g_pKZPlayerManager->ToPlayer(i) && g_pKZPlayerManager->ToPlayer(i)->GetMoveServices())
		{
			error = g_pKZPlayerManager->ToPlayer(i)->GetMoveServices()->m_flAccumulatedJumpError();
			velocity = g_pKZPlayerManager->ToPlayer(i)->currentMoveData->m_vecVelocity;
			break;
		}
	}
	traceHistory.AddToTail({start, end, ray, pm->DidHit(), pm->m_vStartPos, pm->m_vEndPos, pm->m_vHitNormal, pm->m_vHitPoint, pm->m_flHitOffset,
							pm->m_flFraction, error, velocity});
	return ret;
	KZ_LOG_DEBUG(LogChannel::Movement, "Trace %s -> %s, ", VecToString(start), VecToString(end));
	switch (ray.m_eType)
	{
		case RAY_TYPE_LINE:
		{
			KZ_LOG_DEBUG(LogChannel::Movement, "RAY_TYPE_LINE offset %s radius %f, ", VecToString(ray.m_Line.m_vStartOffset), ray.m_Line.m_flRadius);
			break;
		}
		case RAY_TYPE_SPHERE:
		{
			KZ_LOG_DEBUG(LogChannel::Movement, "RAY_TYPE_SPHERE radius %f, center %s, ", ray.m_Sphere.m_flRadius,
						 VecToString(ray.m_Sphere.m_vCenter));
			break;
		}
		case RAY_TYPE_HULL:
		{
			KZ_LOG_DEBUG(LogChannel::Movement, "RAY_TYPE_HULL mins = %s, maxs = %s, ", VecToString(ray.m_Hull.m_vMins),
						 VecToString(ray.m_Hull.m_vMaxs));
			break;
		}
		case RAY_TYPE_CAPSULE:
		{
			KZ_LOG_DEBUG(LogChannel::Movement, "RAY_TYPE_CAPSULE radius %f, center %s %s, ", ray.m_Capsule.m_flRadius,
						 VecToString(ray.m_Capsule.m_vCenter[0]), VecToString(ray.m_Capsule.m_vCenter[1]));
			break;
		}
		case RAY_TYPE_MESH:
		{
			KZ_LOG_DEBUG(LogChannel::Movement, "RAY_TYPE_MESH mins = %s, maxs = %s, numVertice = %i, pVertices = %p, ",
						 VecToString(ray.m_Mesh.m_vMins), VecToString(ray.m_Mesh.m_vMaxs), ray.m_Mesh.m_nNumVertices, ray.m_Mesh.m_pVertices);
			break;
		}
	}
	if (pm->DidHit())
	{
		KZ_LOG_DEBUG(LogChannel::Movement, "hit %s (normal %s, triangle %i, body %p, shape %p)\n", VecToString(pm->m_vEndPos),
					 VecToString(pm->m_vHitNormal), pm->m_nTriangle, pm->m_hBody, pm->m_hShape);
	}
	else
	{
		KZ_LOG_DEBUG(LogChannel::Movement, "missed\n");
	}
#endif
	return ret;
}

void Detour_CPhysicsGameSystemFrameBoundary(void *pThis)
{
	CPhysicsGameSystemFrameBoundary(pThis);
	KZ::misc::OnPhysicsGameSystemFrameBoundary(pThis);
}
