# -*- coding: utf-8 -*-
"""Mejora v2: fila "Velocidad del CPU de la PS2" en el menú L3+R3.
Cambia EECycleRate al momento y lo guarda en /data/ForjaSX2/gamesettings/<SERIAL>_<CRC>.ini.
Uso: python mejoras_v2_menu_ee.py <carpeta del repo emulador>"""
import pathlib
import sys

raiz = pathlib.Path(sys.argv[1])


def rep(rel, pares):
    """Reemplazo exacto; se detiene si un texto no aparece exactamente una vez."""
    f = raiz / rel
    t = f.read_text(encoding="utf-8")
    for a, b in pares:
        n = t.count(a)
        if n != 1:
            raise SystemExit(f"{rel}: esperaba 1 vez y encontré {n}: {a[:70]!r}")
        t = t.replace(a, b)
    f.write_text(t, encoding="utf-8", newline="\n")
    print("ok", rel)


# ---------- InGameMenuState.h: 7 filas y el valor del EE empaquetado en los bits 21-23 ----------
rep("ps4/vm/InGameMenuState.h", [
    ("\t\tbool show_overlay;\n\t};",
     "\t\tbool show_overlay;\n\t\tint ee_rate; // Forja SX2: EECycleRate, -3..3\n\t};"),
    ("return (std::min(value.selected, 5u) + 1u) |",
     "return (std::min(value.selected, 6u) + 1u) |"),
    ("\t\t\t(value.show_overlay ? (1u << 20) : 0u);",
     "\t\t\t(value.show_overlay ? (1u << 20) : 0u) |\n"
     "\t\t\t(static_cast<unsigned>(std::clamp(value.ee_rate, -3, 3) + 3) << 21);"),
    ("return {selection != 0, selection ? std::min(selection - 1u, 5u) : 0u,",
     "return {selection != 0, selection ? std::min(selection - 1u, 6u) : 0u,"),
    ("std::min((value >> 16) & 0x0fu, 4u), (value & (1u << 20)) != 0};",
     "std::min((value >> 16) & 0x0fu, 4u), (value & (1u << 20)) != 0,\n"
     "\t\t\tstatic_cast<int>(std::min((value >> 21) & 0x07u, 6u)) - 3};"),
])

# ---------- GameMenu.h / GameMenu.cpp: firma y dibujo de 7 filas ----------
rep("ps4/vm/GameMenu.h", [
    ("unsigned aspect = 1, bool show_performance_overlay = true);",
     "unsigned aspect = 1, bool show_performance_overlay = true, int ee_rate = -1);"),
])
rep("ps4/vm/GameMenu.cpp", [
    ("\t\tbool show_performance_overlay)\n\t{\n\t\tconst unsigned state = PackInGameMenuState({visible, selected, upscale, max_upscale, aspect,\n"
     "\t\t\tshow_performance_overlay});",
     "\t\tbool show_performance_overlay, int ee_rate)\n\t{\n\t\tconst unsigned state = PackInGameMenuState({visible, selected, upscale, max_upscale, aspect,\n"
     "\t\t\tshow_performance_overlay, ee_rate});"),
    ("\t\t\tconst bool show_performance_overlay = menu_state.show_overlay;\n",
     "\t\t\tconst bool show_performance_overlay = menu_state.show_overlay;\n"
     "\t\t\tconst int ee_rate = menu_state.ee_rate;\n"),
    ("\t\t\tconst char* labels[] = {\"Volver al juego\", resolution, aspect_label, performance_label, \"Volver a la biblioteca\", \"Salir de la aplicación\"};",
     "\t\t\t// Forja SX2: los mismos porcentajes que muestra PCSX2 para EECycleRate -3..3.\n"
     "\t\t\tstatic constexpr const char* ee_names[] = {\"50 %\", \"60 %\", \"75 %\", \"100 % (PS2 real)\", \"130 %\", \"180 %\", \"300 %\"};\n"
     "\t\t\tchar ee_label[96];\n"
     "\t\t\tstd::snprintf(ee_label, sizeof(ee_label), \"Velocidad del CPU de la PS2:  < %s >\", ee_names[std::clamp(ee_rate, -3, 3) + 3]);\n"
     "\t\t\tconst char* labels[] = {\"Volver al juego\", resolution, aspect_label, performance_label, ee_label, \"Volver a la biblioteca\", \"Salir de la aplicación\"};"),
    ("\t\t\t\t\"Mostrar u ocultar FPS, velocidad y tiempos\",\n",
     "\t\t\t\t\"Mostrar u ocultar FPS, velocidad y tiempos\",\n"
     "\t\t\t\t\"Menos % = más FPS en la PS4 (el juego puede perder cuadros). Se guarda para este juego\",\n"),
    # Iconos: la fila 4 nueva es un velocímetro; la flecha y la X bajan una fila.
    ("\t\t\t\telse if (row == 4)\n\t\t\t\t{\n\t\t\t\t\tdraw->AddLine(point(cx - 12, cy), point(cx + 10, cy), icon_line, 3 * scale);",
     "\t\t\t\telse if (row == 4)\n\t\t\t\t{\n"
     "\t\t\t\t\tdraw->AddCircle(point(cx, cy + 4), 14 * scale, icon_line, 24, 2 * scale);\n"
     "\t\t\t\t\tdraw->AddLine(point(cx, cy + 4), point(cx + 8, cy - 6), icon_line, 3 * scale);\n"
     "\t\t\t\t}\n"
     "\t\t\t\telse if (row == 5)\n\t\t\t\t{\n\t\t\t\t\tdraw->AddLine(point(cx - 12, cy), point(cx + 10, cy), icon_line, 3 * scale);"),
    # 7 filas más juntas para que no pisen las ayudas de abajo (antes eran 6 filas de 84 px).
    ("\t\t\tfor (unsigned row = 0; row < 6; row++)\n\t\t\t{\n\t\t\t\tconst float y = 206.0f + row * 84.0f;",
     "\t\t\tfor (unsigned row = 0; row < 7; row++)\n\t\t\t{\n\t\t\t\tconst float y = 200.0f + row * 77.0f;"),
    ("\t\t\t\tdraw->AddRectFilled(point(112, y), point(1808, y + 82),",
     "\t\t\t\tdraw->AddRectFilled(point(112, y), point(1808, y + 72),"),
    ("\t\t\t\t\tdraw->AddRect(point(112, y), point(1808, y + 82), accent, 10.0f * scale, 2.0f * scale, 0);",
     "\t\t\t\t\tdraw->AddRect(point(112, y), point(1808, y + 72), accent, 10.0f * scale, 2.0f * scale, 0);"),
    ("\t\t\t\tdraw_option_icon(row, 168, y + 41, selected);\n"
     "\t\t\t\tdraw->AddText(font, 26.0f * scale, point(226, y + 19), white, labels[row]);\n"
     "\t\t\t\tdraw->AddText(font, 16.0f * scale, point(226, y + 53), muted, descriptions[row]);\n"
     "\t\t\t\tif (row == 1 || row == 2 || row == 3)\n"
     "\t\t\t\t\tdraw->AddText(font, 20.0f * scale, point(1580, y + 29), selected ? white : muted,\n"
     "\t\t\t\t\t\trow == 1 ? \"AJUSTAR\" : (row == 2 ? \"ELEGIR\" : \"CAMBIAR\"));",
     "\t\t\t\tdraw_option_icon(row, 168, y + 36, selected);\n"
     "\t\t\t\tdraw->AddText(font, 26.0f * scale, point(226, y + 12), white, labels[row]);\n"
     "\t\t\t\tdraw->AddText(font, 16.0f * scale, point(226, y + 45), muted, descriptions[row]);\n"
     "\t\t\t\tif (row >= 1 && row <= 4)\n"
     "\t\t\t\t\tdraw->AddText(font, 20.0f * scale, point(1580, y + 24), selected ? white : muted,\n"
     "\t\t\t\t\t\t(row == 1 || row == 4) ? \"AJUSTAR\" : (row == 2 ? \"ELEGIR\" : \"CAMBIAR\"));"),
])

# ---------- main.cpp: estado, lectura al abrir, navegación de 7 filas y guardado por juego ----------
APLICAR_EE = '''
// Forja SX2: the pause menu's "PS2 CPU speed" row. The value goes to this game's settings ini
// (gamesettings/<serial>_<crc>.ini, PCSX2's own per-game file), then the game layer is reloaded so the
// recompilers pick up the new EECycleRate. ReloadGameSettings rebuilds EmuConfig from the settings
// layers, which drops the session-only resolution and aspect choices, so those are put back afterwards.
static void ApplyGameEECycleRate(int rate)
{
	rate = std::clamp(rate, -3, 3);
	const std::string serial = VMManager::GetDiscSerial();
	const u32 crc = VMManager::GetDiscCRC();
	if (crc == 0)
	{
		ProbeLog("EE cycle rate: no disc CRC yet, not saved\\n");
		return;
	}
	const std::string path = VMManager::GetGameSettingsPath(serial, crc);
	INISettingsInterface ini(path);
	ini.Load();
	ini.SetIntValue("EmuCore/Speedhacks", "EECycleRate", rate);
	const bool saved = ini.Save();
	const bool reloaded = VMManager::ReloadGameSettings();
	EmuConfig.GS.UpscaleMultiplier = static_cast<float>(s_game_upscale);
	EmuConfig.GS.AspectRatio = static_cast<AspectRatioType>(std::clamp(s_game_aspect, 0u, 4u));
	EmuConfig.CurrentAspectRatio = EmuConfig.GS.AspectRatio;
	MTGS::ApplySettings();
	MTGS::WaitGS(false, false, false);
	s_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;
	ProbeLog("EE cycle rate: requested=%d active=%d saved=%d reloaded=%d file=%s\\n", rate, s_game_ee_rate,
		saved ? 1 : 0, reloaded ? 1 : 0, path.c_str());
}

void VMHostPoll()
{'''

rep("ps4/vm/main.cpp", [
    ('#include "common/MemorySettingsInterface.h"\n',
     '#include "common/MemorySettingsInterface.h"\n#include "INISettingsInterface.h"\n'),
    ("static bool s_game_show_performance_overlay = true;\n",
     "static bool s_game_show_performance_overlay = true;\nstatic int s_game_ee_rate = -1;\n"),
    ("\tPS4GameMenu::SetInGameMenu(s_game_menu_open, s_game_menu_selection, s_game_upscale, s_max_game_upscale,\n"
     "\t\ts_game_aspect, s_game_show_performance_overlay);",
     "\tPS4GameMenu::SetInGameMenu(s_game_menu_open, s_game_menu_selection, s_game_upscale, s_max_game_upscale,\n"
     "\t\ts_game_aspect, s_game_show_performance_overlay, s_game_ee_rate);"),
    ("\nvoid VMHostPoll()\n{", APLICAR_EE),
    # Al abrir el menú: leer el valor activo.
    ("\t\ts_game_menu_open = true;\n\t\ts_game_menu_selection = 0;\n\t\ts_game_menu_direction = 0;\n",
     "\t\ts_game_menu_open = true;\n\t\ts_game_menu_selection = 0;\n\t\ts_game_ee_rate = EmuConfig.Speedhacks.EECycleRate;\n"
     "\t\ts_game_menu_direction = 0;\n"),
    # Filas ajustables 1-4; volver a la biblioteca = 5, salir = 6.
    ("if (cancel || (confirm && s_game_menu_selection != 1 && s_game_menu_selection != 2 && s_game_menu_selection != 3))",
     "if (cancel || (confirm && (s_game_menu_selection < 1 || s_game_menu_selection > 4)))"),
    ("\t\tif (!cancel && s_game_menu_selection == 4)\n\t\t{\n\t\t\ts_return_to_main_menu",
     "\t\tif (!cancel && s_game_menu_selection == 5)\n\t\t{\n\t\t\ts_return_to_main_menu"),
    ("\t\telse if (!cancel && s_game_menu_selection == 5)\n\t\t{\n\t\t\tProbeLog(\"VM stop request: exit application\\n\");",
     "\t\telse if (!cancel && s_game_menu_selection == 6)\n\t\t{\n\t\t\tProbeLog(\"VM stop request: exit application\\n\");"),
    ("s_game_menu_selection = s_game_menu_selection == 0 ? 5 : s_game_menu_selection - 1;",
     "s_game_menu_selection = s_game_menu_selection == 0 ? 6 : s_game_menu_selection - 1;"),
    ("s_game_menu_selection = (s_game_menu_selection + 1) % 6;",
     "s_game_menu_selection = (s_game_menu_selection + 1) % 7;"),
    ("else if (s_game_menu_selection == 1 || s_game_menu_selection == 2 || s_game_menu_selection == 3)\n\t\t\t{",
     "else if (s_game_menu_selection >= 1 && s_game_menu_selection <= 4)\n\t\t\t{"),
    ("\t\t\t\telse if (delta != 0)\n\t\t\t\t{\n\t\t\t\t\ts_game_show_performance_overlay = !s_game_show_performance_overlay;",
     "\t\t\t\telse if (s_game_menu_selection == 4)\n\t\t\t\t{\n"
     "\t\t\t\t\tconst int requested = std::clamp(s_game_ee_rate + delta, -3, 3);\n"
     "\t\t\t\t\tif (requested != s_game_ee_rate)\n"
     "\t\t\t\t\t\tApplyGameEECycleRate(requested);\n"
     "\t\t\t\t}\n"
     "\t\t\t\telse if (delta != 0)\n\t\t\t\t{\n\t\t\t\t\ts_game_show_performance_overlay = !s_game_show_performance_overlay;"),
    ('PS4GameMenu::ShowLoading(100, "Ready");', 'PS4GameMenu::ShowLoading(100, "Listo");'),
])
print("Listo.")
