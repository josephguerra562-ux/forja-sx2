// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) Uses the same Vulkan/ImGui presentation as the in-game HUD.
#include "GameMenu.h"
#include "ForjaSettings.h"
#include "InGameMenuState.h"
#include "PhysicalDisc.h"
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
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cfloat>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <pthread.h>
#include <sys/stat.h>
#include <utility>
#include <vector>

extern void ProbeLog(const char* format, ...);
// Forja SX2: the watchdog in main.cpp checks that the library loop keeps turning.
extern std::atomic<u64> g_forja_menu_ticks;
extern std::atomic<bool> g_forja_menu_active;
// Forja SX2: GitHub Actions' run number (CMakeLists.txt); 0 for a local build.
#ifndef FORJA_BUILD_NUMBER
#define FORJA_BUILD_NUMBER 0
#endif

namespace PS4GameMenu
{
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
				if (!active) error = "No se pudo iniciar la detección del disco.";
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
		LibraryView s_library_view = LibraryView::Library;
		// Forja SX2: the Settings page. The CPU thread owns the values; the GS thread draws this copy.
		ForjaSettings::Values s_settings_gs;
		unsigned s_settings_row_gs = 0;
		constexpr unsigned SettingsRows = 10;

		const char* SettingLabel(unsigned row)
		{
			static constexpr const char* labels[SettingsRows] = {
				"Velocidad del CPU de la PS2", "Resolución interna", "Relación de aspecto", "Contador de FPS al jugar",
				"Sensibilidad de la palanca", "Cargas rápidas del disco", "Saltar el logo de la PS2",
				"Lecturas de la gráfica (readbacks)", "Modo video automático", "Generación de cuadros (30 a 60 FPS)"};
			return labels[std::min(row, SettingsRows - 1)];
		}

		const char* SettingHelp(unsigned row)
		{
			static constexpr const char* help[SettingsRows] = {
				"Menos % = más FPS en la PS4. Cada juego puede tener la suya desde L3+R3 (esa manda)",
				"1x es la de la PS2. En PS4 Pro aguanta 2x-4x en juegos ligeros",
				"Auto respeta lo que pide el juego",
				"Muestra FPS y velocidad arriba a la derecha (se cambia también en L3+R3)",
				"Qué tanto hay que empujar la palanca para llegar al tope. 133 % es como el PCSX2 de PC",
				"Acorta las cargas. Algunos juegos se cuelgan con esto: si pasa, apágalo",
				"Arranca directo el juego, sin la animación de la PS2",
				"Si el juego se traba sin que el CPU esté al 100 %: prueba Sin esperar. Desactivadas puede romper efectos",
				"Durante los videos del juego baja el CPU de la PS2 al 50 % y lo devuelve al terminar (videos más fluidos)",
				"EXPERIMENTAL: un juego estable a 30 FPS se ve a 60 (la gráfica inventa el cuadro intermedio). +1 cuadro de retraso"};
			return help[std::min(row, SettingsRows - 1)];
		}

		void SettingValue(const ForjaSettings::Values& v, unsigned row, char* out, size_t size)
		{
			static constexpr const char* ee_names[] = {"50 %", "60 %", "75 %", "100 % (PS2 real)", "130 %", "180 %", "300 %"};
			static constexpr const char* aspect_names[] = {"Estirar", "Auto", "4:3", "16:9", "10:7"};
			switch (row)
			{
				case 0: std::snprintf(out, size, "%s", ee_names[std::clamp(v.ee_rate, -3, 3) + 3]); break;
				case 1: std::snprintf(out, size, "%dx%s", v.upscale, v.upscale == 1 ? " (nativa)" : ""); break;
				case 2: std::snprintf(out, size, "%s", aspect_names[std::clamp(v.aspect, 0, 4)]); break;
				case 3: std::snprintf(out, size, "%s", v.overlay ? "Sí" : "No"); break;
				case 4: std::snprintf(out, size, "%d %%", v.stick); break;
				case 5: std::snprintf(out, size, "%s", v.fast_cdvd ? "Sí" : "No"); break;
				case 6: std::snprintf(out, size, "%s", v.fast_boot ? "Sí" : "No"); break;
				case 7:
				{
					static constexpr const char* readback_names[] = {"Precisas", "Sin esperar (más rápido)", "Desactivadas"};
					std::snprintf(out, size, "%s", readback_names[std::clamp(v.readback, 0, 2)]);
					break;
				}
				case 8: std::snprintf(out, size, "%s", v.video_mode ? "Sí" : "No"); break;
				default: std::snprintf(out, size, "%s", v.frame_gen ? "Sí" : "No"); break;
			}
		}

		// delta -1/+1 from Left/Right; X passes +1 with wrap so it cycles through the choices.
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
				case 0: v.ee_rate = step(v.ee_rate, -3, 3); break;
				case 1: v.upscale = step(v.upscale, 1, 4); break;
				case 2: v.aspect = step(v.aspect, 0, 4); break;
				case 3: v.overlay = !v.overlay; break;
				case 4:
				{
					int index = 0;
					while (index + 1 < ForjaSettings::StickStepCount && ForjaSettings::StickSteps[index] < v.stick)
						++index;
					v.stick = ForjaSettings::StickSteps[step(index, 0, ForjaSettings::StickStepCount - 1)];
					break;
				}
				case 5: v.fast_cdvd = !v.fast_cdvd; break;
				case 6: v.fast_boot = !v.fast_boot; break;
				case 7: v.readback = step(v.readback, 0, 2); break;
				case 8: v.video_mode = !v.video_mode; break;
				default: v.frame_gen = !v.frame_gen; break;
			}
		}

		void PublishSettings(const ForjaSettings::Values& values, unsigned row)
		{
			MTGS::RunOnGSThread([values, row]() {
				s_settings_gs = values;
				s_settings_row_gs = row;
			});
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
		std::atomic<unsigned> s_in_game_state{0};
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
					return LoadImageFile(candidate, "cover");
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
				const char* message = view == LibraryView::Disc      ? "No se detectó ningún disco" :
				                      view == LibraryView::Recent    ? "No hay juegos recientes" :
				                      view == LibraryView::Favorites ? "No hay juegos favoritos" :
				                      view == LibraryView::Settings  ? "Ajustes de Forja SX2 (para todos los juegos)" :
				                                                       "No se encontraron juegos";
				items.push_back({message, {}, {}, false});
			}
			return items;
		}

		void Publish(const std::vector<Item>& items, LibraryView view)
		{
			MTGS::RunOnGSThread([items, view]() {
				ClearCoverTextures();
				s_items = items;
				s_library_view = view;
				s_loading_visible = false;
				s_loading_carousel = false;
				s_intro_visible = false;
				s_visible = true;
			});
		}

		void Present(size_t selected, const std::string& status, bool connected, bool sidebar_focused,
			bool exit_dialog_visible, bool exit_confirm)
		{
			MTGS::RunOnGSThread([selected, status, connected, sidebar_focused, exit_dialog_visible, exit_confirm]() {
				s_connected = connected;
				s_sidebar_focused = sidebar_focused;
				s_exit_dialog_visible = exit_dialog_visible;
				s_exit_confirm = exit_confirm;
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
				s_loading_status = "Cargando...";
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
		// Decode the packaged button artwork on the CPU thread; the GS callback
		// only uploads textures and draws them.
		LoadUiAssets();
		PresentStartupLoading(0, 0.0f);
		// Keep the system pic1 visible until the first Vulkan loading frame exists.
		sceSystemServiceHideSplashScreen();
		PlayStartupSequence();
		// (AI-assisted) Query the public release endpoint asynchronously. The
		// launcher remains usable while DNS/HTTPS is in progress.
		PS4Release::Start();
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
		std::string status;
		// Forja SX2: Settings page state. Every change is saved at once (ajustes.ini) and used by the next game.
		ForjaSettings::Values settings = ForjaSettings::Load();
		unsigned settings_row = 0;
		PublishSettings(settings, settings_row);
		const auto change_setting = [&](int delta, bool wrap) {
			ChangeSetting(settings, settings_row, delta, wrap);
			ProbeLog("Game menu: setting %u changed\n", settings_row);
			status = ForjaSettings::Save(settings) ? "Guardado. Se aplica al iniciar el próximo juego." :
			                                         "No se pudieron guardar los ajustes.";
			PublishSettings(settings, settings_row);
		};
		DiscProbe disc_probe;
		DiscCoverDownload disc_cover;
		DiscCoverDownload library_cover;
		size_t library_cover_index = 0;
		std::string disc_status;
		bool boot_disc_when_ready = false;
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
			disc_probe_at = 0.0;
			boot_disc_when_ready = boot;
			const bool started = disc_probe.Start();
			items = {{started ? "Revisando el DVD..." : "Falló la detección del disco", {}, {}, false}};
			selected = 0;
			Publish(items, view);
			status = started ? "Leyendo la unidad. Puedes seguir navegando." : disc_probe.error;
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
				if (!disc_probe.active)
				{
					disc_probe_at = timer.GetTimeSeconds() + 0.7;
					items = {{"Revisando el DVD...", {}, {}, false}};
					Publish(items, view);
				}
				else status = "Revisando el DVD...";
			}
			else if (view == LibraryView::Settings)
			{
				settings = ForjaSettings::Load();
				settings_row = 0;
				PublishSettings(settings, settings_row);
			}
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
					status += " Portada no disponible. Triángulo para reintentar.";
			}
			if (disc_probe.Collect() && view == LibraryView::Disc)
			{
				if (disc_probe.reader)
				{
					const auto& disc = disc_probe.reader->GetInfo();
					const auto* game = GameDatabase::findGame(disc.serial);
					const std::string title = game ? std::string(game->name) : "PS2 DVD - " + disc.serial;
					// Serial identifies the exact disc edition; the database title is a fallback.
					auto cover = LoadCover(disc.serial + ".iso");
					if (!cover && game && title.find_first_of("/\\") == std::string::npos)
						cover = LoadCover(title + ".iso");
					if (!cover && !boot_disc_when_ready && !disc_cover.Active())
						disc_cover.Start(disc.serial);
					items = {{title, PS4Disc::DevicePath, std::move(cover), false}};
					status = disc.serial + (disc.dual_type ? " | DVD doble capa | Pulsa X para iniciar." : " | DVD | Pulsa X para iniciar.");
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
					items = {{"DVD no disponible", {}, {}, false}};
					status = disc_probe.error;
				}
				boot_disc_when_ready = false;
				disc_status = status;
				if (disc_cover.Active())
					status += " Descargando portada...";
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
					             (removing ? "Quitado de favoritos." : "Agregado a favoritos.") :
					             "No se pudieron guardar los favoritos.";
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
					// Forja SX2: X moves the selected setting to its next choice (and wraps around).
					change_setting(1, true);
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
					status = view == LibraryView::Disc     ? "El soporte de disco físico está en preparación." :
					         view == LibraryView::Settings ? "Aquí irán las opciones de ajustes." :
					                                         "No hay juegos en esta vista.";
				else if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
					status = "Archivo no disponible. Pulsa Triángulo para actualizar.";
				else
				{
					// (AI-assisted) Keep an unreadable/unsupported image in the launcher
					// with an error, instead of tearing down the UI and exiting the app.
					InputIsoFile image;
					Error image_error;
					if (!image.Open(path, &image_error))
					{
						status = "No se puede abrir el juego: " + image_error.GetDescription();
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
					Present(selected, "Iniciando...", connected, false, false, false);
					// Consume the confirmation before handing the controller to the game.
					while (scePadReadState(pad, &data) == 0 && data.connected && (data.buttons & ORBIS_PAD_BUTTON_CROSS))
						Threading::Sleep(16);
					start = true;
					ProbeLog("Game menu: selected %s\n", path.c_str());
					platform = items[selected].platform;
					break;
				}
			}
			Present(selected, connected ? (view == LibraryView::Disc && status.empty() ? disc_status : status) : "Conecta un control para elegir un juego.", connected,
				sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// Never destroy a live probe or leave a detached thread using menu state.
		disc_cover.Cancel();
		while (disc_cover.Active() && !disc_cover.Collect())
		{
			Present(selected, "Terminando de bajar la portada...", true, sidebar_focused, false, false);
			Threading::Sleep(16);
		}
		// The driver timeout is requested per command; hardware behavior needs proper testing.
		while (disc_probe.active && !disc_probe.Collect())
		{
			Present(selected, "Terminando de detectar el disco...", true, sidebar_focused, false, false);
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
			s_items.clear();
			s_status.clear();
			s_loading_visible = start;
			s_loading_carousel = false;
			s_loading_progress = start ? 10u : 0u;
			s_loading_status = start ? "Preparando el emulador..." : "";
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
			ImFont* font = ImGui::GetFont();
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
				const float status_width = font->CalcTextSizeA(18.0f * scale, FLT_MAX, 0.0f, s_loading_status.c_str()).x / scale;
				draw->AddText(font, 18.0f * scale, point(960.0f - status_width * 0.5f, by + 62),
					IM_COL32(213, 232, 249, 255), s_loading_status.c_str());
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
			ImFont* font = ImGui::GetFont();
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
			draw->AddText(font, 34.0f * scale, point(196, 82), white, "Menú del emulador");
			draw->AddText(font, 18.0f * scale, point(138, 205), muted, "JUEGO EN PAUSA");
			char resolution[96];
			std::snprintf(resolution, sizeof(resolution), "Resolución interna:  < %ux >%s", upscale,
				upscale == 1 ? "  (nativa)" : "");
			static constexpr const char* aspect_names[] = {"Estirar", "Auto", "4:3", "16:9", "10:7"};
			char aspect_label[96];
			std::snprintf(aspect_label, sizeof(aspect_label), "Relación de aspecto:  < %s >", aspect_names[std::min(aspect, 4u)]);
			char performance_label[96];
			std::snprintf(performance_label, sizeof(performance_label), "Contador de rendimiento:  < %s >",
				show_performance_overlay ? "Sí" : "No");
			// Forja SX2: los mismos porcentajes que muestra PCSX2 para EECycleRate -3..3.
			static constexpr const char* ee_names[] = {"50 %", "60 %", "75 %", "100 % (PS2 real)", "130 %", "180 %", "300 %"};
			char ee_label[96];
			std::snprintf(ee_label, sizeof(ee_label), "Velocidad del CPU de la PS2:  < %s >", ee_names[std::clamp(ee_rate, -3, 3) + 3]);
			const char* labels[] = {"Volver al juego", resolution, aspect_label, performance_label, ee_label, "Volver a la biblioteca", "Salir de la aplicación"};
			const char* descriptions[] = {
				"Seguir jugando desde donde quedó",
				"Más calidad de imagen en esta sesión (consume más)",
				"Forma de la imagen en pantalla",
				"Mostrar u ocultar FPS, velocidad y tiempos",
				"Menos % = más FPS en la PS4 (el juego puede perder cuadros). Se guarda para este juego",
				"Cerrar el juego y volver a la biblioteca",
				"Cerrar el juego y salir de Forja SX2"};
			const auto draw_option_icon = [&](unsigned row, float cx, float cy, bool selected) {
				const ImU32 icon_bg = selected ? IM_COL32(34, 127, 214, 255) : IM_COL32(16, 55, 86, 255);
				const ImU32 icon_line = selected ? IM_COL32(231, 247, 255, 255) : IM_COL32(155, 204, 235, 255);
				draw->AddCircleFilled(point(cx, cy), 29.0f * scale, icon_bg, 32);
				draw->AddCircle(point(cx, cy), 29.0f * scale, selected ? accent : IM_COL32(49, 111, 155, 255), 32, 1.5f * scale);
				if (row == 0)
				{
					draw->AddTriangleFilled(point(cx - 7, cy - 11), point(cx - 7, cy + 11), point(cx + 13, cy), icon_line);
				}
				else if (row == 1)
				{
					draw->AddRect(point(cx - 13, cy - 13), point(cx + 13, cy + 13), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 7, cy + 6), point(cx + 7, cy + 6), icon_line, 2 * scale);
				}
				else if (row == 2)
				{
					draw->AddRect(point(cx - 15, cy - 10), point(cx + 15, cy + 10), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 8, cy - 15), point(cx + 8, cy - 15), icon_line, 2 * scale);
				}
				else if (row == 3)
				{
					draw->AddRect(point(cx - 15, cy - 9), point(cx + 15, cy + 9), icon_line, 3 * scale, 2 * scale, 0);
					draw->AddLine(point(cx - 9, cy), point(cx + 9, cy), icon_line, 2 * scale);
					draw->AddCircle(point(cx, cy), 4 * scale, icon_line, 16, 2 * scale);
				}
				else if (row == 4)
				{
					draw->AddCircle(point(cx, cy + 4), 14 * scale, icon_line, 24, 2 * scale);
					draw->AddLine(point(cx, cy + 4), point(cx + 8, cy - 6), icon_line, 3 * scale);
				}
				else if (row == 5)
				{
					draw->AddLine(point(cx - 12, cy), point(cx + 10, cy), icon_line, 3 * scale);
					draw->AddLine(point(cx - 12, cy), point(cx - 2, cy - 9), icon_line, 3 * scale);
					draw->AddLine(point(cx - 12, cy), point(cx - 2, cy + 9), icon_line, 3 * scale);
				}
				else
				{
					draw->AddLine(point(cx - 10, cy - 12), point(cx + 10, cy + 12), icon_line, 3 * scale);
					draw->AddLine(point(cx + 10, cy - 12), point(cx - 10, cy + 12), icon_line, 3 * scale);
				}
			};
			for (unsigned row = 0; row < 7; row++)
			{
				const float y = 200.0f + row * 77.0f;
				const bool selected = selected_row == row;
				draw->AddRectFilled(point(112, y), point(1808, y + 72),
					selected ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 255), 10.0f * scale);
				if (selected)
					draw->AddRect(point(112, y), point(1808, y + 72), accent, 10.0f * scale, 2.0f * scale, 0);
				draw_option_icon(row, 168, y + 36, selected);
				draw->AddText(font, 26.0f * scale, point(226, y + 12), white, labels[row]);
				draw->AddText(font, 16.0f * scale, point(226, y + 45), muted, descriptions[row]);
				if (row >= 1 && row <= 4)
					draw->AddText(font, 20.0f * scale, point(1580, y + 24), selected ? white : muted,
						(row == 1 || row == 4) ? "AJUSTAR" : (row == 2 ? "ELEGIR" : "CAMBIAR"));
			}
			char hint[128];
			std::snprintf(hint, sizeof(hint), "Izq./Der.: cambiar valor   |   Resolución: 1x-%ux", max_upscale);
		draw->AddText(font, 17.0f * scale, point(138, 756), muted, hint);
		draw->AddText(font, 17.0f * scale, point(138, 786), muted, "3x o más es experimental y solo para PS4 Pro.");
		draw->AddText(font, 17.0f * scale, point(138, 816), muted, "Arriba/Abajo: elegir   Izq./Der.: cambiar   X: aceptar   Círculo: volver");
			image("controle_x", 1480, 904, 30, 30);
			draw->AddText(font, 20.0f * scale, point(1524, 909), white, "Aceptar");
			image("controle_bola", 1690, 904, 30, 30);
			draw->AddText(font, 20.0f * scale, point(1734, 909), muted, "Volver");
			return true;
		}
		if (!s_visible || s_items.empty())
			return false;
		// (AI-assisted) The launcher is drawn by the same Vulkan/ImGui device as
		// the emulator. Keep this path free of file I/O so a library refresh never
		// stalls the GS thread. Cover files will plug into the art slot later.
		ImDrawList* draw = ImGui::GetBackgroundDrawList();
		ImFont* font = ImGui::GetFont();
		const ImVec2 size = ImGui::GetIO().DisplaySize;
		const float scale = std::min(size.x / 1920.0f, size.y / 1080.0f);
		const ImVec2 origin((size.x - 1920 * scale) * 0.5f, (size.y - 1080 * scale) * 0.5f);
		const auto point = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
		const auto fill = [&](float x, float y, float w, float h, ImU32 color, float radius = 12.0f) {
			draw->AddRectFilled(point(x, y), point(x + w, y + h), color, radius * scale);
		};
		const auto text = [&](float x, float baseline, float pixels, ImU32 color, const char* value) {
			draw->AddText(font, pixels * scale, point(x, baseline - pixels * 0.82f), color, value);
		};
		const auto centered_text = [&](float center_x, float baseline, float pixels, ImU32 color, const char* value) {
			const float width = font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, value).x / scale;
			text(center_x - width * 0.5f, baseline, pixels, color, value);
		};
		const auto image = [&](const char* name, float x, float y, float w, float h) {
			if (ImTextureData* texture = GetCoverTexture(FindUiAsset(name)))
				draw->AddImage(texture->GetTexRef(), point(x, y), point(x + w, y + h));
		};
		const auto fit = [&](std::string value, float pixels, float width) {
			if (font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, value.c_str()).x > width * scale)
			{
				do
				{
					size_t last = value.size() - 1;
					while (last > 0 && (static_cast<unsigned char>(value[last]) & 0xc0) == 0x80)
						--last;
					value.resize(last);
				} while (!value.empty() && font->CalcTextSizeA(pixels * scale, FLT_MAX, 0, (value + "…").c_str()).x > width * scale);
				value += "…";
			}
			return value;
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
		centered_text(142, 82, 11, muted, "EMULADOR DE PLAYSTATION 2");
		const size_t selected = std::min(s_selected, s_items.size() - 1);
		const Item& current = s_items[selected];
		text(348, 59, 28, white, fit(current.name, 28, 1430).c_str());
		if (PS4Release::HasUpdate())
		{
			fill(1480, 22, 408, 50, IM_COL32(8, 54, 91, 235), 10);
			draw->AddRect(point(1480, 22), point(1888, 72), IM_COL32(42, 163, 255, 225), 10 * scale,
				1.0f * scale, 0);
			char update_text[96];
			std::snprintf(update_text, sizeof(update_text), "Actualización disponible: %s", PS4Release::LatestVersion());
			text(1502, 54, 17, white, update_text);
		}
		// The sidebar changes the data source while retaining the same grid.
		static constexpr const char* sidebar_icons[] = {
			"menu_biblioteca", "menu_disc", "menu_recentes", "menu_favoritos", "menu_configuracoes"};
		static constexpr const char* sidebar_labels[] = {"Biblioteca", "Disco", "Recientes", "Favoritos", "Ajustes"};
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
			text(91, y + 37, active ? 19 : 18, active ? white : muted, sidebar_labels[i]);
		}
		// Forja SX2: the build number, so a tester sees at a glance whether the new package is the one installed.
		if (FORJA_BUILD_NUMBER > 0)
		{
			char build_text[32];
			std::snprintf(build_text, sizeof(build_text), "Compilación %d", FORJA_BUILD_NUMBER);
			text(43, 1023, 16, muted, build_text);
		}
		else
			text(43, 1023, 16, muted, "Compilación local");
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
			text(1030, 340, 30, white, "DVD de PS2");
			text(1030, 390, 20, muted, detected ? "Disco detectado. Pulsa X para iniciar." : "Mete un DVD y pulsa Triángulo para actualizar.");
		}
		// Forja SX2: the Settings page, one row per choice with its value on the right.
		const bool settings_view = s_library_view == LibraryView::Settings;
		if (settings_view)
		{
			const unsigned focused_row = std::min(s_settings_row_gs, SettingsRows - 1);
			for (unsigned row = 0; row < SettingsRows; ++row)
			{
				const float y = 108.0f + static_cast<float>(row) * 80.0f;
				const bool focused = !s_sidebar_focused && row == focused_row;
				fill(346, y, 1470, 72, focused ? IM_COL32(20, 77, 125, 255) : IM_COL32(11, 33, 52, 235), 10);
				if (focused)
					draw->AddRect(point(346, y), point(1816, y + 72), accent, 10 * scale, 2.0f * scale, 0);
				text(380, y + 31, 22, white, SettingLabel(row));
				text(380, y + 57, 15, muted, SettingHelp(row));
				char value[64];
				SettingValue(s_settings_gs, row, value, sizeof(value));
				char shown[96];
				std::snprintf(shown, sizeof(shown), focused ? "<  %s  >" : "%s", value);
				const float width = font->CalcTextSizeA(25.0f * scale, FLT_MAX, 0, shown).x / scale;
				text(1786 - width, y + 44, 25, focused ? white : accent, shown);
			}
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
			text(346, 936, 17, accent, fit(s_status, 17, 1496).c_str());
		if (settings_view)
		{
			// Forja SX2: the Settings page's own buttons.
			image("controle_x", 346, 1001, 30, 30);
			text(384, 1023, 16, white, "Cambiar");
			text(520, 1023, 16, white, "Izq./Der.: ajustar   Arriba/Abajo: elegir");
			image("controle_bola", 1035, 1001, 30, 30);
			text(1073, 1023, 16, muted, "Menú lateral");
		}
		else
		{
		image("controle_x", 346, 1001, 30, 30);
		text(384, 1023, 16, white, "Jugar");
		if (!disc_view)
		{
			image("controle_quadrado", 500, 1001, 30, 30);
			text(538, 1023, 16, current.path.empty() ? muted : white,
				current.favorite ? "Quitar favorito" : "Agregar favorito");
		}
		// Disc has no favorite action: use the next Library slots without an empty gap.
		const float refresh_x = disc_view ? 500.0f : 760.0f;
		const float exit_x = disc_view ? 760.0f : 1035.0f;
		image("controle_triangulo", refresh_x, 1001, 30, 30);
		text(refresh_x + 38, 1023, 16, white, disc_view ? "Revisar disco" : "Actualizar biblioteca");
		image("controle_bola", exit_x, 1001, 30, 30);
		text(exit_x + 38, 1023, 16, muted, "Salir");
		}
		if (s_exit_dialog_visible)
		{
			draw->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 165));
			fill(398, 282, 1124, 516, IM_COL32(0, 0, 0, 120), 20);
			fill(438, 322, 1044, 436, IM_COL32(4, 17, 31, 252), 12);
			draw->AddRect(point(438, 322), point(1482, 758), IM_COL32(35, 105, 161, 225), 12 * scale,
				1.5f * scale, 0);
			image("menu_logo", 790, 338, 340, 113);
			centered_text(960, 480, 32, white, "¿Cerrar Forja SX2?");
			centered_text(960, 520, 18, muted, "¿Seguro que quieres salir de la aplicación?");
			const auto dialog_button = [&](float x, const char* label, bool active) {
				fill(x, 580, 260, 72, active ? IM_COL32(18, 88, 153, 255) : IM_COL32(11, 34, 54, 255), 10);
				draw->AddRect(point(x, 580), point(x + 260, 652),
					active ? IM_COL32(48, 170, 255, 255) : IM_COL32(37, 80, 113, 190), 10 * scale,
					active ? 2.0f * scale : 1.0f * scale, 0);
				centered_text(x + 130, 626, 20, active ? white : muted, label);
			};
			dialog_button(670, "Salir de Forja SX2", s_exit_confirm);
			dialog_button(990, "Cancelar", !s_exit_confirm);
			image("controle_x", 780, 694, 28, 28);
			text(816, 715, 16, white, "Aceptar");
			image("controle_bola", 995, 694, 28, 28);
			text(1031, 715, 16, muted, "Atrás");
		}
		return true;
	}

	void SetInGameMenu(bool visible, unsigned selected, unsigned upscale, unsigned max_upscale, unsigned aspect,
		bool show_performance_overlay, int ee_rate)
	{
		const unsigned state = PackInGameMenuState({visible, selected, upscale, max_upscale, aspect,
			show_performance_overlay, ee_rate});
		s_in_game_state.store(state, std::memory_order_release);
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
