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
		std::int32_t lastCommand = 0;

		// ProcessUsercmds содержит и backup-команды, которые наш фильтр уже мог
		// обнулить. Их нельзя принять за отпускание: watermark — как у рекордера.
		// Вызывающий всё равно маскирует повторные команды перед движком.
		void ObserveCommand(std::int32_t command, std::uint64_t buttons, std::uint64_t navigation, bool otherOwner)
		{
			if (otherOwner) pending = 0;
			if (command <= lastCommand) return;
			lastCommand = command;
			Observe(buttons, navigation, otherOwner);
		}

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
