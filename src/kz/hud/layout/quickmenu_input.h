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
		std::uint64_t consumedUntilRelease = 0;
		std::uint64_t releasedThisCommand = 0;
		std::int32_t lastCommand = 0;

		// ProcessUsercmds содержит и backup-команды, которые наш фильтр уже мог
		// обнулить. Их нельзя принять за отпускание: watermark — как у рекордера.
		// Вызывающий всё равно маскирует повторные команды перед движком.
		void ObserveCommand(std::int32_t command, std::uint64_t buttons, std::uint64_t navigation, bool otherOwner)
		{
			if (otherOwner) pending = 0;
			if (command <= lastCommand) return;
			lastCommand = command;
			// Отпускание тоже принадлежит погашенному нажатию: его остальные
			// слова кнопок, subtick и attack-history нельзя пропускать в движок.
			releasedThisCommand = consumedUntilRelease & ~buttons;
			consumedUntilRelease &= buttons;
			Observe(buttons, navigation, otherOwner);
		}

		void Observe(std::uint64_t buttons, std::uint64_t navigation, bool otherOwner)
		{
			const std::uint64_t edge = buttons & ~held;
			held = buttons;
			pending = otherOwner ? 0 : pending | (edge & navigation);
		}

		// Только инструментальные кнопки: движение здесь никогда не захватывается.
		std::uint64_t SuppressionMask(std::uint64_t controls, bool active)
		{
			if (active) consumedUntilRelease |= held & controls;
			return (active ? controls : 0) | ((consumedUntilRelease | releasedThisCommand) & controls);
		}

		void Transition(std::uint64_t controls = 0)
		{
			consumedUntilRelease |= held & controls;
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
