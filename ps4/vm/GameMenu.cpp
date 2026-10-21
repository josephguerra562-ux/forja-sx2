// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Uses the same Vulkan/ImGui presentation as the in-game HUD.
#include "GameMenu.h"
#include "ForjaLang.h"
#include "ForjaSettings.h"
#include "InGameMenuState.h"
#include "PhysicalDisc.h"
#include "DiscCopy.h"
#include "CoverDownload.h"
#include "ReleaseCheck.h"
#include "GameDatabase.h"
#include "GameList.h"
#include "CDVD/IsoFileFormats.h"
#include "common/Error.h"
#include "MTGS.h"
#include "GS/GS.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "stb_image.h"
#include <orbis/Pad.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <memory>
#include <pthread.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

extern void ProbeLog(const char* format, ...);
extern void ForjaStartupLogDone(); // main.cpp (compilation 69): the log back to its queue once the library is up
// PlatformVM.cpp: a "STAGE:" line with the PS4's free flexible memory.
extern "C" void orbis_stage(const char* stage);
// Forja SX2 (languages): the fonts UIVM.cpp made, Chinese first and Japanese first.
extern ImFont* g_forja_language_fonts[2];
using ForjaLang::T;
using ForjaLang::TF;
// Forja SX2: the watchdog in main.cpp checks that the library loop keeps turning.
extern std::atomic<u64> g_forja_menu_ticks;
extern std::atomic<bool> g_forja_menu_active;
// Forja SX2: GitHub Actions' run number (CMakeLists.txt); 0 for a local build.
#ifndef FORJA_BUILD_NUMBER
#define FORJA_BUILD_NUMBER 0
#endif
#ifndef FORJA_VERSION_MAJOR
#define FORJA_VERSION_MAJOR 1
#define FORJA_VERSION_MINOR 0
#endif

namespace PS4GameMenu
{
	// Forja SX2 (compilation 42, 44 for every row): the text of the rows whose values are names (the patches, the VU1 filter,
	// HD textures, the recommended settings, the discs), from the CPU thread (SetRowText) to the GS thread's render.
	namespace
	{
		std::mutex s_patch_text_mutex;
		std::string s_row_labels[ROW_COUNT];
		std::string s_row_descriptions[ROW_COUNT];
		std::string s_state_date; // compilation 58
	} // namespace

	void SetStateSlotDate(const std::string& date)
	{
		std::lock_guard lock(s_patch_text_mutex);
		s_state_date = date;
	}

	void SetRowText(unsigned row, const std::string& label, const std::string& description)
	{
		if (row >= ROW_COUNT)
			return;
		std::lock_guard lock(s_patch_text_mutex);
		s_row_labels[row] = label;
		s_row_descriptions[row] = description;
	}

	void SetPatchMenuText(const std::string& label, const std::string& description)
	{
		SetRowText(ROW_PATCH, label, description);
	}

	namespace
	{
		constexpr const char* Directory = "/data/ForjaSX2/roms";
		constexpr const char* CoversDirectory = "/data/ForjaSX2/covers";
		constexpr const char* FavoritesFile = "/data/ForjaSX2/favorites.txt";
		constexpr const char* RecentFile = "/data/ForjaSX2/recent.txt";
		constexpr const char* UiAssetsDirectory = "/app0/resources/ui";
		constexpr const char* IntroDirectory = "/app0/resources/intro";
		constexpr size_t GridColumns = 4;
		constexpr size_t VisibleRows = 2;
		constexpr size_t VisibleItems = GridColumns * VisibleRows;
		enum class LibraryView : unsigned
		{
			Library,
			Disc,
			Recent,
			Favorites,
			Settings,
		};
		struct CoverImage
		{
			std::string path;
			int width = 0;
			int height = 0;
			std::vector<unsigned char> pixels;
		};
		struct Item
		{
			std::string name;
			std::string path;
			std::shared_ptr<CoverImage> cover;
			bool favorite = false;
			std::string serial;
			GamePlatform platform = GamePlatform::Unknown;
		};
		struct CoverTexture
		{
			std::shared_ptr<CoverImage> source;
			ImTextureData* texture = nullptr;
		};
		struct DiscProbe
		{
			pthread_t thread{};
			bool active = false;
			std::atomic<bool> done{false};
			std::unique_ptr<PS4Disc::Reader> reader;
			std::string error;
			bool Start()
			{
				if (active) return false;
				reader.reset();
				error.clear();
				done.store(false, std::memory_order_relaxed);
				const int result = pthread_create(&thread, nullptr, [](void* context) -> void* {
					auto* probe = static_cast<DiscProbe*>(context);
					probe->reader = PS4Disc::Reader::Open(probe->error);
					probe->done.store(true, std::memory_order_release);
					return nullptr;
				}, this);
				active = result == 0;
				if (!active) error = T("No se pudo iniciar la detección del disco.");
				return active;
			}
			bool Collect()
			{
				if (!active || !done.load(std::memory_order_acquire)) return false;
				pthread_join(thread, nullptr);
				active = false;
				return true;
			}
			~DiscProbe() { if (active) pthread_join(thread, nullptr); }
		};
		struct UiImage
		{
			std::string name;
			std::shared_ptr<CoverImage> source;
		};
		// Only accessed by GS callbacks. The CPU owns a separate list for selection.
		std::vector<Item> s_items;
		size_t s_selected = 0;
		bool s_visible = false;
		bool s_connected = false;
		bool s_sidebar_focused = false;
		bool s_exit_dialog_visible = false;
		bool s_exit_confirm = false;
		// Forja SX2: the updater's question (ReleaseCheck.cpp), drawn like the exit dialog. GS thread.
		bool s_update_dialog_visible = false;
		bool s_update_confirm = true;
		// CPU thread: asked once per app run when a newer build shows up; OPTIONS asks again.
		bool s_update_asked = false;
		LibraryView s_library_view = LibraryView::Library;
		// Forja SX2: the Settings page. The CPU thread owns the values; the GS thread draws this copy.
		ForjaSettings::Values s_settings_gs;
		unsigned s_settings_row_gs = 0;
		// The Settings page's rows, in their order on the page (compilation 53: the menu's button; 2026-10-19: the language,
		// first, and whether to ask for it at every start).
		enum SettingRow : unsigned
		{
			SET_LANGUAGE,
			SET_ASK_LANGUAGE,
			SET_EE_RATE,
			SET_CPU_AUTO,
			SET_UPSCALE,
			SET_ASPECT,
			SET_OVERLAY,
			SET_STICK,
			SET_FAST_CDVD,
			SET_FAST_BOOT,
			SET_READBACK,
			SET_VIDEO_MODE,
			SET_FRAME_GEN,
			SET_MENU_BUTTON,
			SettingsRows
		};

		const char* SettingLabel(unsigned row)
		{
			static constexpr const char* labels[SettingsRows] = {N_("Idioma"), N_("Preguntar el idioma al iniciar"),
				N_("Velocidad del CPU de la PS2"), N_("CPU automático"), N_("Resolución interna"), N_("Relación de aspecto"),
				N_("Contador de FPS al jugar"), N_("Sensibilidad de la palanca"), N_("Cargas rápidas del disco"),
				N_("Saltar el logo de la PS2"), N_("Lecturas de la gráfica (readbacks)"), N_("Modo video automático"),
				N_("Generación de cuadros (30 a 60 FPS)"), N_("Botón del menú del emulador")};
			return T(labels[std::min(row, SettingsRows - 1)]);
		}

		const char* SettingHelp(unsigned row)
		{
			static constexpr const char* help[SettingsRows] = {
				N_("El idioma de todo el emulador. X abre la lista de idiomas"),
				N_("Sí: la lista de idiomas sale cada vez que abres Forja SX2"),
				N_("Todo juego empieza aquí. Si uno va en cámara lenta, súbelo en L3+R3 (se guarda para ese juego)"),
				N_("Si subes el CPU de un juego y se pone lento, baja solo en las escenas pesadas, hasta este piso"),
				N_("1x es la de la PS2. En PS4 Pro aguanta 2x-4x en juegos ligeros"),
				N_("Auto respeta lo que pide el juego"),
				N_("Muestra FPS y velocidad arriba a la derecha (se cambia también en L3+R3)"),
				N_("Qué tanto hay que empujar la palanca para llegar al tope. 133 % es como el PCSX2 de PC"),
				N_("Acorta las cargas. Algunos juegos se cuelgan con esto: si pasa, apágalo"),
				N_("Arranca directo el juego, sin la animación de la PS2"),
				N_("Si el juego se traba sin que el CPU esté al 100 %: prueba Sin esperar. Desactivadas puede romper efectos"),
				N_("En los videos del juego busca el CPU de la PS2 que da más cuadros (31 a 100 %) y lo devuelve al terminar"),
				N_("EXPERIMENTAL: un juego estable a 30 FPS se ve a 60 (la gráfica inventa el cuadro intermedio). +1 cuadro de retraso"),
				N_("Panel táctil: aprieta su mitad DERECHA. La izquierda sigue siendo el SELECT de la PS2")};
			return T(help[std::min(row, SettingsRows - 1)]);
		}

		void SettingValue(const ForjaSettings::Values& v, unsigned row, char* out, size_t size)
		{
			static constexpr const char* ee_names[] = {"27 %", "31 %", "36 %", "50 %", "60 %", "75 %", N_("100 % (PS2 real)"), "130 %",
				"180 %", "300 %"};
			static constexpr const char* cpu_auto_names[] = {N_("No"), N_("Hasta 75 %"), N_("Hasta 60 %"), N_("Hasta 50 %"),
				N_("Hasta 36 %"), N_("Hasta 31 %"), N_("Hasta 27 %")};
			static constexpr const char* aspect_names[] = {N_("Estirar"), N_("Auto"), "4:3", "16:9", "10:7"};
			const auto yes_no = [](bool on) { return on ? T("Sí") : T("No"); };
			switch (row)
			{
				case SET_LANGUAGE: std::snprintf(out, size, "%s", ForjaLang::Languages[ForjaLang::Current()].name); break;
				case SET_ASK_LANGUAGE: std::snprintf(out, size, "%s", yes_no(v.ask_language)); break;
				case SET_EE_RATE: std::snprintf(out, size, "%s", T(ee_names[std::clamp(v.ee_rate, -6, 3) + 6])); break;
				case SET_CPU_AUTO: std::snprintf(out, size, "%s", T(cpu_auto_names[-std::clamp(v.cpu_auto, -6, 0)])); break;
				case SET_UPSCALE: std::snprintf(out, size, v.upscale == 1 ? TF("%dx (nativa)") : "%dx", v.upscale); break;
				case SET_ASPECT: std::snprintf(out, size, "%s", T(aspect_names[std::clamp(v.aspect, 0, 4)])); break;
				case SET_OVERLAY: std::snprintf(out, size, "%s", yes_no(v.overlay)); break;
				case SET_STICK: std::snprintf(out, size, "%d %%", v.stick); break;
				case SET_FAST_CDVD: std::snprintf(out, size, "%s", yes_no(v.fast_cdvd)); break;
				case SET_FAST_BOOT: std::snprintf(out, size, "%s", yes_no(v.fast_boot)); break;
				case SET_READBACK:
				{
					static constexpr const char* readback_names[] = {N_("Precisas"), N_("Sin esperar (más rápido)"), N_("Desactivadas")};
					std::snprintf(out, size, "%s", T(readback_names[std::clamp(v.readback, 0, 2)]));
					break;
				}
				case SET_VIDEO_MODE: std::snprintf(out, size, "%s", yes_no(v.video_mode)); break;
				case SET_FRAME_GEN: std::snprintf(out, size, "%s", yes_no(v.frame_gen)); break;
				default:
				{
					static constexpr const char* menu_names[] = {"L3+R3", N_("Panel táctil (derecha)"), N_("Los dos")};
					std::snprintf(out, size, "%s", T(menu_names[std::clamp(v.menu_button, 0, 2)]));
					break;
				}
			}
		}

		// delta -1/+1 from Left/Right; X passes +1 with wrap so it cycles through the choices. The language row is Select()'s:
		// it loads the new language.
		void ChangeSetting(ForjaSettings::Values& v, unsigned row, int delta, bool wrap)
		{
			const auto step = [&](int value, int lo, int hi) {
				const int next = value + delta;
				if (wrap && next > hi)
					return lo;
				return std::clamp(next, lo, hi);
			};
			switch (row)
			{
				case SET_LANGUAGE: break;
				case SET_ASK_LANGUAGE: v.ask_language = !v.ask_language; break;
				case SET_EE_RATE: v.ee_rate = step(v.ee_rate, -6, 3); break;
				case SET_CPU_AUTO: v.cpu_auto = -step(-v.cpu_auto, 0, 6); break; // right: a lower floor
				case SET_UPSCALE: v.upscale = step(v.upscale, 1, 4); break;
				case SET_ASPECT: v.aspect = step(v.aspect, 0, 4); break;
				case SET_OVERLAY: v.overlay = !v.overlay; break;
				case SET_STICK:
				{
					int index = 0;
					while (index + 1 < ForjaSettings::StickStepCount && ForjaSettings::StickSteps[index] < v.stick)
						++index;
					v.stick = ForjaSettings::StickSteps[step(index, 0, ForjaSettings::StickStepCount - 1)];
					break;
				}
				case SET_FAST_CDVD: v.fast_cdvd = !v.fast_cdvd; break;
				case SET_FAST_BOOT: v.fast_boot = !v.fast_boot; break;
				case SET_READBACK: v.readback = step(v.readback, 0, 2); break;
				case SET_VIDEO_MODE: v.video_mode = !v.video_mode; break;
				case SET_FRAME_GEN: v.frame_gen = !v.frame_gen; break;
				default: v.menu_button = step(v.menu_button, 0, 2); break;
			}
		}

		void PublishSettings(const ForjaSettings::Values& values, unsigned row)
		{
			MTGS::RunOnGSThread([values, row]() {
				s_settings_gs = values;
				s_settings_row_gs = row;
			});
		}

		// Forja SX2 (2026-10-19): the language picker, over the library at every start (or from Settings > Idioma). Its texts are
		// in the language under the cursor, so a player who doesn't read Spanish finds their own; the CPU thread picks them.
		constexpr unsigned LanguageRowsPerColumn = 7;
		bool s_language_dialog_visible = false;
		unsigned s_language_focus = 0;
		std::string s_language_texts[4];

		void PublishLanguageDialog(bool visible, unsigned focus)
		{
			ForjaLang::LoadTable(focus);
			std::string texts[4] = {ForjaLang::TIn(focus, N_("Elige el idioma")),
				ForjaLang::TIn(focus, N_("También se cambia en Ajustes, donde puedes apagar esta pregunta.")),
				ForjaLang::TIn(focus, N_("Aceptar")), ForjaLang::TIn(focus, N_("Atrás"))};
			MTGS::RunOnGSThread([visible, focus, texts]() {
				s_language_dialog_visible = visible;
				s_language_focus = focus;
				for (unsigned i = 0; i < 4; i++)
					s_language_texts[i] = texts[i];
			});
		}

		// Japanese is drawn with the font that has its own shapes first (UIVM.cpp).
		ImFont* MenuFont()
		{
			ImFont* japanese = g_forja_language_fonts[1];
			return ForjaLang::Current() == ForjaLang::Japanese && japanese ? japanese : ImGui::GetFont();
		}

		float TextWidth(ImFont* font, float pixels, const char* text)
		{
			return font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, ForjaLang::Visual(text)).x;
		}

		// Every text the menus draw goes through here: ForjaLang::Visual for Arabic and, given a width, the fit: a translation
		// longer than its place is drawn smaller (down to 80 %, in whole pixels, so the atlas keeps few sizes) and then cut with "…".
		void DrawText(ImDrawList* draw, ImFont* font, float pixels, ImVec2 position, ImU32 color, const char* text, float max_width = 0.0f)
		{
			if (!text || !*text)
				return;
			const char* shown = ForjaLang::Visual(text);
			std::string cut;
			if (max_width > 0.0f)
			{
				float width = font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, shown).x;
				if (width > max_width)
				{
					const float smaller = std::max(std::floor(pixels * std::max(0.8f, max_width / width)), 1.0f);
					position.y += (pixels - smaller) * 0.6f;
					pixels = smaller;
					width = font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, shown).x;
				}
				if (width > max_width)
				{
					// The most characters that fit with the "…", by halves.
					std::vector<size_t> ends;
					const size_t length = std::strlen(text);
					for (size_t i = 1; i <= length; i++)
						if (i == length || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80)
							ends.push_back(i);
					size_t keep = 0, low = 1, high = ends.size();
					while (low <= high)
					{
						const size_t middle = (low + high) / 2;
						const std::string candidate = std::string(text, ends[middle - 1]) + "…";
						if (font->CalcTextSizeA(pixels, FLT_MAX, 0.0f, ForjaLang::Visual(candidate.c_str())).x <= max_width)
						{
							keep = middle;
							low = middle + 1;
						}
						else
							high = middle - 1;
					}
					cut = std::string(text, keep ? ends[keep - 1] : 0);
					while (!cut.empty() && cut.back() == ' ')
						cut.pop_back();
					cut += "…";
					shown = ForjaLang::Visual(cut.c_str());
				}
			}
			draw->AddText(font, pixels, position, color, shown);
		}
		bool s_loading_visible = false;
		bool s_loading_carousel = false;
		unsigned s_loading_progress = 0;
		float s_loading_carousel_phase = 0.0f;
		std::string s_loading_status;
		bool s_intro_visible = false;
		std::shared_ptr<CoverImage> s_intro_frame;
		ImTextureData* s_intro_texture = nullptr;
		// One atomic snapshot shared by CPU and GS; field widths are defined
		// in InGameMenuState.h so publication and rendering stay consistent.
		std::atomic<std::uint64_t> s_in_game_state{0};
		std::string s_status;
		std::vector<CoverTexture> s_cover_textures;
		std::vector<UiImage> s_ui_images;

		std::shared_ptr<CoverImage> LoadImageFile(const std::string& image_path, const char* kind, bool log_success = true)
		{
			int width = 0, height = 0, channels = 0;
			unsigned char* decoded = stbi_load(image_path.c_str(), &width, &height, &channels, 4);
			if (!decoded || width <= 0 || height <= 0 || static_cast<size_t>(width) * static_cast<size_t>(height) > 8 * 1024 * 1024)
			{
				if (decoded)
					stbi_image_free(decoded);
				ProbeLog("Game menu: invalid or oversized %s %s\n", kind, image_path.c_str());
				return {};
			}
			auto cover = std::make_shared<CoverImage>();
			cover->path = image_path;
			cover->width = width;
			cover->height = height;
			cover->pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
			stbi_image_free(decoded);
			if (log_success)
				ProbeLog("Game menu: %s loaded %s (%dx%d)\n", kind, image_path.c_str(), width, height);
			return cover;
		}

		// Forja SX2 (compilation 59): a cover at the size it is drawn. Every game's cover stays decoded while the library is open,
		// and one of 512x736 takes 1.4 MB: 200 games would take ~290 MB, about all the memory the app has free (337 MB when the VM
		// starts). Shrunk to fit 300x424 (the card is 270x382, the Disc panel's 340x480), with each pixel the average of the ones
		// it covers, it takes ~0.5 MB.
		std::shared_ptr<CoverImage> ShrinkCover(std::shared_ptr<CoverImage> cover)
		{
			constexpr int max_w = 300, max_h = 424;
			if (!cover || (cover->width <= max_w && cover->height <= max_h))
				return cover;
			const float fit = std::min(static_cast<float>(max_w) / cover->width, static_cast<float>(max_h) / cover->height);
			const int w = std::max(1, static_cast<int>(cover->width * fit)), h = std::max(1, static_cast<int>(cover->height * fit));
			std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
			for (int y = 0; y < h; y++)
			{
				const int y0 = y * cover->height / h, y1 = std::max(y0 + 1, (y + 1) * cover->height / h);
				for (int x = 0; x < w; x++)
				{
					const int x0 = x * cover->width / w, x1 = std::max(x0 + 1, (x + 1) * cover->width / w);
					unsigned sum[4] = {};
					for (int sy = y0; sy < y1; sy++)
						for (int sx = x0; sx < x1; sx++)
							for (int c = 0; c < 4; c++)
								sum[c] += cover->pixels[(static_cast<size_t>(sy) * cover->width + sx) * 4 + c];
					const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
					for (int c = 0; c < 4; c++)
						pixels[(static_cast<size_t>(y) * w + x) * 4 + c] = static_cast<unsigned char>(sum[c] / count);
				}
			}
			cover->width = w;
			cover->height = h;
			cover->pixels = std::move(pixels);
			return cover;
		}

		std::shared_ptr<CoverImage> LoadCover(const std::string& name)
		{
			const size_t dot = name.find_last_of('.');
			if (dot == std::string::npos)
				return {};
			const std::string stem = name.substr(0, dot);
			const char* const extensions[] = {".png", ".jpg", ".jpeg", ".PNG", ".JPG", ".JPEG"};
			for (const char* extension : extensions)
			{
				const std::string candidate = std::string(CoversDirectory) + "/" + stem + extension;
				struct stat info{};
				if (stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
					return ShrinkCover(LoadImageFile(candidate, "cover"));
			}
			return {};
		}

		// Read the disc serial from the image instead of guessing from its display
		// name. This keeps automatic cover downloads working for ISO, BIN and CHD.
		void ReadImageInfo(Item& item)
		{
			if (item.path.empty())
				return;
			GameList::Entry entry;
			if (!GameList::PopulateEntryFromPath(item.path, &entry))
			{
				ProbeLog("Game menu: metadata unavailable for %s\n", item.path.c_str());
				return;
			}
			item.platform = entry.type == GameList::EntryType::PS1Disc ? GamePlatform::PS1 :
				entry.type == GameList::EntryType::PS2Disc ? GamePlatform::PS2 : GamePlatform::Unknown;
			item.serial = entry.serial;
			std::transform(item.serial.begin(), item.serial.end(), item.serial.begin(), [](unsigned char c) {
				return static_cast<char>(std::toupper(c));
			});
			ProbeLog("Game menu: image metadata %s -> serial=%s platform=%s\n", item.path.c_str(), item.serial.c_str(),
				item.platform == GamePlatform::PS1 ? "PS1" : item.platform == GamePlatform::PS2 ? "PS2" : "unknown");
		}

		void LoadUiAssets()
		{
			if (!s_ui_images.empty())
				return;
			static constexpr const char* names[] = {
				"menu_logo", "menu_biblioteca", "menu_disc", "menu_recentes", "menu_favoritos", "menu_configuracoes", "status_controle",
				"controle_x", "controle_bola", "controle_options", "controle_quadrado", "controle_triangulo", "controle_l1", "controle_r1",
				"navegacao_esquerda", "navegacao_direita", "loading_carousel_1", "loading_carousel_2",
				"loading_carousel_3", "loading_carousel_4"};
			for (const char* name : names)
			{
				const std::string path = std::string(UiAssetsDirectory) + "/" + name + ".png";
				s_ui_images.push_back({name, LoadImageFile(path, "UI asset")});
			}
			// (AI-assisted) Reuse the package background at runtime so the PS4 tile
			// and emulator loading screen always show the exact same artwork.
			// Forja SX2: sce_sys/pic1.png lives in the package header, not in the app's file system, so
			// /app0/sce_sys/pic1.png never opened ("invalid or oversized loading background" on every boot).
			// The same artwork ships as a UI asset instead.
			s_ui_images.push_back({"loading_background",
				LoadImageFile(std::string(UiAssetsDirectory) + "/loading_background.png", "loading background")});
			ProbeLog("Game menu: loaded %zu UI button assets\n", s_ui_images.size());
		}

		std::shared_ptr<CoverImage> FindUiAsset(const char* name)
		{
			for (const UiImage& image : s_ui_images)
				if (image.name == name)
					return image.source;
			return {};
		}

		void ClearCoverTextures()
		{
			for (CoverTexture& cover : s_cover_textures)
			{
				if (!cover.texture)
					continue;
				if (GSTexture* texture = static_cast<GSTexture*>(cover.texture->BackendUserData); texture && g_gs_device)
					g_gs_device->Recycle(texture);
				if (ImGui::GetCurrentContext())
					ImGui::UnregisterUserTexture(cover.texture);
				delete cover.texture;
			}
			s_cover_textures.clear();
		}

		void ClearIntroTexture()
		{
			if (!s_intro_texture)
				return;
			if (GSTexture* texture = static_cast<GSTexture*>(s_intro_texture->BackendUserData); texture && g_gs_device)
				g_gs_device->Recycle(texture);
			if (ImGui::GetCurrentContext())
				ImGui::UnregisterUserTexture(s_intro_texture);
			delete s_intro_texture;
			s_intro_texture = nullptr;
		}

		ImTextureData* GetIntroTexture()
		{
			if (s_intro_texture || !s_intro_frame)
				return s_intro_texture;
			s_intro_texture = new ImTextureData();
			s_intro_texture->Create(ImTextureFormat_RGBA32, s_intro_frame->width, s_intro_frame->height);
			std::memcpy(s_intro_texture->Pixels, s_intro_frame->pixels.data(), s_intro_frame->pixels.size());
			s_intro_texture->UseColors = true;
			ImGui::RegisterUserTexture(s_intro_texture);
			return s_intro_texture;
		}

		ImTextureData* GetCoverTexture(const std::shared_ptr<CoverImage>& source)
		{
			if (!source)
				return nullptr;
			for (const CoverTexture& cover : s_cover_textures)
			{
				if (cover.source && cover.source->path == source->path)
					return cover.texture;
			}
			auto* texture = new ImTextureData();
			texture->Create(ImTextureFormat_RGBA32, source->width, source->height);
			std::memcpy(texture->Pixels, source->pixels.data(), source->pixels.size());
			texture->UseColors = true;
			ImGui::RegisterUserTexture(texture);
			s_cover_textures.push_back({source, texture});
			return texture;
		}

		std::vector<Item> Scan()
		{
			std::vector<Item> items;
			if (DIR* dir = opendir(Directory))
			{
				while (dirent* entry = readdir(dir))
				{
					const std::string name(entry->d_name);
					const size_t dot = name.find_last_of('.');
					if (name.empty() || name[0] == '.' || dot == std::string::npos)
						continue;
					std::string extension = name.substr(dot);
					std::transform(extension.begin(), extension.end(), extension.begin(),
						[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
					if (extension != ".iso" && extension != ".bin" && extension != ".chd" && extension != ".cso" &&
					    extension != ".zso" && extension != ".gz")
						continue;
					const std::string path = std::string(Directory) + "/" + name;
					struct stat info{};
					if (stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0)
					{
						Item item{name.substr(0, dot), path, LoadCover(name), false, {}};
						ReadImageInfo(item);
						if (!item.cover && !item.serial.empty())
							item.cover = LoadCover(item.serial + ".iso");
						items.push_back(std::move(item));
					}
				}
				closedir(dir);
			}
			std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
			ProbeLog("Game menu: found %zu .iso/.bin/.chd/.cso/.zso/.gz files in %s\n", items.size(), Directory);
			orbis_stage("game library listed (covers loaded)");
			return items;
		}

		bool ContainsPath(const std::vector<std::string>& paths, const std::string& path)
		{
			return std::find(paths.begin(), paths.end(), path) != paths.end();
		}

		std::vector<std::string> LoadPathList(const char* filename)
		{
			std::vector<std::string> paths;
			FILE* file = std::fopen(filename, "rb");
			if (!file)
				return paths;
			char line[1024];
			while (std::fgets(line, sizeof(line), file))
			{
				std::string path(line);
				while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
					path.pop_back();
				if (!path.empty() && !ContainsPath(paths, path))
					paths.push_back(std::move(path));
			}
			std::fclose(file);
			return paths;
		}

		bool SavePathList(const char* filename, const std::vector<std::string>& paths)
		{
			const std::string temporary = std::string(filename) + ".tmp";
			FILE* file = std::fopen(temporary.c_str(), "wb");
			if (!file)
				return false;
			bool success = true;
			for (const std::string& path : paths)
				success = success && std::fprintf(file, "%s\n", path.c_str()) >= 0;
			success = std::fclose(file) == 0 && success;
			if (success)
				success = std::rename(temporary.c_str(), filename) == 0;
			if (!success)
				std::remove(temporary.c_str());
			return success;
		}

		std::vector<Item> BuildView(const std::vector<Item>& library, LibraryView view,
			const std::vector<std::string>& favorites, const std::vector<std::string>& recent)
		{
			std::vector<Item> items;
			if (view == LibraryView::Disc)
			{
				// Detection is asynchronous; the CPU publishes the result when it is ready.
			}
			else if (view == LibraryView::Recent)
			{
				for (const std::string& path : recent)
				{
					const auto found = std::find_if(library.begin(), library.end(), [&](const Item& item) { return item.path == path; });
					if (found != library.end())
					{
						items.push_back(*found);
						items.back().favorite = ContainsPath(favorites, path);
					}
				}
			}
			else if (view == LibraryView::Library || view == LibraryView::Favorites)
			{
				for (Item item : library)
				{
					item.favorite = ContainsPath(favorites, item.path);
					if (view == LibraryView::Library || item.favorite)
						items.push_back(std::move(item));
				}
			}
			if (items.empty())
			{
				const char* message = view == LibraryView::Disc      ? T("No se detectó ningún disco") :
				                      view == LibraryView::Recent    ? T("No hay juegos recientes") :
				                      view == LibraryView::Favorites ? T("No hay juegos favoritos") :
				                      view == LibraryView::Settings  ? T("Ajustes de Forja SX2 (para todos los juegos)") :
				                                                       T("No se encontraron juegos");
				items.push_back({message, {}, {}, false});
			}
			return items;
		}

		void Publish(const std::vector<Item>& items, LibraryView view)
		{
			MTGS::RunOnGSThread([items, view]() {
				// Forja SX2: the textures (covers, icons, the logo, the 1920x1080 loading background) are kept across view changes;
				// clearing them on every change re-uploaded ~16 MB each time. They are let go only once there are many covers.
				if (s_cover_textures.size() > 64)
					ClearCoverTextures();
				if (g_gs_device)
					ProbeLog("Game menu: GPU texture pool %.1f MB, %zu menu textures\n",
						static_cast<double>(g_gs_device->GetPoolMemoryUsage()) / (1024.0 * 1024.0), s_cover_textures.size());
				s_items = items;
				s_library_view = view;
				s_loading_visible = false;
				s_loading_carousel = false;
				s_intro_visible = false;
				s_visible = true;
			});
		}

		void Present(size_t selected, const std::string& status, bool connected, bool sidebar_focused,
			bool exit_dialog_visible, bool exit_confirm, bool update_dialog_visible = false, bool update_confirm = false)
		{
			MTGS::RunOnGSThread([selected, status, connected, sidebar_focused, exit_dialog_visible, exit_confirm, update_dialog_visible,
									update_confirm]() {
				s_connected = connected;
				s_sidebar_focused = sidebar_focused;
				s_exit_dialog_visible = exit_dialog_visible;
				s_exit_confirm = exit_confirm;
				s_update_dialog_visible = update_dialog_visible;
				s_update_confirm = update_confirm;
				s_selected = selected;
				s_status = status;
				GSPresentCurrentFrame();
			});
			// The PS4 window is surfaceless, so MTGS's generic idle loop does not present.
			// Keep at most one menu frame queued. Transition to the VM needs console testing.
			MTGS::WaitGS(false);
		}

		void PresentStartupLoading(unsigned progress, float carousel_phase)
		{
			MTGS::RunOnGSThread([progress, carousel_phase]() {
				s_visible = false;
				s_intro_visible = false;
				s_loading_visible = true;
				s_loading_carousel = true;
				s_loading_progress = std::min(progress, 100u);
				s_loading_carousel_phase = carousel_phase;
				s_loading_status = T("Cargando...");
				GSPresentCurrentFrame();
			});
			MTGS::WaitGS(false);
		}

		void PresentIntroFrame(const std::shared_ptr<CoverImage>& frame)
		{
			MTGS::RunOnGSThread([frame]() {
				ClearIntroTexture();
				s_intro_frame = frame;
				s_loading_visible = false;
				s_loading_carousel = false;
				s_intro_visible = static_cast<bool>(frame);
				GSPresentCurrentFrame();
			});
			MTGS::WaitGS(false);
		}

		void HideIntro()
		{
			MTGS::RunOnGSThread([]() {
				s_intro_visible = false;
				s_intro_frame.reset();
				ClearIntroTexture();
			});
			MTGS::WaitGS(false);
		}

		class IntroAudio
		{
		public:
			bool Start()
			{
				const std::string path = std::string(IntroDirectory) + "/audio_s16le_48k_stereo.pcm";
				FILE* file = std::fopen(path.c_str(), "rb");
				if (!file)
				{
					ProbeLog("Startup intro: audio file unavailable %s\n", path.c_str());
					return false;
				}
				std::fseek(file, 0, SEEK_END);
				const long bytes = std::ftell(file);
				std::rewind(file);
				if (bytes <= 0 || (bytes % sizeof(int16_t)) != 0)
				{
					std::fclose(file);
					ProbeLog("Startup intro: invalid audio size=%ld\n", bytes);
					return false;
				}
				m_samples.resize(static_cast<size_t>(bytes) / sizeof(int16_t));
				const size_t read = std::fread(m_samples.data(), 1, static_cast<size_t>(bytes), file);
				std::fclose(file);
				if (read != static_cast<size_t>(bytes))
				{
					ProbeLog("Startup intro: short audio read=%zu expected=%ld\n", read, bytes);
					m_samples.clear();
					return false;
				}

				const int module = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_AUDIOOUT);
				const int init = sceAudioOutInit();
				m_handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
					0, Grain, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
				ProbeLog("Startup intro: audio module=%#x init=%#x open=%#x samples=%zu\n",
					module, init, m_handle, m_samples.size());
				if (m_handle < 0)
					return false;
				int32_t levels[2] = {32768, 32768};
				sceAudioOutSetVolume(m_handle, 0x3, levels);
				pthread_attr_t attr;
				int result = pthread_attr_init(&attr);
				if (result == 0)
				{
					result = pthread_attr_setstacksize(&attr, 256 * 1024);
					if (result == 0)
						result = pthread_create(&m_thread, &attr, ThreadEntry, this);
					pthread_attr_destroy(&attr);
				}
				if (result != 0)
				{
					sceAudioOutClose(m_handle);
					m_handle = -1;
					ProbeLog("Startup intro: audio thread failed=%d\n", result);
					return false;
				}
				m_started = true;
				return true;
			}

			void Finish()
			{
				if (m_started)
				{
					pthread_join(m_thread, nullptr);
					m_started = false;
				}
				if (m_handle >= 0)
				{
					const int result = sceAudioOutClose(m_handle);
					ProbeLog("Startup intro: audio close=%#x\n", result);
					m_handle = -1;
				}
			}

			~IntroAudio() { Finish(); }

		private:
			static constexpr size_t Grain = 512;
			static void* ThreadEntry(void* context)
			{
				static_cast<IntroAudio*>(context)->Run();
				return nullptr;
			}

			void Run()
			{
				alignas(64) int16_t buffer[Grain * 2]{};
				for (size_t offset = 0; offset < m_samples.size(); offset += Grain * 2)
				{
					const size_t count = std::min(Grain * 2, m_samples.size() - offset);
					std::memset(buffer, 0, sizeof(buffer));
					std::memcpy(buffer, m_samples.data() + offset, count * sizeof(int16_t));
					const int result = sceAudioOutOutput(m_handle, buffer);
					if (result < 0)
					{
						ProbeLog("Startup intro: audio output failed=%#x offset=%zu\n", result, offset);
						break;
					}
				}
				sceAudioOutOutput(m_handle, nullptr);
			}

			std::vector<int16_t> m_samples;
			int m_handle = -1;
			pthread_t m_thread{};
			bool m_started = false;
		};

		void PlayStartupSequence()
		{
			ProbeLog("STAGE: branded startup loading\n");
			constexpr double loading_seconds = 2.35;
			Common::Timer loading_timer;
			for (;;)
			{
				const double elapsed = loading_timer.GetTimeSeconds();
				const unsigned progress = static_cast<unsigned>(std::min(elapsed / loading_seconds, 1.0) * 100.0);
				PresentStartupLoading(progress, static_cast<float>(elapsed * 275.0));
				if (elapsed >= loading_seconds)
					break;
				Threading::Sleep(16);
			}

			ProbeLog("STAGE: startup intro video\n");
			unsigned frame_count = 0;
			double fps = 0.0;
			const std::string manifest_path = std::string(IntroDirectory) + "/manifest.txt";
			if (FILE* manifest = std::fopen(manifest_path.c_str(), "r"))
			{
				char line[96];
				while (std::fgets(line, sizeof(line), manifest))
				{
					unsigned value = 0;
					double decimal = 0.0;
					if (std::sscanf(line, "frames=%u", &value) == 1)
						frame_count = value;
					else if (std::sscanf(line, "fps=%lf", &decimal) == 1)
						fps = decimal;
				}
				std::fclose(manifest);
			}
			if (frame_count == 0 || frame_count > 300 || fps < 1.0 || fps > 60.0)
			{
				ProbeLog("Startup intro: invalid manifest frames=%u fps=%.3f\n", frame_count, fps);
				HideIntro();
				return;
			}
			char path[160];
			std::snprintf(path, sizeof(path), "%s/frame_%03u.jpg", IntroDirectory, 1u);
			auto frame = LoadImageFile(path, "intro frame", false);
			if (!frame)
			{
				ProbeLog("Startup intro: first frame unavailable; continuing to library\n");
				HideIntro();
				return;
			}
			PresentIntroFrame(frame);
			IntroAudio audio;
			audio.Start();
			Common::Timer video_timer;
			for (unsigned index = 2; index <= frame_count; index++)
			{
				std::snprintf(path, sizeof(path), "%s/frame_%03u.jpg", IntroDirectory, index);
				auto next = LoadImageFile(path, "intro frame", false);
				if (!next)
					break;
				const double deadline = static_cast<double>(index - 1) / fps;
				while (video_timer.GetTimeSeconds() < deadline)
					Threading::Sleep(1);
				PresentIntroFrame(next);
			}
			while (video_timer.GetTimeSeconds() < static_cast<double>(frame_count) / fps)
				Threading::Sleep(1);
			audio.Finish();
			HideIntro();
			ProbeLog("Startup intro: finished frames=%u fps=%.3f\n", frame_count, fps);
		}
	} // namespace

	bool Select(int& pad, std::string& path, GamePlatform& platform)
	{
		platform = GamePlatform::Unknown;
		ProbeLog("STAGE: Game menu\n");
		if (!MTGS::WaitForOpen())
		{
			ProbeLog("Game menu: GS initialization failed\n");
			return false;
		}
		// Forja SX2 (2026-10-19): the saved language, before the first text is drawn. Settings page state too: every change is
		// saved at once (ajustes.ini) and used by the next game.
		ForjaSettings::Values settings = ForjaSettings::Load();
		if (!ForjaLang::Use(ForjaLang::IndexOf(settings.language)))
			ProbeLog("Game menu: the file of language %s is missing; Spanish\n", settings.language);
		ProbeLog("Game menu: language %s%s\n", ForjaLang::CurrentCode(), settings.ask_language ? ", asked at start" : "");
		// Decode the packaged button artwork on the CPU thread; the GS callback
		// only uploads textures and draws them.
		LoadUiAssets();
		PresentStartupLoading(0, 0.0f);
		// Keep the system pic1 visible until the first Vulkan loading frame exists.
		sceSystemServiceHideSplashScreen();
		PlayStartupSequence();
		// (AI-assisted) Query the public release endpoint asynchronously. The
		// launcher remains usable while DNS/HTTPS is in progress.
		PS4Release::Start(FORJA_BUILD_NUMBER);
		auto library = Scan();
		auto favorites = LoadPathList(FavoritesFile);
		auto recent = LoadPathList(RecentFile);
		LibraryView view = LibraryView::Library;
		auto items = BuildView(library, view, favorites, recent);
		Publish(items, view);
		size_t selected = 0;
		OrbisPadData initial{};
		u32 previous = pad >= 0 && scePadReadState(pad, &initial) == 0 ? initial.buttons : 0;
		Common::Timer timer;
		double next_repeat = 0.0, next_pad_open = 0.0;
		u32 held_direction = 0;
		bool start = false;
		bool sidebar_focused = false;
		bool exit_dialog_visible = false;
		bool exit_confirm = false;
		bool update_dialog_visible = false;
		bool update_confirm = true;
		std::string status;
		unsigned settings_row = 0;
		PublishSettings(settings, settings_row);
		// The language is saved with the rest, so ajustes.ini always names the one in use.
		const auto save_settings = [&](const char* saved) {
			std::snprintf(settings.language, sizeof(settings.language), "%s", ForjaLang::CurrentCode());
			status = ForjaSettings::Save(settings) ? saved : T("No se pudieron guardar los ajustes.");
			PublishSettings(settings, settings_row);
		};
		const auto change_setting = [&](int delta, bool wrap) {
			ChangeSetting(settings, settings_row, delta, wrap);
			ProbeLog("Game menu: setting %u changed\n", settings_row);
			save_settings(T("Guardado. Se aplica al iniciar el próximo juego."));
		};
		// Forja SX2 (2026-10-19): the language picker. At every start while Settings > "Preguntar el idioma al iniciar" says
		// Sí, and the first time (no language saved yet, the cursor on the PS4's own language). Not when the library comes back
		// from a game (main.cpp starts the app again for it and leaves this marker): that is the same session.
		bool returning = false;
		if (std::FILE* marker = std::fopen("/data/ForjaSX2/volver-biblioteca", "r"))
		{
			long long when = 0;
			returning = std::fscanf(marker, "%lld", &when) == 1 && std::llabs(static_cast<long long>(std::time(nullptr)) - when) < 300;
			std::fclose(marker);
			std::remove("/data/ForjaSX2/volver-biblioteca");
		}
		bool language_dialog_visible = !returning && (settings.ask_language || !settings.language[0]);
		if (returning)
			ProbeLog("Game menu: back from a game; the language is not asked again\n");
		unsigned language_focus = ForjaLang::Current();
		if (!settings.language[0])
		{
			int32_t system_language = 1;
			if (sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &system_language) == 0)
				language_focus = ForjaLang::FromSystemLanguage(system_language);
			ProbeLog("Game menu: no language saved; the PS4's is %d\n", system_language);
		}
		if (language_dialog_visible)
			PublishLanguageDialog(true, language_focus);
		ForjaStartupLogDone(); // compilation 69
		// Compilation 59: why the last game could not start (main.cpp writes it before the app opens the library again).
		if (std::FILE* reason = std::fopen("/data/ForjaSX2/ultimo-error.txt", "r"))
		{
			char text[512] = {};
			const size_t length = std::fread(text, 1, sizeof(text) - 1, reason);
			std::fclose(reason);
			std::remove("/data/ForjaSX2/ultimo-error.txt");
			std::string why(text, length);
			while (!why.empty() && (why.back() == '\n' || why.back() == '\r'))
				why.pop_back();
			// Compilation 67: main.cpp, ForjaCheckAudioTrial: the last start died opening its sound.
			if (why == "forja-audio-1")
				status = T("Forja se cerró al abrir el sonido del último juego. Ahora el sonido se abrirá en modo sencillo");
			else if (why == "forja-audio-2")
				status = T("Forja se volvió a cerrar al abrir el sonido: en esta consola los juegos irán sin sonido");
			else
				status = why.find("BIOS") != std::string::npos ?
				             T("El juego no pudo iniciar: falta la BIOS de la PS2. Cópiala por FTP a /data/ForjaSX2/bios") :
				             ForjaLang::F(TF("El juego no pudo iniciar: %s"), why.c_str());
			ProbeLog("Game menu: the last game could not start: %s\n", why.c_str());
		}
		DiscProbe disc_probe;
		DiscCoverDownload disc_cover;
		DiscCoverDownload library_cover;
		size_t library_cover_index = 0;
		std::string disc_status;
		bool boot_disc_when_ready = false;
		// Forja SX2 (compilation 41): the disc in the drive (its title) and its copy in roms/ if there is one: X plays the copy.
		std::string disc_title, disc_copy_path;
		double copy_status_at = 0.0;
		const auto pump_library_cover = [&]() {
			if (library_cover.Active())
				return;
			while (library_cover_index < library.size())
			{
				Item& item = library[library_cover_index++];
				if (item.cover || item.serial.empty())
					continue;
				if (library_cover.Start(item.serial, item.name, item.platform))
				{
					ProbeLog("Game menu: queued cover serial=%s title=%s\n", item.serial.c_str(), item.name.c_str());
					return;
				}
			}
		};
		// Forja SX2: the drive is read once the Disc view has stayed selected for a moment, not on every pass of the
		// sidebar over it. The app once closed in the library right after four reads in a row while scrolling past it.
		double disc_probe_at = 0.0;
		const auto probe_disc = [&](bool boot) {
			if (disc_probe.active) return;
			if (PS4DiscCopy::Busy())
			{
				// The drive is the copy's until it ends.
				items = {{disc_title.empty() ? T("Copiando el disco") : disc_title, {}, {}, false}};
				Publish(items, view);
				status = disc_status = PS4DiscCopy::StatusText();
				return;
			}
			disc_probe_at = 0.0;
			boot_disc_when_ready = boot;
			const bool started = disc_probe.Start();
			items = {{started ? T("Revisando el DVD...") : T("Falló la detección del disco"), {}, {}, false}};
			selected = 0;
			Publish(items, view);
			status = started ? T("Leyendo la unidad. Puedes seguir navegando.") : disc_probe.error;
			disc_status = status;
		};
		const auto set_view = [&](LibraryView next) {
			boot_disc_when_ready = false;
			view = next;
			items = BuildView(library, view, favorites, recent);
			selected = 0;
			Publish(items, view);
			status.clear();
			disc_probe_at = 0.0;
			ProbeLog("Game menu: view %u\n", static_cast<unsigned>(view));
			if (view == LibraryView::Disc)
			{
				if (PS4DiscCopy::Busy())
				{
					items = {{disc_title.empty() ? T("Copiando el disco") : disc_title, {}, {}, false}};
					Publish(items, view);
					status = disc_status = PS4DiscCopy::StatusText();
				}
				else if (!disc_probe.active)
				{
					disc_probe_at = timer.GetTimeSeconds() + 0.7;
					items = {{T("Revisando el DVD..."), {}, {}, false}};
					Publish(items, view);
				}
				else status = T("Revisando el DVD...");
			}
			else if (view == LibraryView::Settings)
			{
				settings = ForjaSettings::Load();
				settings_row = 0;
				PublishSettings(settings, settings_row);
			}
		};
		// A language picked (the picker, or Left/Right on Settings > Idioma): its texts from now on, everywhere.
		const auto apply_language = [&](unsigned index) {
			const bool loaded = ForjaLang::Use(index);
			ProbeLog("Game menu: language %s%s\n", ForjaLang::Languages[index].code, loaded ? "" : " (its file is missing: Spanish)");
			if (view != LibraryView::Disc)
				items = BuildView(library, view, favorites, recent);
			selected = std::min(selected, items.size() - 1);
			Publish(items, view);
			save_settings(loaded ? T("Idioma guardado.") : T("No se encontró el archivo de ese idioma: queda el español."));
		};
		// Library refreshes now begin a serialized cover queue. The UI remains
		// responsive while each native HTTPS request completes in its own thread.
		pump_library_cover();
		g_forja_menu_active.store(true, std::memory_order_relaxed);
		for (;;)
		{
			g_forja_menu_ticks.fetch_add(1, std::memory_order_relaxed); // watchdog heartbeat (main.cpp)
			const double now = timer.GetTimeSeconds();
			if (disc_probe_at > 0.0 && now >= disc_probe_at)
			{
				disc_probe_at = 0.0;
				if (view == LibraryView::Disc && !disc_probe.active)
					probe_disc(false);
			}
			// Forja SX2 (compilation 41): the disc copy's progress on the status line, and the library again once it ends.
			if (PS4DiscCopy::TakeFinished())
			{
				library = Scan();
				library_cover_index = 0;
				pump_library_cover();
				if (view != LibraryView::Disc)
				{
					items = BuildView(library, view, favorites, recent);
					selected = std::min(selected, items.empty() ? size_t{0} : items.size() - 1);
					Publish(items, view);
				}
				disc_copy_path = PS4DiscCopy::ResultPath();
				status = disc_status = PS4DiscCopy::StatusText();
			}
			else if (PS4DiscCopy::Busy() && now >= copy_status_at)
			{
				copy_status_at = now + 0.5;
				status = disc_status = PS4DiscCopy::StatusText();
			}
			if (library_cover.Collect())
			{
				for (Item& item : library)
				{
					if (item.serial == library_cover.Serial() && !item.cover)
						item.cover = LoadCover(item.name + ".iso");
				}
				items = BuildView(library, view, favorites, recent);
				Publish(items, view);
				pump_library_cover();
			}
			if (disc_cover.Collect() && view == LibraryView::Disc && !disc_probe.active && disc_probe.reader &&
				disc_probe.reader->GetInfo().serial == disc_cover.Serial() && items.size() == 1 && items[0].path == PS4Disc::DevicePath)
			{
				if (disc_cover.Succeeded())
				{
					items[0].cover = LoadCover(disc_cover.Serial() + ".iso");
					Publish(items, view);
				}
				status = disc_status;
				if (!disc_cover.Succeeded())
					status += T(" Portada no disponible. Triángulo para reintentar.");
			}
			if (disc_probe.Collect() && view == LibraryView::Disc)
			{
				if (disc_probe.reader)
				{
					const auto& disc = disc_probe.reader->GetInfo();
					const auto* game = GameDatabase::findGame(disc.serial);
					const std::string title = game ? std::string(game->name) : T("DVD de PS2 - ") + disc.serial;
					// Serial identifies the exact disc edition; the database title is a fallback.
					auto cover = LoadCover(disc.serial + ".iso");
					if (!cover && game && title.find_first_of("/\\") == std::string::npos)
						cover = LoadCover(title + ".iso");
					if (!cover && !boot_disc_when_ready && !disc_cover.Active())
						disc_cover.Start(disc.serial);
					items = {{title, PS4Disc::DevicePath, std::move(cover), false}};
					// Forja SX2 (compilation 41): a copy of this disc in roms/ (the same serial) is what X plays.
					disc_title = title;
					disc_copy_path.clear();
					for (const Item& item : library)
					{
						if (item.serial == disc.serial && item.path != PS4Disc::DevicePath)
						{
							disc_copy_path = item.path;
							break;
						}
					}
					status = disc.serial + (disc.dual_type ? T(" | DVD doble capa") : " | DVD") +
					         (disc_copy_path.empty() ? T(" | X: jugar desde el disco · □: copiarlo a la PS4") :
					                                   T(" | Ya está copiado en la PS4 · X: jugar la copia"));
					if (boot_disc_when_ready && !disc_copy_path.empty())
					{
						// The disc is the key: its copy starts, read from the console's storage (faster, and the drive rests).
						path = disc_copy_path;
						platform = GamePlatform::PS2;
						start = true;
						ProbeLog("Game menu: physical DVD %s is copied, starting %s\n", title.c_str(), path.c_str());
						break;
					}
					if (boot_disc_when_ready)
					{
						path = PS4Disc::DevicePath;
						platform = GamePlatform::PS2;
						PS4Disc::Prepare(std::move(disc_probe.reader));
						start = true;
						ProbeLog("Game menu: selected physical DVD %s\n", title.c_str());
						break;
					}
				}
				else
				{
					items = {{T("DVD no disponible"), {}, {}, false}};
					status = disc_probe.error;
				}
				boot_disc_when_ready = false;
				disc_status = status;
				if (disc_cover.Active())
					status += T(" Descargando portada...");
				selected = 0;
				Publish(items, view);
			}
			if (pad < 0 && now >= next_pad_open)
			{
				next_pad_open = now + 1.0;
				OrbisUserServiceUserId user = 0;
				if (sceUserServiceGetInitialUser(&user) == 0)
					pad = scePadOpen(user, 0, 0, nullptr);
			}
			OrbisPadData data{};
			const bool connected = pad >= 0 && scePadReadState(pad, &data) == 0 && data.connected;
			const u32 buttons = connected ? data.buttons : 0;
			const u32 pressed = buttons & ~previous;
			previous = buttons;
			// Forja SX2 (2026-10-19): the language picker, two columns: Up/Down in one, Left/Right to the other. X keeps the
			// language under the cursor; Circle leaves the one there was.
			if (language_dialog_visible)
			{
				static constexpr u32 directions[] = {ORBIS_PAD_BUTTON_UP, ORBIS_PAD_BUTTON_DOWN, ORBIS_PAD_BUTTON_LEFT, ORBIS_PAD_BUTTON_RIGHT};
				u32 dialog_navigation = 0;
				for (const u32 direction : directions)
					if (!dialog_navigation && (buttons & direction))
						dialog_navigation = direction;
				if (connected && !dialog_navigation)
					dialog_navigation = data.leftStick.y < 72 ? ORBIS_PAD_BUTTON_UP : data.leftStick.y > 184 ? ORBIS_PAD_BUTTON_DOWN :
					                    data.leftStick.x < 72 ? ORBIS_PAD_BUTTON_LEFT : data.leftStick.x > 184 ? ORBIS_PAD_BUTTON_RIGHT : 0;
				if (dialog_navigation && (dialog_navigation != held_direction || now >= next_repeat))
				{
					const unsigned row = language_focus % LanguageRowsPerColumn, column = language_focus / LanguageRowsPerColumn;
					if (dialog_navigation == ORBIS_PAD_BUTTON_UP && row > 0)
						language_focus--;
					else if (dialog_navigation == ORBIS_PAD_BUTTON_DOWN && row + 1 < LanguageRowsPerColumn &&
					         language_focus + 1 < ForjaLang::LanguageCount)
						language_focus++;
					else if (dialog_navigation == ORBIS_PAD_BUTTON_LEFT && column > 0)
						language_focus -= LanguageRowsPerColumn;
					else if (dialog_navigation == ORBIS_PAD_BUTTON_RIGHT && column == 0)
						language_focus = std::min(language_focus + LanguageRowsPerColumn, ForjaLang::LanguageCount - 1);
					next_repeat = now + (dialog_navigation == held_direction ? 0.12 : 0.35);
					PublishLanguageDialog(true, language_focus);
				}
				held_direction = dialog_navigation;
				if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE))
				{
					language_dialog_visible = false;
					PublishLanguageDialog(false, language_focus);
					if (pressed & ORBIS_PAD_BUTTON_CROSS)
						apply_language(language_focus);
					else
						status.clear();
				}
				Present(selected, status, connected, sidebar_focused, false, false);
				Threading::Sleep(16);
				continue;
			}
			// Forja SX2: the updater. A newer build than this one is offered once per app run; OPTIONS offers it again.
			if (!s_update_asked && !exit_dialog_visible && !language_dialog_visible && PS4Release::HasUpdate())
			{
				s_update_asked = true;
				update_dialog_visible = true;
				update_confirm = true;
			}
			if (update_dialog_visible)
			{
				if (pressed & ORBIS_PAD_BUTTON_LEFT || (connected && data.leftStick.x < 72))
					update_confirm = true;
				else if (pressed & ORBIS_PAD_BUTTON_RIGHT || (connected && data.leftStick.x > 184))
					update_confirm = false;
				if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
					update_dialog_visible = false;
				else if (pressed & ORBIS_PAD_BUTTON_CROSS)
				{
					if (update_confirm)
						PS4Release::StartDownload();
					update_dialog_visible = false;
				}
				Present(selected, status, connected, sidebar_focused, false, false, update_dialog_visible, update_confirm);
				Threading::Sleep(16);
				continue;
			}
			if ((pressed & ORBIS_PAD_BUTTON_OPTIONS) && PS4Release::HasUpdate() &&
				PS4Release::State() != PS4Release::DownloadState::Downloading && PS4Release::State() != PS4Release::DownloadState::Ready)
			{
				update_dialog_visible = true;
				update_confirm = true;
				Present(selected, status, connected, sidebar_focused, false, false, true, true);
				Threading::Sleep(16);
				continue;
			}
			if (exit_dialog_visible)
			{
				u32 dialog_navigation = 0;
				if (buttons & ORBIS_PAD_BUTTON_LEFT)
					dialog_navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (buttons & ORBIS_PAD_BUTTON_RIGHT)
					dialog_navigation = ORBIS_PAD_BUTTON_RIGHT;
				else if (connected && data.leftStick.x < 72)
					dialog_navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (connected && data.leftStick.x > 184)
					dialog_navigation = ORBIS_PAD_BUTTON_RIGHT;
				if (dialog_navigation && (dialog_navigation != held_direction || now >= next_repeat))
				{
					exit_confirm = dialog_navigation == ORBIS_PAD_BUTTON_LEFT;
					next_repeat = now + (dialog_navigation == held_direction ? 0.12 : 0.35);
				}
				held_direction = dialog_navigation;
				if (pressed & ORBIS_PAD_BUTTON_LEFT)
					exit_confirm = true;
				else if (pressed & ORBIS_PAD_BUTTON_RIGHT)
					exit_confirm = false;
				if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
					exit_dialog_visible = false;
				else if (pressed & ORBIS_PAD_BUTTON_CROSS)
				{
					if (exit_confirm)
						break;
					exit_dialog_visible = false;
				}
				Present(selected, status, connected, sidebar_focused, exit_dialog_visible, exit_confirm);
				Threading::Sleep(16);
				continue;
			}
			if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && view == LibraryView::Settings && !sidebar_focused)
			{
				// Forja SX2: Circle leaves the Settings list for the sidebar instead of asking to exit.
				sidebar_focused = true;
				Present(selected, status, connected, sidebar_focused, false, false);
				Threading::Sleep(16);
				continue;
			}
			if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
			{
				boot_disc_when_ready = false;
				exit_dialog_visible = true;
				exit_confirm = false;
				Present(selected, status, connected, sidebar_focused, true, false);
				Threading::Sleep(16);
				continue;
			}
			if ((pressed & ORBIS_PAD_BUTTON_TRIANGLE) && view == LibraryView::Disc)
				probe_disc(false);
			else if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
			{
				const std::string old_path = items[selected].path;
				library = Scan();
				library_cover_index = 0;
				items = BuildView(library, view, favorites, recent);
				pump_library_cover();
				const auto old_item = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.path == old_path; });
				selected = old_item == items.end() ? 0 : static_cast<size_t>(old_item - items.begin());
				Publish(items, view);
				status.clear();
			}
			if (pressed & (ORBIS_PAD_BUTTON_L1 | ORBIS_PAD_BUTTON_R1))
			{
				constexpr unsigned view_count = 5;
				unsigned next = static_cast<unsigned>(view);
				next = pressed & ORBIS_PAD_BUTTON_L1 ? (next + view_count - 1) % view_count : (next + 1) % view_count;
				set_view(static_cast<LibraryView>(next));
			}
			u32 navigation = 0;
			if (buttons & ORBIS_PAD_BUTTON_LEFT)
				navigation = ORBIS_PAD_BUTTON_LEFT;
			else if (buttons & ORBIS_PAD_BUTTON_RIGHT)
				navigation = ORBIS_PAD_BUTTON_RIGHT;
			else if (buttons & ORBIS_PAD_BUTTON_UP)
				navigation = ORBIS_PAD_BUTTON_UP;
			else if (buttons & ORBIS_PAD_BUTTON_DOWN)
				navigation = ORBIS_PAD_BUTTON_DOWN;
			if (connected && navigation == 0)
			{
				if (data.leftStick.x < 72)
					navigation = ORBIS_PAD_BUTTON_LEFT;
				else if (data.leftStick.x > 184)
					navigation = ORBIS_PAD_BUTTON_RIGHT;
				else if (data.leftStick.y < 72)
					navigation = ORBIS_PAD_BUTTON_UP;
				else if (data.leftStick.y > 184)
					navigation = ORBIS_PAD_BUTTON_DOWN;
			}
			if (navigation && (navigation != held_direction || now >= next_repeat))
			{
				if (sidebar_focused)
				{
					unsigned next = static_cast<unsigned>(view);
					if (navigation == ORBIS_PAD_BUTTON_UP && next > 0)
						set_view(static_cast<LibraryView>(next - 1));
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && next < 4)
						set_view(static_cast<LibraryView>(next + 1));
					else if (navigation == ORBIS_PAD_BUTTON_RIGHT)
						sidebar_focused = false;
				}
				else if (view == LibraryView::Settings)
				{
					if (navigation == ORBIS_PAD_BUTTON_UP && settings_row > 0)
						--settings_row;
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && settings_row + 1 < SettingsRows)
						++settings_row;
					else if ((navigation == ORBIS_PAD_BUTTON_LEFT || navigation == ORBIS_PAD_BUTTON_RIGHT) && settings_row == SET_LANGUAGE)
						apply_language((ForjaLang::Current() + (navigation == ORBIS_PAD_BUTTON_LEFT ? ForjaLang::LanguageCount - 1 : 1)) %
						               ForjaLang::LanguageCount);
					else if (navigation == ORBIS_PAD_BUTTON_LEFT)
						change_setting(-1, false);
					else if (navigation == ORBIS_PAD_BUTTON_RIGHT)
						change_setting(1, false);
					PublishSettings(settings, settings_row);
				}
				else
				{
					const size_t column = selected % GridColumns;
					const size_t row = selected / GridColumns;
					const size_t row_count = (items.size() + GridColumns - 1) / GridColumns;
					if (navigation == ORBIS_PAD_BUTTON_LEFT && column == 0)
						sidebar_focused = true;
					else if (navigation == ORBIS_PAD_BUTTON_LEFT)
						--selected;
					else if (navigation == ORBIS_PAD_BUTTON_RIGHT && column + 1 < GridColumns && selected + 1 < items.size())
						++selected;
					else if (navigation == ORBIS_PAD_BUTTON_UP && selected >= GridColumns)
						selected -= GridColumns;
					else if (navigation == ORBIS_PAD_BUTTON_DOWN && row + 1 < row_count)
						selected = std::min(selected + GridColumns, items.size() - 1);
				}
				next_repeat = now + (navigation == held_direction ? 0.10 : 0.35);
				if (view != LibraryView::Settings || sidebar_focused)
					status.clear();
			}
			held_direction = navigation;
			// Forja SX2 (compilation 41): Square on the Disc panel copies the disc to the PS4, or cancels the copy running.
			if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !sidebar_focused && view == LibraryView::Disc)
			{
				if (PS4DiscCopy::Busy())
				{
					PS4DiscCopy::Cancel();
					status = disc_status = T("Cancelando la copia...");
				}
				else if (!disc_copy_path.empty())
					status = T("Este disco ya está copiado en la PS4. X para jugar la copia.");
				else if (!disc_probe.active && disc_probe.reader)
				{
					std::string error;
					if (PS4DiscCopy::Start(std::move(disc_probe.reader), disc_title, error))
						status = disc_status = PS4DiscCopy::StatusText();
					else
						status = error;
				}
				else
					status = T("Pulsa Triángulo para leer el disco y después □ para copiarlo.");
			}
			if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !sidebar_focused && view != LibraryView::Disc)
			{
				const std::string selected_path = items[selected].path;
				if (!selected_path.empty())
				{
					const auto found = std::find(favorites.begin(), favorites.end(), selected_path);
					const bool removing = found != favorites.end();
					if (removing)
						favorites.erase(found);
					else
						favorites.push_back(selected_path);
					status = SavePathList(FavoritesFile, favorites) ?
					             (removing ? T("Quitado de favoritos.") : T("Agregado a favoritos.")) :
					             T("No se pudieron guardar los favoritos.");
					items = BuildView(library, view, favorites, recent);
					const auto same_item = std::find_if(items.begin(), items.end(), [&](const Item& item) { return item.path == selected_path; });
					selected = same_item == items.end() ? std::min(selected, items.size() - 1) : static_cast<size_t>(same_item - items.begin());
					Publish(items, view);
				}
			}
			if (pressed & ORBIS_PAD_BUTTON_CROSS)
			{
				if (sidebar_focused)
				{
					sidebar_focused = false;
					continue;
				}
				if (view == LibraryView::Settings)
				{
					// Forja SX2: X moves the selected setting to its next choice (and wraps around); on Idioma it opens the picker.
					if (settings_row == SET_LANGUAGE)
					{
						language_dialog_visible = true;
						language_focus = ForjaLang::Current();
						PublishLanguageDialog(true, language_focus);
					}
					else
						change_setting(1, true);
					Present(selected, status, connected, false, false, false);
					Threading::Sleep(16);
					continue;
				}
				if (PS4DiscCopy::Busy())
				{
					// Forja SX2 (compilation 41): no game while the drive and the storage are the copy's.
					status = T("Hay una copia del disco en curso: espera a que termine o cancélala con □ en Disco.");
					Present(selected, status, connected, false, false, false);
					Threading::Sleep(16);
					continue;
				}
				if (view == LibraryView::Disc)
				{
					// Re-probe before launch, so an ejected/replaced DVD cannot use old metadata.
					probe_disc(true);
					Present(selected, status, connected, false, false, false);
					Threading::Sleep(16);
					continue;
				}
				path = items[selected].path;
				struct stat info{};
				if (path.empty())
					status = view == LibraryView::Disc     ? T("El soporte de disco físico está en preparación.") :
					         view == LibraryView::Settings ? T("Aquí irán las opciones de ajustes.") :
					                                         T("No hay juegos en esta vista.");
				else if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
					status = T("Archivo no disponible. Pulsa Triángulo para actualizar.");
				else
				{
					// (AI-assisted) Keep an unreadable/unsupported image in the launcher
					// with an error, instead of tearing down the UI and exiting the app.
					InputIsoFile image;
					Error image_error;
					if (!image.Open(path, &image_error))
					{
						status = T("No se puede abrir el juego: ") + image_error.GetDescription();
						ProbeLog("Game menu: launch rejected %s: %s\n", path.c_str(), image_error.GetDescription().c_str());
						Present(selected, status, connected, false, false, false);
						Threading::Sleep(16);
						continue;
					}
					image.Close();
					recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
					recent.insert(recent.begin(), path);
					if (recent.size() > 24)
						recent.resize(24);
					if (!SavePathList(RecentFile, recent))
						ProbeLog("Game menu: could not save recent list\n");
					Present(selected, T("Iniciando..."), connected, false, false, false);
					// Consume the confirmation before handing the controller to the game.
					while (scePadReadState(pad, &data) == 0 && data.connected && (data.buttons & ORBIS_PAD_BUTTON_CROSS))
						Threading::Sleep(16);
					start = true;
					ProbeLog("Game menu: selected %s\n", path.c_str());
					platform = items[selected].platform;
					break;
				}
			}
			Present(selected, connected ? (view == LibraryView::Disc && status.empty() ? disc_status : status) : T("Conecta un control para elegir un juego."), connected,
				sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// Never destroy a live probe or leave a detached thread using menu state.
		disc_cover.Cancel();
		while (disc_cover.Active() && !disc_cover.Collect())
		{
			Present(selected, T("Terminando de bajar la portada..."), true, sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// The driver timeout is requested per command; hardware behavior needs proper testing.
		while (disc_probe.active && !disc_probe.Collect())
		{
			Present(selected, T("Terminando de detectar el disco..."), true, sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		if (start && path == PS4Disc::DevicePath)
		{
			OrbisPadData released{};
			while (scePadReadState(pad, &released) == 0 && released.connected && (released.buttons & ORBIS_PAD_BUTTON_CROSS))
				Threading::Sleep(16);
		}
		MTGS::RunOnGSThread([start]() {
			s_visible = false;
			s_sidebar_focused = false;
			s_exit_dialog_visible = false;
			s_exit_confirm = false;
			s_update_dialog_visible = false;
			s_language_dialog_visible = false;
			s_items.clear();
			s_status.clear();
			s_loading_visible = start;
			s_loading_carousel = false;
			s_loading_progress = start ? 10u : 0u;
			s_loading_status = start ? T("Preparando el emulador...") : "";
			ClearCoverTextures();
			if (start)
				GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
		g_forja_menu_active.store(false, std::memory_order_relaxed);
		return start;
	}

	bool Render()
	{
		ForjaLang::NewFrame();
		if (s_intro_visible)
		{
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 255));
			if (ImTextureData* frame = GetIntroTexture())
			{
				const float scale = std::min(size.x / static_cast<float>(frame->Width), size.y / static_cast<float>(frame->Height));
				const ImVec2 frame_size(frame->Width * scale, frame->Height * scale);
				const ImVec2 origin((size.x - frame_size.x) * 0.5f, (size.y - frame_size.y) * 0.5f);
				draw->AddImage(frame->GetTexRef(), origin, ImVec2(origin.x + frame_size.x, origin.y + frame_size.y));
			}
			return true;
		}
		if (s_loading_visible)
		{
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			ImFont* font = MenuFont();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
			const ImVec2 origin((size.x - 1920.0f * scale) * 0.5f, (size.y - 1080.0f * scale) * 0.5f);
			const auto point = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(2, 7, 15, 255));
			if (ImTextureData* background = GetCoverTexture(FindUiAsset("loading_background")))
				draw->AddImage(background->GetTexRef(), point(0, 0), point(1920, 1080));
			if (s_loading_carousel)
			{
				constexpr float travel = 2380.0f;
				constexpr float spacing = 520.0f;
				constexpr float art_size = 220.0f;
				for (unsigned index = 0; index < 4; index++)
				{
					const float x = std::fmod(s_loading_carousel_phase + static_cast<float>(index) * spacing, travel) - 230.0f;
					char name[32];
					std::snprintf(name, sizeof(name), "loading_carousel_%u", index + 1);
					if (ImTextureData* art = GetCoverTexture(FindUiAsset(name)))
						draw->AddImage(art->GetTexRef(), point(x, 475), point(x + art_size, 475 + art_size),
							ImVec2(0, 0), ImVec2(1, 1), IM_COL32(225, 240, 255, 235));
				}
			}

			// (AI-assisted) CyberTV-style loading bar: compact dark rail, neon glow,
			// blue/cyan fill, highlight and a real stage percentage. Needs proper testing
			// on a 720p console output in addition to the normal 1080p path.
			constexpr float bx = 630.0f, bw = 660.0f, bh = 14.0f;
			const float by = s_loading_carousel ? 748.0f : 674.0f;
			const float progress = static_cast<float>(std::min(s_loading_progress, 100u)) / 100.0f;
			const float fill_width = std::max(0.0f, (bw - 4.0f) * progress);
			for (int glow = 4; glow >= 1; --glow)
			{
				const float spread = static_cast<float>(glow) * 5.0f;
				draw->AddRectFilled(point(bx - spread, by - spread), point(bx + bw + spread, by + bh + spread),
					IM_COL32(0, 126, 255, 12 + (4 - glow) * 7), (7.0f + spread) * scale);
			}
			draw->AddRectFilled(point(bx, by), point(bx + bw, by + bh), IM_COL32(5, 18, 35, 245), 7.0f * scale);
			draw->AddRect(point(bx, by), point(bx + bw, by + bh), IM_COL32(49, 94, 143, 235), 7.0f * scale,
				1.0f * scale, 0);
			if (fill_width > 0.0f)
			{
				draw->AddRectFilledMultiColor(point(bx + 2, by + 2), point(bx + 2 + fill_width, by + bh - 2),
					IM_COL32(0, 88, 255, 255), IM_COL32(0, 220, 255, 255),
					IM_COL32(0, 178, 255, 255), IM_COL32(34, 126, 255, 255));
				if (fill_width > 8.0f)
					draw->AddRectFilled(point(bx + 4, by + 3), point(bx + fill_width - 2, by + 6),
						IM_COL32(220, 250, 255, 125), 1.5f * scale);
			}
			char percentage[16];
			std::snprintf(percentage, sizeof(percentage), "%u%%", std::min(s_loading_progress, 100u));
			const float percent_width = font->CalcTextSizeA(17.0f * scale, FLT_MAX, 0.0f, percentage).x / scale;
			draw->AddText(font, 17.0f * scale, point(960.0f - percent_width * 0.5f, by + 29),
				IM_COL32(190, 218, 241, 255), percentage);
			if (!s_loading_status.empty())
			{
				const float status_width = TextWidth(font, 18.0f * scale, s_loading_status.c_str()) / scale;
				DrawText(draw, font, 18.0f * scale, point(960.0f - status_width * 0.5f, by + 62), IM_COL32(213, 232, 249, 255),
					s_loading_status.c_str());
			}
			return true;
		}
		const auto menu_state = UnpackInGameMenuState(s_in_game_state.load(std::memory_order_acquire));
		if (menu_state.visible)
		{
			const unsigned selected_row = menu_state.selected;
			const unsigned max_upscale = menu_state.max_upscale;
			const unsigned upscale = menu_state.upscale;
			const unsigned aspect = menu_state.aspect;
			const bool show_performance_overlay = menu_state.show_overlay;
			const int ee_rate = menu_state.ee_rate;
			ImDrawList* draw = ImGui::GetBackgroundDrawList();
			ImFont* font = MenuFont();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
			const auto point = [scale](float x, float y) { return ImVec2(x * scale, y * scale); };
			const auto image = [&](const char* name, float x, float y, float w, float h) {
				if (ImTextureData* texture = GetCoverTexture(FindUiAsset(name)))
					draw->AddImage(texture->GetTexRef(), point(x, y), point(x + w, y + h));
			};
			const ImU32 white = IM_COL32(239, 246, 253, 255);
			const ImU32 muted = IM_COL32(145, 171, 193, 255);
			const ImU32 accent = IM_COL32(45, 153, 255, 255);
			const ImU32 panel = IM_COL32(7, 24, 40, 218);
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 5, 14, 158));
			draw->AddRectFilled(point(64, 38), point(1856, 1042), panel, 24.0f * scale);
			draw->AddRect(point(64, 38), point(1856, 1042), IM_COL32(36, 127, 204, 235), 24.0f * scale,
				2.0f * scale, 0);
			draw->AddRectFilled(point(64, 38), point(1856, 170), IM_COL32(4, 19, 34, 205), 24.0f * scale);
			draw->AddLine(point(64, 170), point(1856, 170), IM_COL32(38, 103, 157, 150), 1.0f * scale);
			image("status_controle", 126, 82, 40, 34);
			DrawText(draw, font, 34.0f * scale, point(196, 82), white, T("Menú del emulador"), 960.0f * scale);
			DrawText(draw, font, 18.0f * scale, point(1180, 66), muted, T("JUEGO EN PAUSA"), 640.0f * scale);
			// Forja SX2: which profiles this game has (pcsx2/ForjaGameProfile.h). The buffers are big for the translations
			// (2026-10-19): a Russian or Chinese letter takes two or three bytes.
			const auto yes_no = [](bool on) { return on ? T("Sí") : T("No"); };
			char profile_label[256];
			std::snprintf(profile_label, sizeof(profile_label), TF("Perfil de este juego:  de fábrica %s  ·  tuyo %s"),
				menu_state.has_factory ? T("sí") : T("no"), menu_state.has_player ? T("sí") : T("no"));
			DrawText(draw, font, 18.0f * scale, point(1180, 104), muted, profile_label, 640.0f * scale);
			static constexpr const char* origin_names[] = {N_("global"), N_("perfil de fábrica"), N_("tu perfil")};
			const auto origin = [&](unsigned index) { return T(origin_names[std::min(index, 2u)]); };
			char resolution[256];
			std::snprintf(resolution, sizeof(resolution), upscale == 1 ? TF("Resolución interna:  < %ux >  (nativa)") :
			                                                             TF("Resolución interna:  < %ux >"), upscale);
			static constexpr const char* aspect_names[] = {N_("Estirar"), N_("Auto"), "4:3", "16:9", "10:7"};
			char aspect_label[256];
			std::snprintf(aspect_label, sizeof(aspect_label), TF("Relación de aspecto:  < %s >"), T(aspect_names[std::min(aspect, 4u)]));
			char performance_label[256];
			std::snprintf(performance_label, sizeof(performance_label), TF("Contador de rendimiento:  < %s >"),
				yes_no(show_performance_overlay));
			// Forja SX2: los mismos porcentajes que muestra PCSX2 para EECycleRate -3..3; -4..-6 (compilación 36) son experimentales.
			static constexpr const char* ee_names[] = {N_("27 % (exp.)"), N_("31 % (exp.)"), N_("36 % (exp.)"), "50 %", "60 %", "75 %",
				N_("100 % (PS2 real)"), "130 %", "180 %", "300 %"};
			char ee_label[256];
			std::snprintf(ee_label, sizeof(ee_label), TF("Velocidad del CPU de la PS2:  < %s >"), T(ee_names[std::clamp(ee_rate, -6, 3) + 6]));
			// Forja SX2: the game's profile rows. Video mode: the EE rate while a movie plays (0 = off).
			static constexpr const char* video_names[] = {N_("No"), "100 %", "75 %", "60 %", "50 %", "36 %", N_("Auto")};
			char video_label[256];
			std::snprintf(video_label, sizeof(video_label), TF("Modo video:  < %s >"), T(video_names[std::min(menu_state.video, 6u)]));
			char video_description[512];
			std::snprintf(video_description, sizeof(video_description), TF("%s Para este juego  ·  ahora: %s"),
				menu_state.video == 6 ? T("Busca el CPU de la PS2 que da más cuadros en cada video (31 a 100 %).") :
										T("CPU de la PS2 durante las cinemáticas."),
				origin(menu_state.video_origin));
			// Forja SX2 (compilation 44): the picture's sharpness (main.cpp, ApplyGameSharpness).
			static constexpr const char* sharpness_names[] = {N_("Suave"), N_("Nítida"), "CAS", N_("CAS fuerte"), N_("Píxeles")};
			static constexpr const char* sharpness_descriptions[] = {
				N_("Como hasta ahora: la imagen escalada y suavizada. Se guarda para este juego"),
				N_("Bilineal nítido: bordes más definidos, sin costo. Se guarda para este juego"),
				N_("AMD CAS (de la familia del FSR): realza el detalle al agrandar la imagen. Cuesta muy poco"),
				N_("AMD CAS al máximo: aún más detalle; puede verse algo granulado"),
				N_("Sin filtro: cada píxel tal cual, estilo retro. Se guarda para este juego")};
			char sharpness_label[256];
			std::snprintf(sharpness_label, sizeof(sharpness_label), TF("Nitidez:  < %s >"), T(sharpness_names[std::min(menu_state.sharpness, 4u)]));
			// Forja SX2 (compilation 40): No, Sí (PS5SX2's, for a steady 30) and Fluida (every refresh the game leaves empty).
			static constexpr const char* frame_gen_names[] = {N_("No"), N_("Sí"), N_("Fluida")};
			char frame_gen_label[256];
			std::snprintf(frame_gen_label, sizeof(frame_gen_label), TF("Generación de cuadros:  < %s >"),
				T(frame_gen_names[std::min(menu_state.frame_gen, 2u)]));
			char frame_gen_description[512];
			std::snprintf(frame_gen_description, sizeof(frame_gen_description), TF("%s Para este juego  ·  ahora: %s"),
				menu_state.frame_gen >= 2 ? T("Rellena cada refresco que la tele repetiría (juegos PAL, cuadros desparejos).") :
											T("Muestra a 60 un juego estable a 30 (experimental)."),
				origin(menu_state.frame_gen_origin));
			// Forja SX2 (compilation 47): "Saltar cuadros" (main.cpp, UpdateGsSkip; GSRendererPS4.cpp, VSync). 1.06: back in
			// Rendimiento without "(experimental)", and "1 de 2" says it skips even where the GS needs no help.
			static constexpr const char* gs_skip_names[] = {N_("No"), N_("Auto"), N_("1 de 2")};
			char gs_skip_label[256];
			std::snprintf(gs_skip_label, sizeof(gs_skip_label), TF("Saltar cuadros (GS):  < %s >"), T(gs_skip_names[std::min(menu_state.gs_skip, 2u)]));
			char gs_skip_description[512];
			std::snprintf(gs_skip_description, sizeof(gs_skip_description),
				menu_state.gs_skip == 2 ? TF("Siempre 1 de cada 2 cuadros, aunque el GS no lo necesite: mejor en Auto  ·  ahora: %s  ·  %s") :
										  TF("Si el GS no da abasto, dibuja 1 de cada 2 cuadros: el juego va a su velocidad  ·  ahora: %s  ·  %s"),
				menu_state.gs_skip_active ? T("saltando") : T("dibuja todos"), origin(menu_state.gs_skip_origin));
			static constexpr const char* cores_names[] = {N_("Global"), N_("Normal"), N_("Plan B"), N_("Propio")};
			char cores_label[256];
			std::snprintf(cores_label, sizeof(cores_label), TF("Núcleos de la CPU:  < %s >"), T(cores_names[std::min(menu_state.cores, 3u)]));
			char cores_description[512];
			std::snprintf(cores_description, sizeof(cores_description),
				TF("Reparto de EE, VU1 y GS (Plan B: EE 1, VU1 2, GS 3). Para este juego  ·  ahora: %s"), origin(menu_state.cores_origin));
			// Forja SX2 (compilation 35): the automatic PS2 CPU speed (main.cpp, UpdateCpuAuto).
			static constexpr const char* cpu_auto_names[] = {N_("No"), N_("Hasta 75 %"), N_("Hasta 60 %"), N_("Hasta 50 %"),
				N_("Hasta 36 % (exp.)"), N_("Hasta 31 % (exp.)"), N_("Hasta 27 % (exp.)")};
			char cpu_auto_label[256];
			std::snprintf(cpu_auto_label, sizeof(cpu_auto_label), TF("CPU automático:  < %s >"), T(cpu_auto_names[std::min(menu_state.cpu_auto, 6u)]));
			char cpu_auto_description[512];
			if (menu_state.cpu_auto == 0)
				std::snprintf(cpu_auto_description, sizeof(cpu_auto_description), "%s",
					T("Baja el CPU de la PS2 solo en escenas pesadas: el juego va a su velocidad, con menos cuadros"));
			else
				std::snprintf(cpu_auto_description, sizeof(cpu_auto_description),
					TF("Solo en escenas pesadas, para este juego  ·  ahora: %s  ·  %s"),
					menu_state.cpu_auto_steps > 0 ? T(ee_names[std::clamp(ee_rate - static_cast<int>(menu_state.cpu_auto_steps), -6, 3) + 6]) :
													T("sin bajar"),
					origin(menu_state.cpu_auto_origin));
			// Forja SX2: save states (compilation 31). The slot is shared by the save and load rows.
			const unsigned slot = menu_state.slot + 1;
			char save_label[256], load_label[256], save_description[512], load_description[512];
			std::string state_date; // compilation 58: when the slot was saved
			{
				std::lock_guard lock(s_patch_text_mutex);
				state_date = s_state_date;
			}
			std::snprintf(save_label, sizeof(save_label), TF("Guardar estado:  < Ranura %u >"), slot);
			std::snprintf(load_label, sizeof(load_label), TF("Cargar estado:  < Ranura %u >"), slot);
			if (menu_state.action == STATE_SAVED)
				std::snprintf(save_description, sizeof(save_description), TF("Guardado en la ranura %u."), slot);
			else if (menu_state.action == STATE_SAVE_FAILED)
				std::snprintf(save_description, sizeof(save_description), TF("No se pudo guardar en la ranura %u."), slot);
			else if (menu_state.slot_used && !state_date.empty())
				std::snprintf(save_description, sizeof(save_description), TF("X: guarda aquí (reemplaza el del %s)"), state_date.c_str());
			else
				std::snprintf(save_description, sizeof(save_description), "%s",
					menu_state.slot_used ? T("X: guarda aquí (reemplaza lo que hay en esta ranura)") : T("X: guarda la partida en esta ranura (vacía)"));
			if (menu_state.action == STATE_EMPTY)
				std::snprintf(load_description, sizeof(load_description), TF("La ranura %u está vacía."), slot);
			else if (menu_state.action == STATE_LOAD_FAILED)
				std::snprintf(load_description, sizeof(load_description), TF("No se pudo cargar la ranura %u."), slot);
			else if (menu_state.action == STATE_LOAD_DAMAGED) // compilation 61
				std::snprintf(load_description, sizeof(load_description),
					TF("La ranura %u está dañada: rompía el juego y volviste a donde estabas. Guarda en otra ranura"), slot);
			else if (menu_state.slot_used && !state_date.empty())
				std::snprintf(load_description, sizeof(load_description), TF("Guardado el %s  ·  X: vuelve a ese momento y sigue jugando"),
					state_date.c_str());
			else
				std::snprintf(load_description, sizeof(load_description), "%s",
					menu_state.slot_used ? T("X: vuelve a ese momento y sigue jugando") : T("Ranura vacía"));
			// Each row's text by its name, not by its place: the rows moved into tabs in compilation 35.
			std::string row_labels[ROW_COUNT], row_descriptions[ROW_COUNT];
			{
				std::lock_guard lock(s_patch_text_mutex);
				for (unsigned r = 0; r < ROW_COUNT; r++)
				{
					row_labels[r] = s_row_labels[r];
					row_descriptions[r] = s_row_descriptions[r];
				}
			}
			const auto label_of = [&](unsigned row) -> const char* {
				if (row < ROW_COUNT && !row_labels[row].empty())
					return row_labels[row].c_str();
				switch (row)
				{
					case ROW_RESUME: return T("Volver al juego");
					case ROW_SAVE_STATE: return save_label;
					case ROW_LOAD_STATE: return load_label;
					case ROW_RESTART: return T("Reiniciar juego");
					case ROW_RESOLUTION: return resolution;
					case ROW_ASPECT: return aspect_label;
					case ROW_FRAME_GEN: return frame_gen_label;
					case ROW_SHARPNESS: return sharpness_label;
					case ROW_OVERLAY: return performance_label;
					case ROW_EE_RATE: return ee_label;
					case ROW_CPU_AUTO: return cpu_auto_label;
					case ROW_VIDEO_MODE: return video_label;
					case ROW_CORES: return cores_label;
					case ROW_GS_SKIP: return gs_skip_label;
					case ROW_RESET_PROFILE: return T("Restablecer ajustes de este juego");
					case ROW_LIBRARY: return T("Volver a la biblioteca");
					default: return T("Salir de la aplicación");
				}
			};
			const auto description_of = [&](unsigned row) -> const char* {
				if (row < ROW_COUNT && !row_labels[row].empty())
					return row_descriptions[row].c_str();
				switch (row)
				{
					case ROW_RESUME: return T("Seguir jugando desde donde quedó");
					case ROW_SAVE_STATE: return save_description;
					case ROW_LOAD_STATE: return load_description;
					case ROW_RESTART: return T("X: empieza el juego de nuevo sin cerrarlo (lo no guardado se pierde)");
					case ROW_RESOLUTION: return T("Más calidad de imagen (consume más). Se guarda para este juego");
					case ROW_ASPECT: return T("Forma de la imagen en pantalla. Se guarda para este juego");
					case ROW_FRAME_GEN: return frame_gen_description;
					case ROW_SHARPNESS: return T(sharpness_descriptions[std::min(menu_state.sharpness, 4u)]);
					case ROW_OVERLAY: return T("Mostrar u ocultar FPS, velocidad y tiempos");
					case ROW_EE_RATE: return T("Menos % = más FPS en la PS4 (el juego puede perder cuadros). Se guarda para este juego");
					case ROW_CPU_AUTO: return cpu_auto_description;
					case ROW_VIDEO_MODE: return video_description;
					case ROW_CORES: return cores_description;
					case ROW_GS_SKIP: return gs_skip_description;
					case ROW_RESET_PROFILE:
						return menu_state.has_player ? T("X: borra tus cambios de este juego (queda copia .bak). Siguen el perfil de fábrica y los globales") :
													   T("Este juego no tiene cambios tuyos");
					case ROW_LIBRARY: return T("Cerrar el juego y volver a la biblioteca");
					default: return T("Cerrar el juego y salir de Forja SX2");
				}
			};
			// Icons in a 32 px circle; the shapes are drawn on a +-16 grid.
			const auto draw_option_icon = [&](unsigned row, float cx, float cy, bool selected) {
				const ImU32 icon_bg = selected ? IM_COL32(34, 127, 214, 255) : IM_COL32(16, 55, 86, 255);
				const ImU32 icon_line = selected ? IM_COL32(231, 247, 255, 255) : IM_COL32(155, 204, 235, 255);
				const auto ip = [&](float dx, float dy) { return point(cx + dx, cy + dy); };
				const float lw = 2.5f * scale;
				draw->AddCircleFilled(point(cx, cy), 32.0f * scale, icon_bg, 40);
				draw->AddCircle(point(cx, cy), 32.0f * scale, selected ? accent : IM_COL32(49, 111, 155, 255), 40, 1.5f * scale);
				switch (row)
				{
					case ROW_RESUME:
						draw->AddTriangleFilled(ip(-7, -11), ip(-7, 11), ip(13, 0), icon_line);
						break;
					case ROW_SAVE_STATE: // a floppy disk
						draw->AddRect(ip(-13, -13), ip(13, 13), icon_line, 2 * scale, lw, 0);
						draw->AddRectFilled(ip(-7, -13), ip(7, -5), icon_line);
						draw->AddRect(ip(-8, 3), ip(8, 13), icon_line, 0, lw, 0);
						break;
					case ROW_LOAD_STATE: // an arrow out of a tray
						draw->AddLine(ip(0, 6), ip(0, -12), icon_line, lw);
						draw->AddLine(ip(0, -12), ip(-7, -5), icon_line, lw);
						draw->AddLine(ip(0, -12), ip(7, -5), icon_line, lw);
						draw->AddLine(ip(-13, 4), ip(-13, 13), icon_line, lw);
						draw->AddLine(ip(-13, 13), ip(13, 13), icon_line, lw);
						draw->AddLine(ip(13, 13), ip(13, 4), icon_line, lw);
						break;
					case ROW_RESTART: // a circular arrow
						draw->PathArcTo(point(cx, cy), 13.0f * scale, 0.6f, 5.6f, 24);
						draw->PathStroke(icon_line, lw);
						draw->AddTriangleFilled(ip(9, -13), ip(17, -3), ip(6, -2), icon_line);
						break;
					case ROW_RESOLUTION:
						draw->AddRect(ip(-13, -13), ip(13, 13), icon_line, 3 * scale, lw, 0);
						draw->AddLine(ip(-7, 6), ip(7, 6), icon_line, lw);
						break;
					case ROW_ASPECT:
						draw->AddRect(ip(-15, -10), ip(15, 10), icon_line, 3 * scale, lw, 0);
						draw->AddLine(ip(-8, -15), ip(8, -15), icon_line, lw);
						break;
					case ROW_OVERLAY:
						draw->AddRect(ip(-15, -9), ip(15, 9), icon_line, 3 * scale, lw, 0);
						draw->AddLine(ip(-9, 0), ip(9, 0), icon_line, lw);
						break;
					case ROW_EE_RATE:
						draw->AddCircle(ip(0, 4), 14.0f * scale, icon_line, 28, lw);
						draw->AddLine(ip(0, 4), ip(8, -6), icon_line, lw);
						break;
					case ROW_SHARPNESS: // a diamond
						draw->AddLine(ip(0, -15), ip(14, 0), icon_line, lw);
						draw->AddLine(ip(14, 0), ip(0, 15), icon_line, lw);
						draw->AddLine(ip(0, 15), ip(-14, 0), icon_line, lw);
						draw->AddLine(ip(-14, 0), ip(0, -15), icon_line, lw);
						break;
					case ROW_VU1_FILTER: // a funnel
						draw->AddLine(ip(-15, -12), ip(15, -12), icon_line, lw);
						draw->AddLine(ip(-15, -12), ip(-3, 2), icon_line, lw);
						draw->AddLine(ip(15, -12), ip(3, 2), icon_line, lw);
						draw->AddLine(ip(-3, 2), ip(-3, 14), icon_line, lw);
						draw->AddLine(ip(3, 2), ip(3, 14), icon_line, lw);
						break;
					case ROW_FAST_FORWARD: // two arrows (compilation 58)
						draw->AddTriangle(ip(-15, -10), ip(-2, 0), ip(-15, 10), icon_line, lw);
						draw->AddTriangle(ip(0, -10), ip(13, 0), ip(0, 10), icon_line, lw);
						break;
					case ROW_CHEATS: // a lightning bolt (compilation 58)
						draw->AddLine(ip(4, -15), ip(-8, 2), icon_line, lw);
						draw->AddLine(ip(-8, 2), ip(4, 2), icon_line, lw);
						draw->AddLine(ip(4, 2), ip(-4, 15), icon_line, lw);
						break;
					case ROW_GS_FILTER: // a picture crossed out (compilation 51)
						draw->AddRect(ip(-15, -11), ip(15, 11), icon_line, 3 * scale, lw, 0);
						draw->AddLine(ip(-11, 8), ip(11, -8), icon_line, lw);
						break;
					case ROW_TEXTURES: // a picture
						draw->AddRect(ip(-15, -12), ip(15, 12), icon_line, 3 * scale, lw, 0);
						draw->AddLine(ip(-11, 8), ip(-3, -2), icon_line, lw);
						draw->AddLine(ip(-3, -2), ip(4, 6), icon_line, lw);
						draw->AddLine(ip(4, 6), ip(11, 0), icon_line, lw);
						break;
					case ROW_RECOMMENDED: // a cloud's arrow down
						draw->AddLine(ip(0, -14), ip(0, 10), icon_line, lw);
						draw->AddLine(ip(-8, 2), ip(0, 10), icon_line, lw);
						draw->AddLine(ip(8, 2), ip(0, 10), icon_line, lw);
						draw->AddLine(ip(-14, 15), ip(14, 15), icon_line, lw);
						break;
					case ROW_CHANGE_DISC: // a disc
						draw->AddCircle(ip(0, 0), 14.0f * scale, icon_line, 28, lw);
						draw->AddCircle(ip(0, 0), 4.0f * scale, icon_line, 12, lw);
						break;
					case ROW_PATCH: // a bandage
						draw->AddRect(ip(-15, -7), ip(15, 7), icon_line, 7 * scale, lw, 0);
						draw->AddLine(ip(-4, -7), ip(-4, 7), icon_line, lw);
						draw->AddLine(ip(4, -7), ip(4, 7), icon_line, lw);
						break;
					case ROW_CPU_AUTO: // the gauge with an arrow going down and up
						draw->AddCircle(ip(0, 4), 14.0f * scale, icon_line, 28, lw);
						draw->AddLine(ip(0, 4), ip(-8, -6), icon_line, lw);
						draw->AddTriangleFilled(ip(10, -14), ip(16, -6), ip(4, -6), icon_line);
						break;
					case ROW_VIDEO_MODE:
						draw->AddRect(ip(-15, -11), ip(15, 11), icon_line, 3 * scale, lw, 0);
						draw->AddTriangleFilled(ip(-5, -6), ip(-5, 6), ip(7, 0), icon_line);
						break;
					case ROW_FRAME_GEN:
						draw->AddRect(ip(-15, -12), ip(5, 6), icon_line, 2 * scale, lw, 0);
						draw->AddRect(ip(-5, -6), ip(15, 12), icon_line, 2 * scale, lw, 0);
						break;
					case ROW_GS_SKIP: // two frames, the second one crossed out
						draw->AddRect(ip(-16, -9), ip(-2, 9), icon_line, 2 * scale, lw, 0);
						draw->AddRect(ip(2, -9), ip(16, 9), icon_line, 2 * scale, lw, 0);
						draw->AddLine(ip(2, 9), ip(16, -9), icon_line, lw);
						break;
					case ROW_CORES:
						for (int i = 0; i < 4; i++)
						{
							const float x = (i & 1) ? 2.0f : -14.0f, y = (i & 2) ? 2.0f : -14.0f;
							draw->AddRect(ip(x, y), ip(x + 12, y + 12), icon_line, 2 * scale, lw, 0);
						}
						break;
					case ROW_RESET_PROFILE: // sliders back at their start
						draw->AddLine(ip(-13, -7), ip(13, -7), icon_line, lw);
						draw->AddLine(ip(-13, 7), ip(13, 7), icon_line, lw);
						draw->AddCircleFilled(ip(-7, -7), 4.5f * scale, icon_line, 12);
						draw->AddCircleFilled(ip(-7, 7), 4.5f * scale, icon_line, 12);
						break;
					case ROW_LIBRARY:
						draw->AddLine(ip(-12, 0), ip(10, 0), icon_line, lw);
						draw->AddLine(ip(-12, 0), ip(-2, -9), icon_line, lw);
						draw->AddLine(ip(-12, 0), ip(-2, 9), icon_line, lw);
						break;
					default:
						draw->AddLine(ip(-10, -12), ip(10, 12), icon_line, lw);
						draw->AddLine(ip(10, -12), ip(-10, 12), icon_line, lw);
						break;
				}
			};
			// Forja SX2 (compilation 35): the rows in four tabs, as Dolphin's quick menu (Joseph, 2026-10-16: one list of fifteen
			// rows was hard to follow). L1/R1 move between the tabs (main.cpp); each tab fits without scrolling.
			static constexpr const char* tab_names[TAB_COUNT] = {N_("Partida"), N_("Imagen"), N_("Rendimiento"), N_("Extras"),
				N_("No tocar"), N_("Sistema")};
			const unsigned active_tab = TabOfRow(selected_row);
			draw->AddText(font, 20.0f * scale, point(112, 198), muted, "L1");
			// The tabs shrink together when a language's names don't fit between L1 and R1 (2026-10-19).
			float tabs_w = 0.0f;
			for (unsigned tab = 0; tab < TAB_COUNT; tab++)
				tabs_w += TextWidth(font, 26.0f * scale, T(tab_names[tab])) / scale + 64.0f + 16.0f;
			const float tab_fit = std::min(1.0f, (1760.0f - 152.0f) / tabs_w);
			const float tab_pixels = std::floor(26.0f * tab_fit * scale);
			float tab_x = 152.0f;
			for (unsigned tab = 0; tab < TAB_COUNT; tab++)
			{
				const float text_w = TextWidth(font, tab_pixels, T(tab_names[tab])) / scale;
				const float w = text_w + 64.0f * tab_fit;
				const bool on = tab == active_tab;
				draw->AddRectFilled(point(tab_x, 186), point(tab_x + w, 238), on ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 255),
					26.0f * scale);
				if (on)
					draw->AddRect(point(tab_x, 186), point(tab_x + w, 238), accent, 26.0f * scale, 2.0f * scale, 0);
				DrawText(draw, font, tab_pixels, point(tab_x + 32.0f * tab_fit, 196 + (26.0f * scale - tab_pixels) * 0.5f / scale),
					on ? white : muted, T(tab_names[tab]));
				tab_x += w + 16.0f * tab_fit;
			}
			draw->AddText(font, 20.0f * scale, point(tab_x + 8, 198), muted, "R1");
			// Compilation 53: the labs' tab says what it is.
			if (active_tab == TAB_LAB)
				DrawText(draw, font, 18.0f * scale, point(112, 244), muted,
					T("Experimentos para medir y mejorar los juegos (en prueba). Si no sabes qué hace algo, déjalo como está."), 1690.0f * scale);
			constexpr float ROW_TOP = 270.0f, ROW_PITCH = 112.0f, ROW_HEIGHT = 100.0f;
			for (unsigned row = TAB_FIRST_ROW[active_tab]; row < TAB_FIRST_ROW[active_tab + 1]; row++)
			{
				const float y = ROW_TOP + static_cast<float>(row - TAB_FIRST_ROW[active_tab]) * ROW_PITCH;
				const bool selected = selected_row == row;
				draw->AddRectFilled(point(112, y), point(1808, y + ROW_HEIGHT),
					selected ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 255), 14.0f * scale);
				if (selected)
					draw->AddRect(point(112, y), point(1808, y + ROW_HEIGHT), accent, 14.0f * scale, 2.5f * scale, 0);
				draw_option_icon(row, 176, y + ROW_HEIGHT * 0.5f, selected);
				const char* action = nullptr;
				if (row == ROW_RESOLUTION || row == ROW_EE_RATE)
					action = T("AJUSTAR");
				else if (row == ROW_ASPECT || row == ROW_VIDEO_MODE || row == ROW_CORES || row == ROW_CPU_AUTO || row == ROW_FRAME_GEN ||
				         row == ROW_SHARPNESS || row == ROW_GS_SKIP)
					action = T("ELEGIR");
				else if (row == ROW_VU1_FILTER || row == ROW_GS_FILTER)
					action = T("ELEGIR  ·  X  ·  CUADRADO"); // compilation 52: Cuadrado brings everything back
				else if (row == ROW_PATCH || row == ROW_CHEATS || row == ROW_CHANGE_DISC)
					action = T("ELEGIR  ·  X");
				else if (row == ROW_FAST_FORWARD) // compilation 58
					action = "X";
				else if (row == ROW_TEXTURES)
					action = T("ELEGIR");
				else if (row == ROW_RECOMMENDED)
					action = "X";
				else if (row == ROW_OVERLAY)
					action = T("CAMBIAR");
				else if (row == ROW_SAVE_STATE || (row == ROW_LOAD_STATE && menu_state.slot_used))
					action = T("X  ·  RANURA");
				else if (row == ROW_RESTART || (row == ROW_RESET_PROFILE && menu_state.has_player))
					action = "X";
				// 2026-10-19: the action ends at the row's right edge, so a longer word in another language stays inside it and the
				// texts on the left get the room left.
				float text_right = 1780.0f;
				if (action)
				{
					const float action_w = std::min(TextWidth(font, 24.0f * scale, action) / scale, 420.0f);
					DrawText(draw, font, 24.0f * scale, point(1780.0f - action_w, y + 36), selected ? white : muted, action, 420.0f * scale);
					text_right = 1780.0f - action_w - 28.0f;
				}
				DrawText(draw, font, 32.0f * scale, point(240, y + 12), white, label_of(row), (text_right - 240.0f) * scale);
				DrawText(draw, font, 20.0f * scale, point(240, y + 60), muted, description_of(row), (text_right - 240.0f) * scale);
			}
			char hint[512];
			std::snprintf(hint, sizeof(hint),
				TF("Resolución: 1x-%ux (3x o más es experimental y solo para PS4 Pro).   Lo que cambias en Imagen y Rendimiento se guarda solo para este juego."),
				max_upscale);
			DrawText(draw, font, 20.0f * scale, point(138, 940), muted,
				T("L1 / R1: cambiar de pestaña     Arriba/Abajo: elegir     Izq./Der.: cambiar valor o ranura"), 1660.0f * scale);
			DrawText(draw, font, 17.0f * scale, point(138, 972), muted, hint, 1660.0f * scale);
			image("controle_x", 1480, 998, 30, 30);
			DrawText(draw, font, 21.0f * scale, point(1522, 1001), white, T("Aceptar"), 160.0f * scale);
			image("controle_bola", 1690, 998, 30, 30);
			DrawText(draw, font, 21.0f * scale, point(1732, 1001), muted, T("Volver"), 110.0f * scale);
			return true;
		}
		if (!s_visible || s_items.empty())
			return false;
		// (AI-assisted) The launcher is drawn by the same Vulkan/ImGui device as
		// the emulator. Keep this path free of file I/O so a library refresh never
		// stalls the GS thread. Cover files will plug into the art slot later.
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		ImFont* font = MenuFont();
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
		const ImVec2 origin((size.x - 1920 * scale) * 0.5f, (size.y - 1080 * scale) * 0.5f);
		const auto point = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
		const auto fill = [&](float x, float y, float w, float h, ImU32 color, float radius = 12.0f) {
			draw->AddRectFilled(point(x, y), point(x + w, y + h), color, radius * scale);
		};
		// 2026-10-19: a width (in 1080p units) fits a longer translation in its place (DrawText).
		const auto text = [&](float x, float baseline, float pixels, ImU32 color, const char* value, float max_width = 0.0f) {
			DrawText(draw, font, pixels * scale, point(x, baseline - pixels * 0.82f), color, value, max_width * scale);
		};
		const auto text_width = [&](float pixels, const char* value) { return TextWidth(font, pixels * scale, value) / scale; };
		const auto centered_text = [&](float center_x, float baseline, float pixels, ImU32 color, const char* value, float max_width = 0.0f) {
			const float width = max_width > 0.0f ? std::min(text_width(pixels, value), max_width) : text_width(pixels, value);
			text(center_x - width * 0.5f, baseline, pixels, color, value, max_width);
		};
		const auto image = [&](const char* name, float x, float y, float w, float h) {
			if (ImTextureData* texture = GetCoverTexture(FindUiAsset(name)))
				draw->AddImage(texture->GetTexRef(), point(x, y), point(x + w, y + h));
		};
		const ImU32 white = IM_COL32(239, 246, 253, 255);
		const ImU32 muted = IM_COL32(145, 171, 193, 255);
		const ImU32 faint = IM_COL32(82, 117, 145, 255);
		const ImU32 accent = IM_COL32(45, 153, 255, 255);
		const ImU32 card_bg = IM_COL32(12, 33, 52, 255);
		const ImU32 art_a[] = {IM_COL32(29, 76, 116, 255), IM_COL32(92, 49, 58, 255),
			IM_COL32(104, 58, 35, 255), IM_COL32(38, 91, 87, 255), IM_COL32(72, 58, 120, 255)};
		const ImU32 art_b[] = {IM_COL32(8, 25, 46, 255), IM_COL32(23, 37, 63, 255),
			IM_COL32(33, 24, 47, 255), IM_COL32(14, 42, 46, 255), IM_COL32(25, 25, 67, 255)};
		draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(3, 13, 25, 255));
		// Layered navy background and restrained diagonal lighting give the menu
		// depth without adding a texture or a frame-time allocation.
		draw->AddRectFilledMultiColor(point(0, 0), point(1920, 1080), IM_COL32(9, 31, 55, 255),
			IM_COL32(2, 14, 29, 255), IM_COL32(4, 18, 34, 255), IM_COL32(1, 10, 21, 255));
		for (int i = -4; i < 9; ++i)
			draw->AddLine(point(266 + i * 108, 0), point(940 + i * 108, 1080), IM_COL32(22, 71, 119, 38), 1.0f * scale);
		draw->AddCircle(point(1680, 130), 390.0f * scale, IM_COL32(22, 92, 151, 28), 72, 2.0f * scale);
		draw->AddCircle(point(1680, 130), 310.0f * scale, IM_COL32(17, 79, 142, 24), 72, 2.0f * scale);
		fill(0, 0, 286, 1080, IM_COL32(3, 15, 28, 244), 0);
		fill(286, 0, 1634, 92, IM_COL32(4, 19, 34, 220), 0);
		draw->AddLine(point(286, 92), point(1920, 92), IM_COL32(38, 103, 157, 110), 1.0f * scale);
		image("menu_logo", 32, 0, 220, 73);
		centered_text(142, 82, 11, muted, T("EMULADOR DE PLAYSTATION 2"), 250);
		const size_t selected = std::min(s_selected, s_items.size() - 1);
		const Item& current = s_items[selected];
		text(348, 59, 28, white, current.name.c_str(), 1430);
		// Forja SX2: the updater's line (a newer build, its download's progress, or where the package was left).
		if (const std::string update_text = PS4Release::StatusText(); !update_text.empty())
		{
			const float text_w = text_width(16, update_text.c_str());
			const float box_w = std::min(text_w + 44.0f, 1000.0f), box_x = 1888.0f - box_w;
			fill(box_x, 22, box_w, 50, IM_COL32(8, 54, 91, 235), 10);
			draw->AddRect(point(box_x, 22), point(1888, 72), IM_COL32(42, 163, 255, 225), 10 * scale, 1.0f * scale, 0);
			text(box_x + 22, 53, 16, white, update_text.c_str(), box_w - 44.0f);
		}
		// The sidebar changes the data source while retaining the same grid.
		static constexpr const char* sidebar_icons[] = {
			"menu_biblioteca", "menu_disc", "menu_recentes", "menu_favoritos", "menu_configuracoes"};
		static constexpr const char* sidebar_labels[] = {N_("Biblioteca"), N_("Disco"), N_("Recientes"), N_("Favoritos"), N_("Ajustes")};
		const unsigned active_view = static_cast<unsigned>(s_library_view);
		for (unsigned i = 0; i < 5; ++i)
		{
			const float y = 164.0f + static_cast<float>(i) * 72.0f;
			const bool active = i == active_view;
			if (active)
			{
				fill(24, y, 228, 58, s_sidebar_focused ? IM_COL32(19, 91, 158, 255) : IM_COL32(13, 62, 109, 225), 10);
				draw->AddRect(point(24, y), point(252, y + 58), s_sidebar_focused ? IM_COL32(47, 164, 255, 255) : IM_COL32(35, 116, 181, 210),
					10 * scale, s_sidebar_focused ? 2.0f * scale : 1.0f * scale, 0);
			}
			image(sidebar_icons[i], 43, y + 12, 34, 34);
			text(91, y + 37, active ? 19 : 18, active ? white : muted, T(sidebar_labels[i]), 155);
		}
		// Forja SX2: the public version (VERSION) and, in brackets, the build, so a tester sees at a glance whether the new
		// package is the one installed.
		{
			char build_text[128];
			if (FORJA_BUILD_NUMBER > 0)
				std::snprintf(build_text, sizeof(build_text), TF("Versión %d.%02d (%d)"), FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR,
					FORJA_BUILD_NUMBER);
			else
				std::snprintf(build_text, sizeof(build_text), TF("Versión %d.%02d (local)"), FORJA_VERSION_MAJOR, FORJA_VERSION_MINOR);
			text(43, 1023, 16, muted, build_text, 230);
		}
		const size_t total_rows = (s_items.size() + GridColumns - 1) / GridColumns;
		const size_t selected_row = selected / GridColumns;
		const size_t max_first_row = total_rows > VisibleRows ? total_rows - VisibleRows : 0;
		const size_t first_row = std::min(selected_row >= VisibleRows ? selected_row - (VisibleRows - 1) : 0, max_first_row);
		const size_t first = first_row * GridColumns;
		constexpr float card_w = 270, card_h = 382;
		constexpr float grid_x = 346, grid_y = 120, column_step = 372, row_step = 400;
		const bool disc_view = s_library_view == LibraryView::Disc;
		if (disc_view)
		{
			const bool detected = current.path == PS4Disc::DevicePath;
			if (ImTextureData* cover = GetCoverTexture(current.cover); detected && cover)
			{
				constexpr float x = 620, y = 230, w = 340, h = 480;
				fill(x - 10, y - 10, w + 20, h + 20, IM_COL32(10, 37, 67, 210), 14);
				fill(x, y, w, h, card_bg, 9);
				draw->AddRect(point(x - 3, y - 3), point(x + w + 3, y + h + 3), accent, 10 * scale, 3.0f * scale, 0);
				const float fit_scale = std::min((w - 4) / cover->Width, (h - 4) / cover->Height);
				const float image_w = cover->Width * fit_scale, image_h = cover->Height * fit_scale;
				const float image_x = x + (w - image_w) * 0.5f, image_y = y + (h - image_h) * 0.5f;
				draw->AddImage(cover->GetTexRef(), point(image_x, image_y), point(image_x + image_w, image_y + image_h));
			}
			else
				image("menu_disc", 810, 280, 180, 180);
			text(1030, 340, 30, white, T("DVD de PS2"), 790);
			if (PS4DiscCopy::Busy())
			{
				// Forja SX2 (compilation 41): the progress is on the status line below.
				text(1030, 390, 20, muted, T("Copiando el disco a la PS4. Puedes seguir en la biblioteca."), 790);
				text(1030, 424, 18, muted, T("□ cancela la copia. No saques el disco hasta que termine."), 790);
			}
			else
			{
				text(1030, 390, 20, muted, detected ? T("Disco detectado. X para jugar · □ para copiarlo a la PS4.") :
				                                      T("Mete un DVD y pulsa Triángulo para actualizar."), 790);
				if (detected)
					text(1030, 424, 18, muted, T("Copiado una vez, el disco abre su copia: más rápida y sin ruido de la unidad."), 790);
			}
		}
		// Forja SX2: the Settings page, one row per choice with its value on the right. 2026-10-19 (Joseph: "ya está muy chiquito
		// las letras"): bigger letters, the value on the name's line and the help under both, eight rows on screen in a list that
		// scrolls with the cursor; the bar on the right and "3 / 14" say where the page is.
		const bool settings_view = s_library_view == LibraryView::Settings;
		if (settings_view)
		{
			constexpr unsigned visible_rows = 8;
			constexpr float rows_top = 110.0f, row_pitch = 100.0f, row_height = 92.0f;
			const unsigned focused_row = std::min(s_settings_row_gs, SettingsRows - 1);
			const unsigned first_row = std::min(focused_row > 3 ? focused_row - 3 : 0u, SettingsRows - visible_rows);
			for (unsigned slot = 0; slot < visible_rows && first_row + slot < SettingsRows; ++slot)
			{
				const unsigned row = first_row + slot;
				const float y = rows_top + static_cast<float>(slot) * row_pitch;
				const bool focused = !s_sidebar_focused && row == focused_row;
				fill(346, y, 1450, row_height, focused ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 235), 10);
				if (focused)
					draw->AddRect(point(346, y), point(1796, y + row_height), accent, 10 * scale, 2.0f * scale, 0);
				char value[128];
				SettingValue(s_settings_gs, row, value, sizeof(value));
				char shown[160];
				std::snprintf(shown, sizeof(shown), focused ? "<  %s  >" : "%s", value);
				const float value_w = std::min(text_width(28, shown), 640.0f);
				text(1766 - value_w, y + 42, 28, focused ? white : accent, shown, 640);
				text(380, y + 42, 28, white, SettingLabel(row), 1766 - value_w - 40 - 380);
				text(380, y + 76, 20, muted, SettingHelp(row), 1386);
			}
			constexpr float track_h = visible_rows * row_pitch - (row_pitch - row_height);
			const float thumb_h = track_h * static_cast<float>(visible_rows) / static_cast<float>(SettingsRows);
			const float thumb_y = rows_top + (track_h - thumb_h) * static_cast<float>(first_row) / static_cast<float>(SettingsRows - visible_rows);
			fill(1810, rows_top, 6, track_h, IM_COL32(30, 66, 94, 180), 3);
			fill(1810, thumb_y, 6, thumb_h, accent, 3);
			char where[24];
			std::snprintf(where, sizeof(where), "%u / %u", focused_row + 1, static_cast<unsigned>(SettingsRows));
			text(1796 - text_width(18, where), 1023, 18, muted, where);
		}
		for (size_t slot = 0; !disc_view && !settings_view && slot < VisibleItems && first + slot < s_items.size(); ++slot)
		{
			const size_t index = first + slot;
			const float x = grid_x + static_cast<float>(slot % GridColumns) * column_step;
			const float y = grid_y + static_cast<float>(slot / GridColumns) * row_step;
			const bool focused = index == selected;
			fill(x - (focused ? 11 : 7), y - (focused ? 11 : 7), card_w + (focused ? 22 : 14),
				card_h + (focused ? 24 : 14), focused ? IM_COL32(10, 37, 67, 210) : IM_COL32(5, 17, 30, 235), 14);
			fill(x, y, card_w, card_h, focused ? IM_COL32(18, 53, 86, 255) : card_bg, 9);
			if (focused)
			{
				draw->AddRect(point(x - 3, y - 3), point(x + card_w + 3, y + card_h + 3), accent, 10 * scale, 3.0f * scale, 0);
				draw->AddRect(point(x - 7, y - 7), point(x + card_w + 7, y + card_h + 7), IM_COL32(28, 128, 228, 105), 13 * scale, 2.0f * scale, 0);
			}
			// Cover slot: use the user's image from /data/ForjaSX2/covers when it
			// exists. The artwork fills the complete card and the selected title
			// is shown once in the top bar.
			fill(x + 2, y + 2, card_w - 4, card_h - 4, art_b[index % 5], 7);
			if (ImTextureData* cover = GetCoverTexture(s_items[index].cover))
			{
				const float box_w = card_w - 4.0f;
				const float box_h = card_h - 4.0f;
				const float image_aspect = static_cast<float>(cover->Width) / static_cast<float>(cover->Height);
				const float box_aspect = box_w / box_h;
				float image_w = box_w;
				float image_h = box_h;
				if (image_aspect > box_aspect)
					image_h = box_w / image_aspect;
				else
					image_w = box_h * image_aspect;
				const float image_x = x + 2.0f + (box_w - image_w) * 0.5f;
				const float image_y = y + 2.0f + (box_h - image_h) * 0.5f;
				draw->AddImage(cover->GetTexRef(), point(image_x, image_y), point(image_x + image_w, image_y + image_h));
			}
			else
			{
				draw->AddRectFilledMultiColor(point(x + 2, y + 2), point(x + card_w - 2, y + card_h - 2), art_a[index % 5], art_b[index % 5],
					IM_COL32(4, 15, 28, 255), art_a[index % 5]);
				for (int stripe = 0; stripe < 4; ++stripe)
					draw->AddLine(point(x + 2, y + 80 + stripe * 66), point(x + card_w - 2, y + 42 + stripe * 66), IM_COL32(139, 207, 255, 30), 2.0f * scale);
				draw->AddCircleFilled(point(x + card_w * 0.5f, y + card_h * 0.5f), 46 * scale,
					focused ? IM_COL32(56, 151, 224, 235) : IM_COL32(35, 98, 143, 225), 48);
			}
			if (s_items[index].favorite)
			{
				draw->AddCircleFilled(point(x + card_w - 25, y + 25), 20 * scale, IM_COL32(3, 15, 28, 225), 32);
				image("menu_favoritos", x + card_w - 39, y + 11, 28, 28);
			}
		}
		if (total_rows > VisibleRows)
		{
			constexpr float track_y = grid_y, track_h = card_h * 2.0f + (row_step - card_h);
			fill(1815, track_y, 5, track_h, IM_COL32(30, 66, 94, 180), 3);
			const float thumb_h = std::max(96.0f, track_h * static_cast<float>(VisibleRows) / static_cast<float>(total_rows));
			const float thumb_y = track_y + (track_h - thumb_h) * static_cast<float>(first_row) / static_cast<float>(max_first_row);
			fill(1815, thumb_y, 5, thumb_h, accent, 3);
		}
		if (!s_status.empty())
			text(346, 936, 17, accent, s_status.c_str(), 1496);
		if (settings_view)
		{
			// Forja SX2: the Settings page's own buttons.
			image("controle_x", 346, 1001, 30, 30);
			text(384, 1023, 16, white, T("Cambiar"), 126);
			text(520, 1023, 16, white, T("Izq./Der.: ajustar   Arriba/Abajo: elegir"), 500);
			image("controle_bola", 1035, 1001, 30, 30);
			text(1073, 1023, 16, muted, T("Menú lateral"), 400);
		}
		else
		{
		image("controle_x", 346, 1001, 30, 30);
		text(384, 1023, 16, white, T("Jugar"), 108);
		if (!disc_view)
		{
			image("controle_quadrado", 500, 1001, 30, 30);
			text(538, 1023, 16, current.path.empty() ? muted : white,
				current.favorite ? T("Quitar favorito") : T("Agregar favorito"), 214);
		}
		// Disc has no favorite action: use the next Library slots without an empty gap.
		const float refresh_x = disc_view ? 500.0f : 760.0f;
		const float exit_x = disc_view ? 760.0f : 1035.0f;
		image("controle_triangulo", refresh_x, 1001, 30, 30);
		text(refresh_x + 38, 1023, 16, white, disc_view ? T("Revisar disco") : T("Actualizar biblioteca"), exit_x - refresh_x - 46);
		image("controle_bola", exit_x, 1001, 30, 30);
		text(exit_x + 38, 1023, 16, muted, T("Salir"), 300);
		}
		if (s_exit_dialog_visible)
		{
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 165));
			fill(398, 282, 1124, 516, IM_COL32(0, 0, 0, 120), 20);
			fill(438, 322, 1044, 436, IM_COL32(4, 17, 31, 252), 12);
			draw->AddRect(point(438, 322), point(1482, 758), IM_COL32(35, 105, 161, 225), 12 * scale,
				1.5f * scale, 0);
			image("menu_logo", 790, 338, 340, 113);
			centered_text(960, 480, 32, white, T("¿Cerrar Forja SX2?"), 980);
			centered_text(960, 520, 18, muted, T("¿Seguro que quieres salir de la aplicación?"), 980);
			const auto dialog_button = [&](float x, const char* label, bool active) {
				fill(x, 580, 260, 72, active ? IM_COL32(18, 88, 153, 255) : IM_COL32(11, 34, 54, 255), 10);
				draw->AddRect(point(x, 580), point(x + 260, 652),
					active ? IM_COL32(48, 170, 255, 255) : IM_COL32(37, 80, 113, 190), 10 * scale,
					active ? 2.0f * scale : 1.0f * scale, 0);
				centered_text(x + 130, 626, 20, active ? white : muted, label, 236);
			};
			dialog_button(670, T("Salir de Forja SX2"), s_exit_confirm);
			dialog_button(990, T("Cancelar"), !s_exit_confirm);
			image("controle_x", 780, 694, 28, 28);
			text(816, 715, 16, white, T("Aceptar"), 170);
			image("controle_bola", 995, 694, 28, 28);
			text(1031, 715, 16, muted, T("Atrás"), 170);
		}
		// Forja SX2: the updater's question, laid out like the exit dialog above.
		if (s_update_dialog_visible)
		{
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 165));
			fill(398, 282, 1124, 516, IM_COL32(0, 0, 0, 120), 20);
			fill(438, 322, 1044, 436, IM_COL32(4, 17, 31, 252), 12);
			draw->AddRect(point(438, 322), point(1482, 758), IM_COL32(35, 105, 161, 225), 12 * scale, 1.5f * scale, 0);
			image("menu_logo", 790, 338, 340, 113);
			char title[256];
			std::snprintf(title, sizeof(title), TF("%s disponible"), PS4Release::LatestName().c_str());
			centered_text(960, 480, 32, white, title, 980);
			centered_text(960, 520, 18, muted, T("Se baja a /data/pkg y se verifica. Después cierra Forja e instálala con GoldHEN."), 980);
			centered_text(960, 548, 16, muted, T("Tus juegos, ajustes y perfiles se quedan como están."), 980);
			const auto update_button = [&](float x, const char* label, bool active) {
				fill(x, 580, 260, 72, active ? IM_COL32(18, 88, 153, 255) : IM_COL32(11, 34, 54, 255), 10);
				draw->AddRect(point(x, 580), point(x + 260, 652), active ? IM_COL32(48, 170, 255, 255) : IM_COL32(37, 80, 113, 190),
					10 * scale, active ? 2.0f * scale : 1.0f * scale, 0);
				centered_text(x + 130, 626, 20, active ? white : muted, label, 236);
			};
			update_button(670, T("Descargar"), s_update_confirm);
			update_button(990, T("Ahora no"), !s_update_confirm);
			image("controle_x", 780, 694, 28, 28);
			text(816, 715, 16, white, T("Aceptar"), 170);
			image("controle_bola", 995, 694, 28, 28);
			text(1031, 715, 16, muted, T("Atrás"), 170);
		}
		// Forja SX2 (2026-10-19): the language picker, over everything; each language's name in its own letters, the title in
		// the language under the cursor.
		if (s_language_dialog_visible)
		{
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 165));
			fill(398, 92, 1124, 896, IM_COL32(0, 0, 0, 120), 20);
			fill(438, 132, 1044, 816, IM_COL32(4, 17, 31, 252), 12);
			draw->AddRect(point(438, 132), point(1482, 948), IM_COL32(35, 105, 161, 225), 12 * scale, 1.5f * scale, 0);
			image("menu_logo", 840, 144, 240, 80);
			centered_text(960, 272, 32, white, s_language_texts[0].c_str(), 980);
			centered_text(960, 308, 18, muted, s_language_texts[1].c_str(), 980);
			for (unsigned index = 0; index < ForjaLang::LanguageCount; index++)
			{
				const float x = index < LanguageRowsPerColumn ? 500.0f : 980.0f;
				const float y = 340.0f + static_cast<float>(index % LanguageRowsPerColumn) * 74.0f;
				const bool active = index == s_language_focus;
				const bool current_language = index == ForjaLang::Current();
				fill(x, y, 440, 62, active ? IM_COL32(18, 88, 153, 255) : IM_COL32(11, 34, 54, 255), 10);
				draw->AddRect(point(x, y), point(x + 440, y + 62), active ? IM_COL32(48, 170, 255, 255) : IM_COL32(37, 80, 113, 190),
					10 * scale, active ? 2.0f * scale : 1.0f * scale, 0);
				centered_text(x + 220, y + 41, 24, active ? white : current_language ? accent : muted, ForjaLang::Languages[index].name, 400);
			}
			image("controle_x", 780, 878, 28, 28);
			text(816, 899, 16, white, s_language_texts[2].c_str(), 170);
			image("controle_bola", 995, 878, 28, 28);
			text(1031, 899, 16, muted, s_language_texts[3].c_str(), 170);
		}
		return true;
	}

	void SetInGameMenu(const InGameMenuState& state)
	{
		s_in_game_state.store(PackInGameMenuState(state), std::memory_order_release);
		// Paused emulation produces no VSync. Queue the redraw on the GS thread
		// so every value change is visible immediately in the panel.
		MTGS::RunOnGSThread([]() {
			GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
	}

	void ShowLoading(unsigned progress, const char* status)
	{
		const std::string text = status ? status : "";
		MTGS::RunOnGSThread([progress, text]() {
			s_visible = false;
			s_loading_visible = true;
			s_loading_carousel = false;
			s_loading_progress = std::min(progress, 100u);
			s_loading_status = text;
			GSPresentCurrentFrame();
		});
		MTGS::WaitGS(false);
	}

	void HideLoading()
	{
		MTGS::RunOnGSThread([]() {
			s_loading_visible = false;
			s_loading_carousel = false;
			s_loading_progress = 0;
			s_loading_status.clear();
		});
		MTGS::WaitGS(false);
	}
} // namespace PS4GameMenu
