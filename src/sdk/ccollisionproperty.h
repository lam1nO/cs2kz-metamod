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

	// Границы ВИДИМОСТИ, а не коллизии. Отбор «что вообще рисовать/слать» идёт не по
	// m_vecMins/m_vecMaxs, а по surrounding-боксу: m_nSurroundType говорит, откуда его брать,
	// m_vecSpecifiedSurrounding* — бокс В ЛОКАЛЬНЫХ координатах (относительно origin),
	// m_vecSurrounding* — уже посчитанный МИРОВОЙ. У свежесозданной энтити все они нули, то
	// есть бокс вырожден в точку в origin — для луча, у которого origin лишь один конец
	// отрезка, это и есть «отрезок в кадре, а рисовать его не будут».
	// Имена и ширины сверены с живой схемой CS2 (SteamTracking/GameTracking-CS2,
	// DumpSource2/schemas/server/CCollisionProperty.h); m_nSurroundType объявлен там как
	// SurroundingBoundsType_t : uint8_t, поэтому здесь uint8 — самого enum'а в hl2sdk нет.
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
