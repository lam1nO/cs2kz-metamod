#pragma once
#include "cbasemodelentity.h"

// Штатная сущность-луч CS2. Класс ЖИВОЙ (доказано пробником kz_beam_probe на канарейке
// 11.09): у энтити `beam` движок отдаёт цепочку `beam` → `CBeam` → `CBaseModelEntity` →
// `CBaseEntity` → `CEntityInstance`, у `env_beam` — та же цепочка через `CEnvBeam`. Дерево
// Source 1 в hl2sdk-cs2 (game/server/EnvBeam.cpp) к этому отношения не имеет и в плагин не
// компилируется — поля здесь объявлены по ЖИВОЙ схеме сервера.
//
// Объявлены ТОЛЬКО те поля, существование и ширину которых пробник подтвердил живьём (все
// сетевые):
//   m_vecEndPos   0xAD0, 12 Б — второй конец луча (объявлен в CBeam);
//   m_fWidth      0xAAC,  4 Б — ширина у начала;
//   m_fEndWidth   0xAB0,  4 Б — ширина у конца;
//   m_fAmplitude  0xABC,  4 Б — «шум» линии (0 = прямая);
//   m_bTurnedOff  0xACC,  1 Б — луч погашен.
// Имён `m_flWidth`/`m_flEndWidth`/`m_flAmplitude` в схеме НЕТ — пробник по ним отдал
// set=missing. Не добавлять «по аналогии с Source 1»: SCHEMA_FIELD на несуществующем поле
// даёт офсет 0, то есть запись в голову объекта.
// Цвет — m_clrRender из CBaseModelEntity (0x850), он уже объявлен в базовом классе.
//
// ЗАПРЕТ: `env_laser` создавать нельзя — он вешает главный поток сервера (проба 11.09,
// канарейку пришлось пересоздавать). Классов лазера здесь нет намеренно.
class CBeam : public CBaseModelEntity
{
public:
	DECLARE_SCHEMA_CLASS_ENTITY(CBeam)

	SCHEMA_FIELD(Vector, m_vecEndPos)
	SCHEMA_FIELD(float32, m_fWidth)
	SCHEMA_FIELD(float32, m_fEndWidth)
	SCHEMA_FIELD(float32, m_fAmplitude)
	SCHEMA_FIELD(bool, m_bTurnedOff)
};
