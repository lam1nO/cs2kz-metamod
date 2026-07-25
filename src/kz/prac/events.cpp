#include "kz_prac.h"
#include "kz/timer/kz_timer.h"

// В prac активного рана нет, но игрок летает по карте и может влететь в стартовую
// зону курса. Запуск таймера в этом состоянии — единственное, что нужно отбить в ядре.
static_global class PracTimerListener : public KZTimerServiceEventListener
{
public:
	virtual bool OnTimerStart(KZPlayer *player, u32 courseGUID) override
	{
		if (!player->pracService->IsInPrac())
		{
			return true;
		}
		// Настоящий таймер не пускаем, но для prac это «свежая попытка»: prac-часы стартуют
		// ровно здесь — все гарды TimerStart уже пройдены, то есть это тот самый тик, на
		// котором пошёл бы ран.
		player->pracService->OnTimerStartBlocked();
		return false;
	}
} pracTimerListener;

void KZPracService::Init()
{
	KZTimerService::RegisterEventListener(&pracTimerListener);
}
