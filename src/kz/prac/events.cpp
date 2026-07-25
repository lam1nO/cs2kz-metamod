#include "kz_prac.h"
#include "kz/timer/kz_timer.h"

// В prac активного рана нет, но игрок летает по карте и может влететь в стартовую
// зону курса. Запуск таймера в этом состоянии — единственное, что нужно отбить в ядре.
class PracTimerListener : public KZTimerServiceEventListener
{
public:
	virtual bool OnTimerStart(KZPlayer *player, u32 courseGUID) override
	{
		return !player->pracService->IsInPrac();
	}
} pracTimerListener;

void KZPracService::Init()
{
	KZTimerService::RegisterEventListener(&pracTimerListener);
}
