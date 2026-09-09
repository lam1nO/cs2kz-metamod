#include "kz_prac.h"
#include "kz/timer/kz_timer.h"

// В prac активного рана нет, но игрок летает по карте и может влететь в стартовую
// зону курса. Запуск таймера в этом состоянии — единственное, что нужно отбить в ядре.
static_global class PracTimerListener : public KZTimerServiceEventListener
{
public:
	virtual bool OnTimerStart(KZPlayer *player, u32 courseGUID) override
	{
		// Ровно вето и ничего больше: НАСТОЯЩИЙ таймер в prac стартовать не должен ни от чего,
		// потому что на timerRunning висит весь сабмит. Сами зоны сюда уже не доходят —
		// KZTriggerService гасит KZTRIGGER_ZONE_START до таймера и на выходе из зоны вместо
		// TimerStart заводит prac-часы (KZPracService::OnStartZoneEndTouch); это вето — защита
		// от любого другого вызывающего TimerStart.
		return !player->pracService->IsInPrac();
	}
} pracTimerListener;

void KZPracService::Init()
{
	KZTimerService::RegisterEventListener(&pracTimerListener);
	// Сверку раскладки ResponseContext_t здесь НЕ делаем — см. EnsureMapContextsSupportChecked.
	// Инцидент 08.09 (cyb.151): schema::GetOffset("CBaseEntity", …) из Init при Load плагина
	// построил таблицу полей CBaseEntity, пока GameEntitySystem() ещё NULL → IsFieldNetworked дала
	// false ВСЕМ полям, таблица закэшировалась навсегда, Set() перестал звать NetworkStateChanged →
	// клиент не узнавал о смене MoveType/FL_NOCLIP и «застревал» в стенах в ноуклипе. Всему флоту.
}

bool KZPracService::mapContextsChecked = false;

void KZPracService::EnsureMapContextsSupportChecked()
{
	if (KZPracService::mapContextsChecked)
	{
		return;
	}
	// Зовётся из игры (первый !prac): энтити-система уже живая, таблицы схемы строятся с
	// верными флагами networked. Пока её нет — не проверяем и не запоминаем результат.
	if (!GameEntitySystem())
	{
		return;
	}
	KZPracService::mapContextsChecked = true;

	// Сверка раскладки ResponseContext_t с живой схемой: пишем в вектор пешки сырыми структурами,
	// и после апдейта Valve тихий сдвиг поля означал бы порчу памяти. Не сошлось — фича молча
	// выключена (старое поведение), в лог — error: это сломалось у нас.
	int size = 0, fieldCount = 0;
	const bool found = schema::GetClassLayout("ResponseContext_t", size, fieldCount);
	const SchemaKey nameKey = schema::GetOffset("ResponseContext_t", hash_32_fnv1a_const("ResponseContext_t"), "m_iszName",
												hash_32_fnv1a_const("m_iszName"));
	const SchemaKey valueKey = schema::GetOffset("ResponseContext_t", hash_32_fnv1a_const("ResponseContext_t"), "m_iszValue",
												 hash_32_fnv1a_const("m_iszValue"));
	const SchemaKey expKey = schema::GetOffset("ResponseContext_t", hash_32_fnv1a_const("ResponseContext_t"), "m_fExpirationTime",
											   hash_32_fnv1a_const("m_fExpirationTime"));
	const SchemaKey vecKey =
		schema::GetOffset("CBaseEntity", hash_32_fnv1a_const("CBaseEntity"), "m_ResponseContexts", hash_32_fnv1a_const("m_ResponseContexts"));
	// Размер коллекции меняем движковым манипулятором — без него писать некуда.
	const bool hasManipulator = schema::GetCollectionManipulator("CBaseEntity", "m_ResponseContexts") != NULL;
	// Размер и число полей — обязательно: поле, дописанное Valve в хвост, офсетов не сдвинет, а шаг
	// элемента в векторе станет другим. Офсет m_iszName нулевой, поэтому его «не нашли» = 0 —
	// различаем через found/fieldCount.
	KZPracService::mapContextsSupported = found && size == (int)sizeof(ResponseContext_t) && fieldCount == 3
										  && nameKey.offset == offsetof(ResponseContext_t, m_iszName)
										  && valueKey.offset == offsetof(ResponseContext_t, m_iszValue)
										  && expKey.offset == offsetof(ResponseContext_t, m_fExpirationTime) && vecKey.offset != 0 && hasManipulator;
	if (!KZPracService::mapContextsSupported)
	{
		KZ_LOG_ERROR(LogChannel::Timer,
					 "[cyb] prac_map_contexts_disabled reason=schema_mismatch found=%d size=%d fields=%d name_off=%u value_off=%u exp_off=%u "
					 "vec_off=%u manipulator=%d (expected size=%u fields=3 offs=0/%u/%u vec!=0 manipulator=1)\n",
					 (int)found, size, fieldCount, nameKey.offset, valueKey.offset, expKey.offset, vecKey.offset, (int)hasManipulator,
					 (u32)sizeof(ResponseContext_t), (u32)offsetof(ResponseContext_t, m_iszValue),
					 (u32)offsetof(ResponseContext_t, m_fExpirationTime));
	}
}
