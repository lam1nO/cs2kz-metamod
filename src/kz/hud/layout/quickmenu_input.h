#pragma once
#include <cstdint>

namespace KZ::quickmenu
{
	// На переходе UI очищаем события, но сохраняем raw-held: зажатая E не должна
	// стать новым нажатием в следующем режиме. Чужой владелец ввода также съедает
	// накопленные события, при этом отпускания продолжают отслеживаться.
	struct InputLatch
	{
		std::uint64_t held = 0;
		std::uint64_t pending = 0;

		void Observe(std::uint64_t buttons, std::uint64_t navigation, bool otherOwner)
		{
			const std::uint64_t edge = buttons & ~held;
			held = buttons;
			pending = otherOwner ? 0 : pending | (edge & navigation);
		}

		void Transition()
		{
			pending = 0;
		}

		std::uint64_t Take()
		{
			const std::uint64_t result = pending;
			pending = 0;
			return result;
		}
	};
}
