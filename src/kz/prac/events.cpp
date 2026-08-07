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
}
