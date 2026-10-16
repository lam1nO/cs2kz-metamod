#pragma once

namespace KZ::misc::customchangemap
{
	// Регистрирует Steam-колбэк и таймер заявок смены карты. Вызывать один раз при загрузке
	// плагина; late — поздняя загрузка (meta load), тогда стартовую карту не добиваем.
	void Init(bool late);
	// Ставит хуки ISteamUGC «карта из кэша ноды = установлена». Вызывать, когда Steam API
	// игрового сервера поднят (до этого SteamUGC() пуст); повторный вызов ничего не делает.
	void OnSteamAPIActivated();
	// Снимает колбэк и хуки. Вызывать при выгрузке плагина.
	void Cleanup();
} // namespace KZ::misc::customchangemap
