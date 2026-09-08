// Сущность custom_hud_layout (Task 4): создание/уничтожение на игрока, запись классов
// панелей и dialog-переменных. Перенесено с апстрима (src/kz/hud/layout/entity.cpp),
// расхождения с ним — см. docs/superpowers/sdd/2026-09-08-panorama-hud/base-facts.md (R1/R4).
#include "kz/hud/layout/layout.h"
#include "kz/hud/layout/panorama_tables.h"
#include "sdk/entity/ccscustomhudlayout.h"
#include "entitykeyvalues.h"
#include "utils/utils.h"
#include "cs2kz.h"

#include "tier0/memdbgon.h"

// clang-format off
extern const LayoutElementDef LAYOUT_ELEMENTS[(i32)LayoutElement::Count] =
{
	{"mhud_timer",      "timer",      "hudTimer",      "mhudTimerX",      "mhudTimerY",      "mhudTimerSize",      "mhudTimerFont",      "hudOutline", "mhudTimerOpacity",      LAYOUT_DEF_TIMER_X,      LAYOUT_DEF_TIMER_Y,      LAYOUT_DEF_TIMER_SIZE},
	{"mhud_speed",      "speed",      "hudSpeed",      "mhudSpeedX",      "mhudSpeedY",      "mhudSpeedSize",      "mhudSpeedFont",      "hudOutline", "mhudSpeedOpacity",      LAYOUT_DEF_SPEED_X,      LAYOUT_DEF_SPEED_Y,      LAYOUT_DEF_SPEED_SIZE},
	{"mhud_prespeed",   "prespeed",   "hudPrespeed",   "mhudPrespeedX",   "mhudPrespeedY",   "mhudPrespeedSize",   "mhudPrespeedFont",   "hudOutline", "mhudPrespeedOpacity",   LAYOUT_DEF_PRESPEED_X,   LAYOUT_DEF_PRESPEED_Y,   LAYOUT_DEF_PRESPEED_SIZE},
	{"mhud_keys",       "keys",       "hudKeys",       "mhudKeysX",       "mhudKeysY",       "mhudKeysSize",       "mhudKeysFont",       "hudOutline", "mhudKeysOpacity",       LAYOUT_DEF_KEYS_X,       LAYOUT_DEF_KEYS_Y,       LAYOUT_DEF_KEYS_SIZE},
	{"mhud_checkpoint", "checkpoint", "hudCpTp",       "mhudCheckpointX", "mhudCheckpointY", "mhudCheckpointSize", "mhudCheckpointFont", "hudOutline", "mhudCheckpointOpacity", LAYOUT_DEF_CHECKPOINT_X, LAYOUT_DEF_CHECKPOINT_Y, LAYOUT_DEF_CHECKPOINT_SIZE},
};
// clang-format on

// === Запись классов на сущность (глобальный слой — у нас сущность персональная, но апстримный
// API работает с глобальным layout-состоянием, per-player состояние нам не нужно) =============

void KZHUDService::SetLayoutClass(CCSCustomHudLayout *layout, const char *panelId, const char *&cache, const char *className)
{
	if (cache == className)
	{
		return;
	}
	if (cache)
	{
		layout->SetHasClass(panelId, cache, k_eHudPanelClassStatus_DoesNotHaveClass);
	}
	if (className)
	{
		layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_HasClass);
	}
	cache = className;
}

void KZHUDService::SetLayoutValueClass(CCSCustomHudLayout *layout, const char *panelId, i32 &cache, i32 value, const char *prefix, bool percent)
{
	value = percent ? panorama::SnapToStep(value, -100, 100) : panorama::SnapToStep(value, 0, 500);
	if (cache == value)
	{
		return;
	}
	const char *unit = percent ? "pct" : "px";
	char className[64];
	if (cache != INT_MIN)
	{
		V_snprintf(className, sizeof(className), "%s--%s%i%s", prefix, cache < 0 ? "neg" : "", abs(cache), unit);
		layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_DoesNotHaveClass);
	}
	V_snprintf(className, sizeof(className), "%s--%s%i%s", prefix, value < 0 ? "neg" : "", abs(value), unit);
	layout->SetHasClass(panelId, className, k_eHudPanelClassStatus_HasClass);
	cache = value;
}

void KZHUDService::UpdateLayoutElement(CCSCustomHudLayout *layout, LayoutElement element, bool show, const char *text, const Color &color, bool force)
{
	const LayoutElementDef &def = LAYOUT_ELEMENTS[(i32)element];
	LayoutElementState &state = this->layoutElements[(i32)element];

	// force — сразу после EnsureOwnedLayout(created=true): свежая сущность у схемы уже
	// прибита к дефолтным классам разметки, а наш кэш думает, что ничего слать не надо
	// (совпал с прошлым состоянием прошлой сущности/дефолтом) — форс сбрасывает кэш, чтобы
	// первый кадр реально переслал все классы новой сущности.
	if (force)
	{
		state = LayoutElementState();
	}

	if (state.hidden != !show)
	{
		state.hidden = !show;
		layout->SetHasClass(def.panelId, "hidden", state.hidden ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
	}
	// Скрытый элемент не теряет значения — включить его обратно ничего не стоит.
	if (state.hidden)
	{
		return;
	}

	// NULL значит вызывающему нечего показать сейчас — держим прошлый текст, а не очищаем.
	if (text && state.text != text)
	{
		state.text = text;
		layout->SetDialogVariableString(def.panelId, def.varName, text);
	}

	const MHUDLayoutPrefs::Element &cached = this->GetLayoutPrefs().elements[(i32)element];
	this->SetLayoutValueClass(layout, def.panelId, state.x, cached.x, "x", true);
	this->SetLayoutValueClass(layout, def.panelId, state.y, cached.y, "y", true);
	this->SetLayoutValueClass(layout, def.panelId, state.fontSize, cached.size, "font-size", false);

	const u32 packed = ((u32)color.r() << 24) | ((u32)color.g() << 16) | ((u32)color.b() << 8) | (u32)color.a();
	if (!state.colorComputed || state.lastColorPacked != packed)
	{
		state.colorComputed = true;
		state.lastColorPacked = packed;
		state.colorClassComputed = panorama::ResolveColorClass(color);
	}
	this->SetLayoutClass(layout, def.panelId, state.colorClass, state.colorClassComputed);
	// cached.fontClass — уже РЕЗОЛВЛЕННЫЙ css-класс (RefreshLayoutPrefs, Task 5 зовёт
	// panorama::ResolveFontClass один раз при обновлении префов). Повторный резолв здесь
	// сравнил бы готовый класс со слагом и всегда проигрывал бы fallback'у.
	this->SetLayoutClass(layout, def.panelId, state.fontClass, cached.fontClass);

	const i32 opacity = Clamp(cached.opacity, 0, 100);
	if (state.opacity != opacity)
	{
		char className[32];
		if (state.opacity != INT_MIN)
		{
			V_snprintf(className, sizeof(className), "opacity--%ipct", state.opacity);
			layout->SetHasClass(def.panelId, className, k_eHudPanelClassStatus_DoesNotHaveClass);
		}
		V_snprintf(className, sizeof(className), "opacity--%ipct", opacity);
		layout->SetHasClass(def.panelId, className, k_eHudPanelClassStatus_HasClass);
		state.opacity = opacity;
	}

	// Обводка — общий преф элемента (hudOutline), Task 5 наполняет его из ключа outlineKey.
	if (state.outline != cached.outline)
	{
		state.outline = cached.outline;
		layout->SetHasClass(def.panelId, "outline", cached.outline ? k_eHudPanelClassStatus_HasClass : k_eHudPanelClassStatus_DoesNotHaveClass);
	}
}

// === Сущность на игрока =============================================================

CCSCustomHudLayout *KZHUDService::EnsureOwnedLayout(bool &created)
{
	created = false;
	// Плагин выгружается или panorama-худ недоступен (нет MultiAddonManager/форс-cvar) —
	// новую сущность заводить некому её показать.
	if (g_KZPlugin.unloading || !KZHUDService::IsMHUDAvailable())
	{
		return NULL;
	}
	if (CBaseEntity *cached = this->ownedLayout.Get())
	{
		return (CCSCustomHudLayout *)cached;
	}
	// Сущность персональная (не общая на сервер): у неё свой набор dialog-переменных/классов
	// панелей, и транзит чужим гасится KZ::quiet (OwnsLayoutEntity) — общая сущность заставила
	// бы разных игроков драться за одни и те же значения.
	CCSCustomHudLayout *layout = utils::CreateEntityByName<CCSCustomHudLayout>("custom_hud_layout");
	if (!layout)
	{
		return NULL;
	}
	CEntityKeyValues *pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("layout", KZ_MHUD_LAYOUT);
	// targetname по слоту — для опознания сущности в отладчике.
	char name[32];
	V_snprintf(name, sizeof(name), "kzmhud%i", this->player->GetPlayerSlot().Get());
	pKeyValues->SetString("targetname", name);
	layout->DispatchSpawn(pKeyValues);
	this->ownedLayout = layout;
	created = true;
	return layout;
}

void KZHUDService::DestroyOwnedLayout()
{
	if (CBaseEntity *ent = this->ownedLayout.Get())
	{
		g_pKZUtils->RemoveEntity(ent);
	}
	this->ownedLayout = nullptr;
	// Кэши классов элементов обнуляются ВМЕСТЕ с сущностью: они хранят упрощённое состояние
	// ЭТОЙ сущности (что на ней уже выставлено), а не абстрактные значения игрока — оставить
	// их живыми означало бы отдать следующему владельцу слота (реконнект/новый игрок) чужой
	// кэш, из-за которого UpdateLayoutElement решит, что менять уже нечего.
	for (i32 i = 0; i < (i32)LayoutElement::Count; i++)
	{
		this->layoutElements[i] = LayoutElementState();
	}
	// Кэш классов клавиш (Task 6) — та же ловушка: живёт вместе с сущностью, иначе следующий
	// владелец слота (реконнект/новый игрок) унаследует чужие классы кнопок и клавиши останутся
	// пустыми (кэш решит, что менять уже нечего).
	this->layoutKeys = LayoutKeysState();
	// Кэш классов крестика (Task 10) — та же ловушка: без обнуления следующий владелец слота
	// унаследует xh-* классы прошлой сущности, и ApplyCrosshair решит, что крестик уже
	// выставлен как надо, — крестик молча не появится (тот же баг, что ревью поймало для
	// клавиш в задаче 6).
	this->layoutCrosshair = LayoutCrosshairState();
}

void KZHUDService::LayoutCleanup()
{
	for (i32 i = 0; i < MAXPLAYERS; i++)
	{
		KZPlayer *player = g_pKZPlayerManager->ToPlayer(CPlayerSlot(i));
		if (player && player->hudService)
		{
			player->hudService->DestroyOwnedLayout();
		}
	}
}

bool KZHUDService::OwnsLayoutEntity(CEntityHandle handle)
{
	return this->ownedLayout.IsValid() && this->ownedLayout.ToInt() == handle.ToInt();
}
