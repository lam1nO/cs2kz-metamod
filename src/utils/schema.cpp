#include "common.h"

#include "schema.h"
#include "schemasystem/schemasystem.h"
#include "utils/interfaces.h"
// #include <unordered_map>
#include "tier1/utlmap.h"
#include "plat.h"
#include "sdk/entity/cbaseentity.h"
#include "entity2/entityclass.h"

#include "tier0/memdbgon.h"
using SchemaKeyValueMap_t = std::map<uint32_t, SchemaKey>;
using SchemaTableMap_t = std::map<uint32_t, SchemaKeyValueMap_t>;

static constexpr uint32_t g_ChainKey = hash_32_fnv1a_const("__m_pChainEntity");


// Признак «поле сетевое» берём из МЕТАДАННЫХ СХЕМЫ (MNetworkEnable), а не из базы сетевого
// сериализатора. Причина — инцидент 23.09.2026 (билд CS2 25470087): прежний путь ходил в
// CNetworkSerializerCodeGenDatabase::m_ClassInfos, чья раскладка в hl2sdk с этим билдом
// разъехалась, и обход массива читал имя класса по случайному адресу → SIGSEGV уже после
// загрузки карты (ядро: #0 schema::GetOffset, шаг 112 = sizeof(CNetworkSerializerClassInfo)).
// Метаданные лежат в самой записи поля, которую мы и так разбираем, и от раскладки сетевого
// сериализатора не зависят вовсе — то есть этот класс отказов закрыт, а не обойдён.
//
// MNetworkEnable — штатная пометка networked-полей в схеме CS2; ею же пользуются другие
// плагины (CounterStrikeSharp). Гейт по GameEntitySystem() сохранён: до её появления схема
// отдаёт неполные данные, и кэшировать результат нельзя (инцидент cyb.151 — ноуклип).
static bool IsFieldNetworked(const char *cppName, SchemaClassFieldData_t &field)
{
	if (!GameEntitySystem())
	{
		return false;
	}

	const int count = field.m_nStaticMetadataCount;
	SchemaMetadataEntryData_t *pMeta = field.m_pStaticMetadata;
	if (count <= 0 || count > 256 || !pMeta)
	{
		return false;
	}

	for (int i = 0; i < count; ++i)
	{
		const char *pszName = pMeta[i].m_pszName;
		if (pszName && V_strcmp(pszName, "MNetworkEnable") == 0)
		{
			return true;
		}
	}

	return false;
}

// Try to recursively find __m_pChainEntity in base classes
// (e.g. CCSGameRules -> CTeamplayRules -> CMultiplayRules -> CGameRules, in this case it's in CGameRules)
static void InitChainOffset(SchemaClassInfoData_t *pClassInfo, SchemaKeyValueMap_t &keyValueMap)
{
	short fieldsSize = pClassInfo->m_nFieldCount;
	SchemaClassFieldData_t *pFields = pClassInfo->m_pFields;

	for (int i = 0; i < fieldsSize; ++i)
	{
		SchemaClassFieldData_t &field = pFields[i];

		if (hash_32_fnv1a_const(field.m_pszName) != g_ChainKey)
		{
			continue;
		}

		std::pair<uint32_t, SchemaKey> keyValuePair;
		keyValuePair.first = g_ChainKey;
		keyValuePair.second.offset = field.m_nSingleInheritanceOffset;
		keyValuePair.second.networked = IsFieldNetworked(pClassInfo->m_pszName, field);

		keyValueMap.insert(keyValuePair);
		return;
	}

	// Not the base class yet, keep looking
	if (pClassInfo->m_nBaseClassCount)
	{
		return InitChainOffset(pClassInfo->m_pBaseClasses[0].m_pClass, keyValueMap);
	}
}

static void InitSchemaKeyValueMap(SchemaClassInfoData_t *pClassInfo, SchemaKeyValueMap_t &keyValueMap)
{
	short fieldsSize = pClassInfo->m_nFieldCount;
	SchemaClassFieldData_t *pFields = pClassInfo->m_pFields;

	for (int i = 0; i < fieldsSize; ++i)
	{
		SchemaClassFieldData_t &field = pFields[i];

#ifdef _DEBUG
		Msg("%s::%s found at -> 0x%X - %llx\n", pClassInfo->m_pszName, field.m_pszName, field.m_nSingleInheritanceOffset, &field);
#endif

		std::pair<uint32_t, SchemaKey> keyValuePair;
		keyValuePair.first = hash_32_fnv1a_const(field.m_pszName);
		keyValuePair.second.offset = field.m_nSingleInheritanceOffset;
		keyValuePair.second.networked = IsFieldNetworked(pClassInfo->m_pszName, field);

		keyValueMap.insert(keyValuePair);
	}

	// Датамапа (старый datadesc) ОТКЛЮЧЕНА на билде CS2 25470087 (23.09.2026).
	//
	// Она — ДОПОЛНЕНИЕ к полям схемы: добавляет то, чего в схеме нет. Указатель на неё берётся
	// по смещению внутри SchemaClassInfoData_t, и раскладка этой структуры в hl2sdk с этим
	// билдом разъехалась: сюда приезжает мусор, обход читал имя поля по случайному адресу и
	// ронял сервер после загрузки карты (ядро: #0 schema::GetOffset, шаг 112 =
	// sizeof(typedescription_t), чтение fieldName по +8).
	//
	// Проверка правдоподобия адреса НЕ помогает и была снята: мусор попадает в допустимый
	// диапазон пользовательских адресов и всё равно указывает в неотображённую память —
	// проверено живьём, падение повторилось с гардом (cyb.224). Безопасного способа
	// «пощупать» чужой указатель здесь нет, поэтому не читаем вовсе.
	//
	// Цена: поля, которые есть ТОЛЬКО в datadesc и отсутствуют в схеме, перестают находиться.
	// Поля схемы (всё, чем пользуется KZ через SCHEMA_FIELD) разобраны выше и работают.
	// Вернуть: когда hl2sdk обновит SchemaClassInfoData_t под билд — снять этот блок и
	// прогнать смоук на канарейке. Сторож апстрима следит, пункт hl2sdk-network-serializer
	// в scripts/valve-update-watch.json.

	// If this is a child class there might be a parent class with __m_pChainEntity
	if (keyValueMap.find(g_ChainKey) == keyValueMap.end() && pClassInfo->m_nBaseClassCount)
	{
		InitChainOffset(pClassInfo->m_pBaseClasses[0].m_pClass, keyValueMap);
	}
}

static bool InitSchemaFieldsForClass(SchemaTableMap_t &tableMap, const char *className, uint32_t classKey)
{
	// Таблица класса строится один раз и живёт до выгрузки, а флаг networked каждого поля берётся
	// из IsFieldNetworked, которая без энтити-системы честно отвечает false. Построить таблицу до
	// её появления (вызов из Init при Load) — значит навсегда лишить Set() всех полей класса
	// NetworkStateChanged: инцидент cyb.151 — ноуклип «застревал» в стенах у всего флота. Поэтому
	// пока энтити-системы нет, ничего не кэшируем: вызывающий получит {0,0} и повторит позже.
	if (!GameEntitySystem())
	{
		Warning("InitSchemaFieldsForClass(): '%s' requested before the entity system exists - not cached\n", className);
		return false;
	}

	CSchemaSystemTypeScope *pType = g_pSchemaSystem->FindTypeScopeForModule(MODULE_PREFIX "server" MODULE_EXT);

	if (!pType)
	{
		return false;
	}

	SchemaClassInfoData_t *pClassInfo = pType->FindDeclaredClass(className).Get();

	if (!pClassInfo)
	{
		SchemaKeyValueMap_t map;
		tableMap.insert(std::make_pair(classKey, map));

		Warning("InitSchemaFieldsForClass(): '%s' was not found!\n", className);
		return false;
	}

	// Проверка правдоподобия того, что вернул движок. FindDeclaredClass — ВИРТУАЛЬНЫЙ вызов по
	// раскладке интерфейса из hl2sdk; если апдейт Valve сдвинул её, сюда приходит не-NULL мусор,
	// и разбор полей читает имя по случайному адресу — ровно так сервер падал 23.09.2026 на
	// билде 25470087 (ядро: #0 schema::GetOffset, шаг 112 = sizeof(SchemaClassInfoData_t),
	// чтение m_pszName). Дешёвые инварианты: имя непустое, печатное и совпадает с запрошенным,
	// число полей в разумных пределах. Не сошлось — честный отказ и строка в лог с ИМЕНЕМ
	// класса, а не SIGSEGV.
	const char *pszName = pClassInfo->m_pszName;
	const bool sane = pszName && (uintptr_t)pszName > 0x10000 && pClassInfo->m_nFieldCount < 4096
					  && V_strcmp(pszName, className) == 0;
	if (!sane)
	{
		SchemaKeyValueMap_t map;
		tableMap.insert(std::make_pair(classKey, map));
		Warning("InitSchemaFieldsForClass(): движок вернул неправдоподобный класс для '%s' "
				"(поля=%u) — раскладка схемы в hl2sdk не совпадает с билдом игры\n",
				className, (unsigned)pClassInfo->m_nFieldCount);
		return false;
	}

	SchemaKeyValueMap_t &keyValueMap = tableMap.insert(std::make_pair(classKey, SchemaKeyValueMap_t())).first->second;

	InitSchemaKeyValueMap(pClassInfo, keyValueMap);

	return true;
}

static SchemaClassInfoData_t *FindClassInfo(const char *className)
{
	CSchemaSystemTypeScope *pType = g_pSchemaSystem->FindTypeScopeForModule(MODULE_PREFIX "server" MODULE_EXT);
	return pType ? pType->FindDeclaredClass(className).Get() : NULL;
}

SchemaCollectionManipulatorFn_t schema::GetCollectionManipulator(const char *className, const char *fieldName)
{
	SchemaClassInfoData_t *pClassInfo = FindClassInfo(className);

	if (!pClassInfo)
	{
		Warning("schema::GetCollectionManipulator(): '%s' was not found!\n", className);
		return NULL;
	}

	for (int i = 0; i < pClassInfo->m_nFieldCount; ++i)
	{
		SchemaClassFieldData_t &field = pClassInfo->m_pFields[i];

		if (V_strcmp(field.m_pszName, fieldName) != 0)
		{
			continue;
		}

		CSchemaType *pFieldType = field.m_pType;

		if (!pFieldType || pFieldType->m_eTypeCategory != SCHEMA_TYPE_ATOMIC || pFieldType->m_eAtomicCategory != SCHEMA_ATOMIC_COLLECTION_OF_T)
		{
			Warning("schema::GetCollectionManipulator(): '%s::%s' is not a collection!\n", className, fieldName);
			return NULL;
		}

		return static_cast<CSchemaType_Atomic_CollectionOfT *>(pFieldType)->m_pfnManipulator;
	}

	Warning("schema::GetCollectionManipulator(): '%s' was not found in '%s'!\n", fieldName, className);
	return NULL;
}

// [СПАЙК beam-probe] см. schema.h. Размер и имя типа берём у самой схемы, а не угадываем:
// писать в поле, не зная его ширины, значит затирать соседей.
int schema::GetClassFields(const char *className, schema::FieldDesc *out, int maxFields)
{
	if (!out || maxFields <= 0 || !GameEntitySystem())
	{
		return -1;
	}
	SchemaClassInfoData_t *pClassInfo = FindClassInfo(className);
	if (!pClassInfo)
	{
		return -1;
	}
	int count = 0;
	for (int i = 0; i < pClassInfo->m_nFieldCount && count < maxFields; ++i)
	{
		SchemaClassFieldData_t &field = pClassInfo->m_pFields[i];
		int fieldSize = 0;
		uint8 alignment = 0;
		if (field.m_pType)
		{
			field.m_pType->GetSizeAndAlignment(fieldSize, alignment);
		}
		out[count].name = field.m_pszName;
		out[count].typeName = field.m_pType ? field.m_pType->m_sTypeName.Get() : "?";
		out[count].offset = (uint32_t)field.m_nSingleInheritanceOffset;
		out[count].size = fieldSize;
		out[count].networked = IsFieldNetworked(pClassInfo->m_pszName, field);
		count++;
	}
	return count;
}

bool schema::GetClassLayout(const char *className, int &size, int &fieldCount)
{
	size = 0;
	fieldCount = 0;
	SchemaClassInfoData_t *pClassInfo = FindClassInfo(className);
	if (!pClassInfo)
	{
		return false;
	}
	size = pClassInfo->m_nSize;
	fieldCount = pClassInfo->m_nFieldCount;
	return true;
}

int16_t schema::FindChainOffset(const char *className, uint32_t classNameHash)
{
	return schema::GetOffset(className, classNameHash, "__m_pChainEntity", g_ChainKey).offset;
}

SchemaKey schema::GetOffset(const char *className, uint32_t classKey, const char *memberName, uint32_t memberKey)
{
	static SchemaTableMap_t schemaTableMap;

	if (schemaTableMap.find(classKey) == schemaTableMap.end())
	{
		if (InitSchemaFieldsForClass(schemaTableMap, className, classKey))
		{
			return GetOffset(className, classKey, memberName, memberKey);
		}

		return {0, 0};
	}

	SchemaKeyValueMap_t tableMap = schemaTableMap[classKey];

	if (tableMap.find(memberKey) == tableMap.end())
	{
		if (memberKey != g_ChainKey)
		{
			Warning("schema::GetOffset(): '%s' was not found in '%s'!\n", memberName, className);
		}

		return {0, 0};
	}

	return tableMap[memberKey];
}

void NetworkVarStateChanged(uintptr_t pNetworkVar, uint32_t nOffset, uint32 nNetworkStateChangedOffset)
{
	NetworkStateChangedData data(nOffset);
	CALL_VIRTUAL(void, nNetworkStateChangedOffset, (void *)pNetworkVar, &data);
}

void EntityNetworkStateChanged(uintptr_t pEntity, uint nOffset)
{
	NetworkStateChangedData data(nOffset);
	reinterpret_cast<CEntityInstance *>(pEntity)->NetworkStateChanged(NetworkStateChangedData(nOffset));
}

void ChainNetworkStateChanged(uintptr_t pNetworkVarChainer, uint nLocalOffset, int nArrayIndex)
{
	CEntityInstance *pEntity = reinterpret_cast<CNetworkVarChainer *>(pNetworkVarChainer)->GetObject();

	if (pEntity)
	{
		pEntity->NetworkStateChanged(
			NetworkStateChangedData(nLocalOffset, nArrayIndex, reinterpret_cast<CNetworkVarChainer *>(pNetworkVarChainer)->m_PathIndex));
	}
}
