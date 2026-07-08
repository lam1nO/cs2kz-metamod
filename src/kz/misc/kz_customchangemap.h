#pragma once

namespace KZ::misc::customchangemap
{
	// Регистрирует Steam-колбэк докачки воркшоп-карт. Вызывать один раз при загрузке плагина.
	void Init();
	// Снимает колбэк. Вызывать при выгрузке плагина.
	void Cleanup();
} // namespace KZ::misc::customchangemap
