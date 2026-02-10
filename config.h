/* See LICENSE file for copyright and license details. */
/* Default settings; can be overriden by command line. */

static int topbar = 1;                      /* -b  option; if 0, dmenu appears at bottom     */
static const unsigned int alpha = 0x88;     /* Amount of opacity. 0xff is opaque             */
/* -fn option overrides fonts[0]; default X11 font or font set */
static const char *fonts[] = {
	"monospace:size=14"
};
static const char *prompt      = NULL;      /* -p  option; prompt to the left of input field */

/* Цветовая схема, согласованная с dwm */
static const char col_dark0[]      = "#051b35";   /* Глубокий синий */
static const char col_dark1[]      = "#0e4466";   /* Тёмный сине-серый */
static const char col_dark2[]      = "#1d5e72";   /* Серо-синий */
static const char col_dark3[]      = "#246f8b";   /* Умеренный синий */
static const char col_dark4[]      = "#308fad";   /* Светлый синий */
static const char col_light0[]     = "#d6b8a1";   /* Светло-бежевый */
static const char col_light1[]     = "#bb794a";   /* Коричневый */
static const char col_light2[]     = "#a34c65";   /* Приглушённый розовый */
static const char col_light3[]     = "#644862";   /* Тёмный фиолетово-коричневый */
static const char col_light4[]     = "#53b0bc";   /* Бирюзовый */

/* Схемы цветов для dmenu */
static const char *colors[SchemeLast][2] = {
	/*     fg (текст)         bg (фон)      */
	[SchemeNorm] = { col_light0, col_dark0 },     /* обычный элемент */
	[SchemeSel] = { "#ffffff", col_dark2 },       /* выбранный элемент (фон как активный тег в dwm) */
	[SchemeOut] = { col_light4, col_dark0 },      /* выходной элемент */
};

static const unsigned int alphas[SchemeLast][2] = {
	[SchemeNorm] = { OPAQUE, alpha },
	[SchemeSel] = { OPAQUE, alpha },
	[SchemeOut] = { OPAQUE, alpha },
};

/* -l option; if nonzero, dmenu uses vertical list with given number of lines */
static unsigned int lines      = 10;        /* Увеличено для лучшего визуала */

/*
 * Characters not considered part of a word while deleting words
 * for example: " /?\"&[]"
 */
static const char worddelimiters[] = " ";
