#pragma once
#include "kz/kz.h"

// Классы, которые понимает panorama-разметка чужого аддона 3469155349: цвет, шрифт и шаг
// позиций заданы СГЕНЕРИРОВАННЫМИ листами (palette/fonts/positions.vcss). Произвольного
// значения тут нет — любое число и любой цвет обязаны сойтись к существующему классу,
// иначе элемент просто не покрасится, и это будет молчаливый отказ.
namespace panorama
{
	// ResolveColorClass — класс ТЕКСТОВОГО цвета (`color:`, pal-fg-N/grad-N),
	// ResolveSwatchClass — класс ФОНА (`background-color:`, pal-bg-N/gbg-N). Пустой <Panel> без
	// текста (плечи крестика, свотчи палитры) красится только фоном: pal-fg-N на нём молча ничего
	// не рисует.
	const char *ResolveColorClass(const Color &color);
	const char *ResolveSwatchClass(const Color &color);
	const char *ResolveFontClass(const char *name, const char *fallback);
	const char *ResolveFontSlug(const char *name, const char *fallback);
	const char *GetFontDisplayName(const char *name, const char *fallback);
	// Обход таблицы шрифтов по индексу — попап списка «Шрифт» в меню настроек (layout/menu.cpp).
	i32 GetFontCount();
	const char *GetFontSlugAt(i32 index);
	// Таблица упорядочена по семействам, и попап шрифтов листается ИМЕННО семействами
	// (layout/menu.cpp): family — заголовок страницы, variant — подпись строки, className —
	// начертание, которым строка нарисована. Звёздочка в имени семейства («Lato*») означает
	// «шрифт берётся из системных шрифтов игрока» — сноска попапа объясняет её.
	const char *GetFontFamilyAt(i32 index);
	const char *GetFontVariantAt(i32 index);
	const char *GetFontClassAt(i32 index);
	// Позиции: шаг 1 внутри +-100, шаг 5 до +-500. Размеры: шаг 1 до 100, дальше 5.
	i32 SnapToStep(i32 value, i32 lo, i32 hi);
	i32 GetColorEntryCount();
	// Сколько записей палитры сплошные: записи [0, GetSolidColorCount) — сплошной цвет,
	// дальше градиенты. Нужно потребителям, у которых градиента нет в разметке (key-glow-N).
	i32 GetSolidColorCount();
	const char *GetColorEntryBgClass(i32 entry);
	Color GetColorEntryValue(i32 entry);
	i32 FindColorEntry(const Color &color);
} // namespace panorama
