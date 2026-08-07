#include "kz_prac.h"
#include "kz/timer/kz_timer.h"

// В prac активного рана нет, но игрок летает по карте и может влететь в стартовую
// зону курса. Запуск таймера в этом состоянии — единственное, что нужно отбить в ядре.
static_global class PracTimerListener : public KZTimerServiceEventListener
{
public:
	virtual bool OnTimerStart(KZPlayer *player, u32 courseGUID) override
	{
		// Ровно вето и ничего больше (решение пользователя 07.08): стартовая зона в prac — не
		// событие, prac-часы она не заводит и не сбрасывает. Сами зоны сюда уже не доходят —
		// KZTriggerService гасит KZTRIGGER_ZONE_START до таймера; это защита от любого другого
		// вызывающего TimerStart, потому что на timerRunning висит весь сабмит.
		return !player->pracService->IsInPrac();
	}
} pracTimerListener;

void KZPracService::Init()
{
	KZTimerService::RegisterEventListener(&pracTimerListener);
}
