#pragma once
#include "utils/schema.h"

class CBaseModelEntity;

struct VPhysicsCollisionAttribute_t
{
	DECLARE_SCHEMA_CLASS_BASE(VPhysicsCollisionAttribute_t, 1)

	SCHEMA_FIELD(uint8, m_nCollisionGroup)
	SCHEMA_FIELD(uint64, m_nInteractsAs)
	SCHEMA_FIELD(uint64, m_nInteractsWith)
	SCHEMA_FIELD(uint16, m_nHierarchyId)
};

class CCollisionProperty
{
public:
	DECLARE_SCHEMA_CLASS_BASE(CCollisionProperty, 1)

	SCHEMA_FIELD(VPhysicsCollisionAttribute_t, m_collisionAttribute)
	SCHEMA_FIELD(Vector, m_vecMins)
	SCHEMA_FIELD(Vector, m_vecMaxs)
	SCHEMA_FIELD(SolidType_t, m_nSolidType)
	SCHEMA_FIELD(uint8, m_usSolidFlags)
	SCHEMA_FIELD(uint8, m_CollisionGroup)

	// Границы ВИДИМОСТИ (surrounding-бокс), а не коллизии: m_nSurroundType говорит, откуда его
	// брать, m_vecSpecifiedSurrounding* — бокс В ЛОКАЛЬНЫХ координатах, m_vecSurrounding* —
	// посчитанный МИРОВОЙ. Имена и ширины сверены с живой схемой CS2 (SteamTracking,
	// DumpSource2/schemas/server/CCollisionProperty.h); m_nSurroundType там объявлен как
	// SurroundingBoundsType_t : uint8_t, поэтому здесь uint8 — самого enum'а в hl2sdk нет.
	//
	// ДВЕ ЛОВУШКИ, ОБЕ СТОИЛИ КРУГА ПРОВЕРОК НА КАНАРЕЙКЕ 17.09.2026:
	// 1. m_vecSurroundingMins/Maxs НЕ СЕТЕВЫЕ ВООБЩЕ — у них нет MNetworkEnable, их считает
	//    сервер для себя. Клиенту уезжают только m_nSurroundType и m_vecSpecifiedSurrounding*.
	// 2. У сущности-луча (CBeam) не уезжает и это: класс помечен MNetworkNoBase и пересобирает
	//    сетевой набор с нуля, включая поимённо лишь Origin, m_nModelIndex, m_nRenderFX,
	//    m_nRenderMode, m_clrRender и m_hParent. m_Collision в этот список НЕ входит, то есть
	//    границы видимости луча на клиенте считаются клиентом и правкой с сервера не меняются.
	// Для энтити, у которых сетевой набор наследуется штатно (триггеры, зоны), поля рабочие.
	SCHEMA_FIELD(uint8, m_nSurroundType)
	SCHEMA_FIELD(Vector, m_vecSpecifiedSurroundingMins)
	SCHEMA_FIELD(Vector, m_vecSpecifiedSurroundingMaxs)
	SCHEMA_FIELD(Vector, m_vecSurroundingMins)
	SCHEMA_FIELD(Vector, m_vecSurroundingMaxs)

	CBaseModelEntity *GetOuter()
	{
		return (CBaseModelEntity *)((char *)this + 8);
	}
};
