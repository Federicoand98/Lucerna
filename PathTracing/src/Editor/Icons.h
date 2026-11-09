#pragma once

#ifndef EDITOR_ICONS_H
#define EDITOR_ICONS_H

// Le icone della UI (redesign, concept A): glifi del font Lucide (res/fonts/lucide.ttf, ISC), fusi
// nei font di testo da LoadFonts, quindi si scrivono dentro le stringhe: ICON_SUN " Sun".
// Generato dai codepoint di lucide-static 0.460.0 (font/info.json); per un'icona nuova si aggiunge
// una riga col suo codepoint e la si mette nell'intervallo caricato (ICON_MIN..ICON_MAX).

#define ICON_MIN 0xe000
#define ICON_MAX 0xe6ff

#define ICON_SUN            "\xee\x85\xbc" // sun
#define ICON_SKY            "\xee\x88\x96" // cloud-sun
#define ICON_POINT_LIGHT    "\xee\x87\x82" // lightbulb
#define ICON_SPOT_LIGHT     "\xee\x8b\x99" // lamp-ceiling
#define ICON_MESH           "\xee\x81\xa5" // box
#define ICON_BOX            "\xee\x94\xa9" // cuboid
#define ICON_SPHERE         "\xee\x81\xba" // circle
#define ICON_QUAD           "\xee\x85\xab" // square
#define ICON_CAMERA         "\xee\x86\xa5" // video
#define ICON_MATERIAL       "\xee\x87\x9d" // palette
#define ICON_WORLD          "\xee\x83\xab" // globe
#define ICON_RENDER         "\xee\x81\x84" // aperture
#define ICON_FILM           "\xee\x83\x94" // film
#define ICON_OBJECT         "\xee\x92\xb8" // shapes
#define ICON_MOVE           "\xee\x84\xa5" // move
#define ICON_ROTATE         "\xee\x8b\xaa" // rotate-3d
#define ICON_SCALE          "\xee\x8b\xac" // scaling
#define ICON_VIEW           "\xee\x82\xbe" // eye
#define ICON_DENOISE        "\xee\x90\x97" // sparkles
#define ICON_CHEVRON_RIGHT  "\xee\x81\xb3" // chevron-right
#define ICON_CHEVRON_DOWN   "\xee\x81\xb1" // chevron-down
#define ICON_ADD            "\xee\x85\x81" // plus
#define ICON_DELETE         "\xee\x86\x8e" // trash-2
#define ICON_OPEN           "\xee\x89\x87" // folder-open
#define ICON_SAVE           "\xee\x85\x91" // save
#define ICON_NEW            "\xee\x83\x8d" // file-plus
#define ICON_SCREENSHOT     "\xee\x83\xb9" // image
#define ICON_SETTINGS       "\xee\x85\x98" // settings
#define ICON_HELP           "\xee\x82\x82" // circle-help
#define ICON_KEYBOARD       "\xee\x8a\x84" // keyboard
#define ICON_LAYOUT         "\xee\x91\xb5" // layout-panel-left
#define ICON_PAUSE          "\xee\x84\xb2" // pause
#define ICON_PLAY           "\xee\x85\x80" // play
#define ICON_RESET          "\xee\x85\x8c" // rotate-ccw
#define ICON_DEBUG          "\xee\x88\x8c" // bug
#define ICON_CLOSE          "\xee\x86\xb2" // x
#define ICON_CHECK          "\xee\x81\xb0" // check
#define ICON_TEXTURE        "\xee\x84\x84" // layers
#define ICON_MONITOR        "\xee\x84\xa1" // monitor
#define ICON_MINIMIZE       "\xee\x84\xa0" // minus
#define ICON_MAXIMIZE       "\xee\x85\xab" // square
#define ICON_RESTORE        "\xee\x82\xa2" // copy
#define ICON_PROFILER       "\xee\x8a\xa5" // chart-line
#define ICON_ZAP            "\xee\x86\xb4" // zap
#define ICON_EXTERNAL       "\xee\x82\xbd" // external-link
#define ICON_LINK           "\xee\x84\x88" // link
#define ICON_UNLINK         "\xee\x86\x9c" // unlink
#define ICON_REFRESH        "\xee\x85\x89" // refresh-cw
#define ICON_FOLDER         "\xee\x83\x9c" // folder

#endif // EDITOR_ICONS_H
