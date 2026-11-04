/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
** SDL3Main.cpp
**
** Entry point for Linux builds using SDL3 windowing and DXVK graphics.
**
** TheSuperHackers @feature CnC_Generals_Linux 07/02/2026
** Entry point replaces WinMain() for Linux builds.
** Instantiates SDL3GameEngine and calls GameMain().
*/

#ifndef _WIN32

// SYSTEM INCLUDES
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>   // _NSGetExecutablePath (app-bundle detection)
#include <climits>
#include <cerrno>
#include <filesystem>
#include <string>
#endif
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// On iOS, SDL renames main() to SDL_main and provides its own UIApplicationMain
// bootstrap; the app lifecycle (suspend/resume, window) is owned by SDL.
#include <SDL3/SDL_main.h>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <filesystem>
#include <string>
#endif
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <unistd.h>   // _exit()
#include <glob.h>     // glob() for Vulkan ICD discovery

// USER INCLUDES (match WinMain.cpp pattern)
#include "Lib/BaseType.h"
#include "Common/CommandLine.h"
#include "Common/CriticalSection.h"
#include "Common/GlobalData.h"
#include "Common/GameEngine.h"
#include "Common/GameMemory.h"
#include "Common/Debug.h"
#include "Common/version.h"  // GeneralsX @bugfix BenderAI 14/02/2026 Version class + TheVersion extern
#include "SDL3GameEngine.h"

// DXVK WSI
#define DXVK_WSI_SDL3 1
#include <wsi/native_wsi.h>

// CRITICAL SECTIONS (Linux needs these too)
static CriticalSection critSec1;
static CriticalSection critSec2;
static CriticalSection critSec3;
static CriticalSection critSec4;
static CriticalSection critSec5;

// GLOBAL COMMAND LINE ARGUMENTS
// TheSuperHackers @build felipebraz 13/02/2026
// Store argc/argv from main() for use by CommandLine.cpp parseCommandLine() on Linux
// Windows provides these automatically; Linux needs explicit globals
int __argc = 0;          ///< global argument count
char** __argv = nullptr; ///< global argument vector

// GLOBAL WINDOW HANDLE
// TheSuperHackers @build felipebraz 13/02/2026
// ApplicationHWnd is declared extern in GeneralsMD/Code/Main/WinMain.h
// On Linux, we cast SDL_Window* to HWND type for compatibility
HWND ApplicationHWnd = nullptr;  ///< our application window handle

// GLOBAL SDL3 WINDOW
// GeneralsX @feature felipebraz 16/02/2026
// SDL3 window created in main() before GameMain(), stored globally for engine access
SDL_Window* TheSDL3Window = nullptr;

// GAME TEXT FILE PATHS
// TheSuperHackers @build felipebraz 13/02/2026
// GameText.cpp uses these paths to load CSF and STR files (game localization)
// Format %s is replaced with language code in GameTextManager::init()
// GeneralsX @bugfix BenderAI 13/02/2026 - Fix case-sensitivity on Linux (generals.csf vs Generals.csf)
const Char *g_csfFile = "data/%s/generals.csf";  ///< CSF file path (lowercase for Linux compatibility)
const Char *g_strFile = "data/Generals.str";     ///< STR file path

// Extern declarations (from GameMain.cpp)
extern Int GameMain();

/**
 * FilterSoftwareVulkanICDs
 *
 * Sets VK_DRIVER_FILES to only hardware Vulkan ICDs, excluding LLVMpipe/lavapipe.
 *
 * Workaround for Mesa/LLVM 20.x bug: libvulkan_lvp.so (LLVMpipe Vulkan ICD) crashes
 * during dlopen() static initialization with a null-ptr deref in llvm::Regex::Regex().
 * The Vulkan loader loads ALL ICDs found in the ICD directories when
 * vkEnumerateInstanceExtensionProperties() is called, which triggers the crash.
 * Filtering hardware-only ICDs via VK_DRIVER_FILES prevents loading libvulkan_lvp.so.
 *
 * Only applied when neither VK_DRIVER_FILES nor VK_ICD_FILENAMES is already set,
 * so the user can always override by setting those variables externally.
 *
 * GeneralsX @bugfix BenderAI 06/03/2026
 */
static void FilterSoftwareVulkanICDs()
{
	if (getenv("VK_DRIVER_FILES") || getenv("VK_ICD_FILENAMES")) {
		return;
	}

	auto icd_is_software = [](const char *name) -> bool {
		char low[256] = "";
		for (int i = 0; name[i] && i < 255; ++i) {
			low[i] = (char)tolower((unsigned char)name[i]);
		}
		return strstr(low, "lvp") || strstr(low, "lavapipe") || strstr(low, "softpipe") || strstr(low, "llvmpipe");
	};

	static char hw_icds[4096] = "";
	const char *patterns[] = {
		"/usr/share/vulkan/icd.d/*.json",
		"/etc/vulkan/icd.d/*.json",
		nullptr
	};

	glob_t gl = {};
	int gflags = 0;
	for (int i = 0; patterns[i]; ++i) {
		if (glob(patterns[i], gflags, nullptr, &gl) == 0) {
			gflags = GLOB_APPEND;
		}
	}

	bool found_hw = false;
	for (size_t i = 0; i < gl.gl_pathc; ++i) {
		const char *path = gl.gl_pathv[i];
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		if (icd_is_software(base)) {
			fprintf(stderr, "INFO: Vulkan ICD filter: skipping software ICD '%s'\n", base);
			continue;
		}
		if (found_hw) {
			strncat(hw_icds, ":", sizeof(hw_icds) - strlen(hw_icds) - 1);
		}
		strncat(hw_icds, path, sizeof(hw_icds) - strlen(hw_icds) - 1);
		found_hw = true;
	}
	globfree(&gl);

	if (found_hw) {
		setenv("VK_DRIVER_FILES", hw_icds, 1);
		fprintf(stderr, "INFO: Vulkan ICD filter: VK_DRIVER_FILES=%s\n", hw_icds);
	} else {
		fprintf(stderr, "WARNING: Vulkan ICD filter: no hardware ICDs found, LLVMpipe exclusion skipped\n");
		fprintf(stderr, "WARNING: If startup crashes in libvulkan_lvp.so, set VK_DRIVER_FILES manually\n");
	}
}

/**
 * FilterPipeWireOpenAL
 *
 * Sets ALSOFT_DRIVERS to skip PipeWire, falling back to pulse/alsa.
 *
 * Workaround for openal-soft PipeWire backend crash: alcOpenDevice() segfaults
 * inside the PipeWire backend while opening the default playback device.
 * The crash occurs in PipeWire's stream/context internals and is unrecoverable
 * from userspace. Excluding PipeWire via ALSOFT_DRIVERS causes openal-soft to
 * fall back to the PulseAudio backend, which works correctly on PipeWire systems
 * via the PulseAudio compatibility layer.
 *
 * NOTE: openal-soft reads ALSOFT_DRIVERS from a static global constructor when
 * libopenal.so is loaded by the dynamic linker, which is before main() runs.
 * This function is therefore only effective for builds that use lazy
 * initialization. The authoritative fix is in the launch scripts (run-linux-zh.sh
 * etc.), which set ALSOFT_DRIVERS before the binary starts.
 *
 * Only applied when ALSOFT_DRIVERS is not already set by the user.
 *
 * GeneralsX @bugfix 09/03/2026
 */
static void FilterPipeWireOpenAL()
{
	// GeneralsX @bugfix Copilot 24/03/2026 PipeWire/OpenAL workaround is Linux-only; keep macOS CoreAudio backend selection untouched.
	#if defined(__linux__)
	// Crash: alcOpenDevice() hits 'movaps %xmm1,0x26260(%rbx)' — SSE movaps requires
	// 16-byte alignment; a misaligned ALCdevice struct faults regardless of backend.
	// Disabling CPU extensions forces openal-soft to use scalar code that has no
	// alignment requirements. Also exclude pipewire which has its own crash at
	// device-open time on PipeWire 1.4.x.
	// NOTE: these env vars are authoritative only when set before the binary loads
	// (openal-soft reads them from a static constructor). The launch scripts set them
	// first; this is a best-effort fallback for lazy-init builds.
	if (!getenv("ALSOFT_DISABLE_CPU_EXTS")) {
		setenv("ALSOFT_DISABLE_CPU_EXTS", "all", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DISABLE_CPU_EXTS=all (movaps alignment crash workaround)\n");
	}
	if (!getenv("ALSOFT_DRIVERS")) {
		setenv("ALSOFT_DRIVERS", "pulse,alsa,oss,jack,null,wave", 1);
		fprintf(stderr, "INFO: OpenAL: ALSOFT_DRIVERS=pulse,alsa,oss,jack,null,wave (pipewire excluded)\n");
	}
	#else
	fprintf(stderr, "INFO: OpenAL: keeping default driver selection on non-Linux platform\n");
	#endif
}

/**
 * CreateGameEngine
 *
 * Factory function for SDL3GameEngine on Linux.
 * Called by GameMain() to instantiate platform-specific engine.
 *
 * @return SDL3GameEngine instance
 */
GameEngine *CreateGameEngine(void)
{
	fprintf(stderr, "INFO: CreateGameEngine() - Creating SDL3GameEngine for Linux\n");
	SDL3GameEngine *engine = NEW SDL3GameEngine();
	return engine;
}

#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
/**
 * ConfigureMacAppBundleRuntime
 *
 * GeneralsX @feature Claude 04/11/2026 Self-contained macOS .app support.
 * When the executable lives in <App>.app/Contents/MacOS, point every runtime
 * dependency at the bundle itself so no launcher script or DYLD_* variable is
 * needed: game data (Resources/GameData), the MoltenVK ICD, DXVK/fontconfig
 * config, and per-user cache/log/save locations outside the signed bundle.
 * Every variable is set with overwrite=0, so a developer's own environment
 * still wins. Outside a bundle (deploy/run.sh dev loop) this is a no-op.
 */
static void ConfigureMacAppBundleRuntime()
{
	char rawExe[PATH_MAX] = {0};
	uint32_t rawSize = sizeof(rawExe);
	if (_NSGetExecutablePath(rawExe, &rawSize) != 0) {
		return;
	}
	char exePath[PATH_MAX] = {0};
	if (realpath(rawExe, exePath) == nullptr) {
		return;
	}

	const std::string exe(exePath);
	const std::string marker = ".app/Contents/MacOS/";
	const size_t markerPos = exe.rfind(marker);
	if (markerPos == std::string::npos) {
		return;  // not running from inside an .app bundle
	}
	const std::string resources = exe.substr(0, markerPos) + ".app/Contents/Resources";
	const std::string gameData = resources + "/GameData";
	const char *home = getenv("HOME");

	// Per-user, non-bundle locations (the signed bundle is never written to).
	if (home != nullptr) {
		const std::string cacheDir = std::string(home) + "/Library/Caches/GeneralsX";
		const std::string logDir = std::string(home) + "/Library/Logs/GeneralsX";
		std::error_code ec;
		std::filesystem::create_directories(cacheDir, ec);
		std::filesystem::create_directories(logDir, ec);
		setenv("DXVK_STATE_CACHE_PATH", cacheDir.c_str(), 0);
		setenv("DXVK_LOG_PATH", logDir.c_str(), 0);

		// Launched from Finder: stderr goes nowhere, so keep a log (previous run
		// preserved as .prev.log, matching the iOS diagnostic convention).
		if (!isatty(STDERR_FILENO)) {
			const std::string logPath = logDir + "/GeneralsXZH.log";
			const std::string prevPath = logDir + "/GeneralsXZH.prev.log";
			rename(logPath.c_str(), prevPath.c_str());
			freopen(logPath.c_str(), "w", stderr);
			setvbuf(stderr, nullptr, _IOLBF, 0);
		}
	}

	setenv("DXVK_LOG_LEVEL", "none", 0);
	setenv("DXVK_HUD", "0", 0);

	// MoltenVK ICD, DXVK and fontconfig configuration shipped in Resources.
	const std::string icd = resources + "/MoltenVK_icd.json";
	if (access(icd.c_str(), R_OK) == 0) {
		setenv("VK_ICD_FILENAMES", icd.c_str(), 0);
		setenv("VK_DRIVER_FILES", icd.c_str(), 0);
	}
	const std::string dxvkConf = resources + "/dxvk.conf";
	if (access(dxvkConf.c_str(), R_OK) == 0) {
		setenv("DXVK_CONFIG_FILE", dxvkConf.c_str(), 0);
	}
	const std::string fontsConf = resources + "/fontconfig/fonts.conf";
	if (access(fontsConf.c_str(), R_OK) == 0) {
		setenv("FONTCONFIG_FILE", fontsConf.c_str(), 0);
		setenv("FONTCONFIG_PATH", (resources + "/fontconfig").c_str(), 0);
	}

	// Game data baked into the bundle: resolve it explicitly and run from it,
	// because loose files (Data/INI, Window/...) are looked up relative to cwd.
	if (access(gameData.c_str(), R_OK) == 0) {
		setenv("CNC_GENERALS_ZH_PATH", gameData.c_str(), 0);
		if (chdir(gameData.c_str()) != 0) {
			fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", gameData.c_str(), strerror(errno));
		}

		// Seed default settings on first run (the 2003 GPU auto-detect would
		// otherwise drop unknown GPUs to Low detail).
		if (home != nullptr) {
			const std::string userDataDir = std::string(home) + "/Library/Application Support/GeneralsX/GeneralsZH";
			const std::string optionsPath = userDataDir + "/Options.ini";
			const std::string defaults = resources + "/DefaultOptions.ini";
			if (access(optionsPath.c_str(), F_OK) != 0 && access(defaults.c_str(), R_OK) == 0) {
				std::error_code ec;
				std::filesystem::create_directories(userDataDir, ec);
				std::filesystem::copy_file(defaults, optionsPath, ec);
				if (!ec) {
					fprintf(stderr, "INFO: Seeded default Options.ini\n");
				}
			}
		}
		fprintf(stderr, "INFO: macOS app bundle data: %s\n", gameData.c_str());
	}
}
#endif

/**
 * main
 *
 * Linux entry point (replaces WinMain on Windows).
 * Initializes subsystems and calls GameMain().
 *
 * @param argc Command line argument count
 * @param argv Command line arguments
 * @return Exit code (0 = success)
 */
int main(int argc, char* argv[])
{
	int exitcode = 1;

	// TheSuperHackers @build felipebraz 13/02/2026
	// Store command line arguments in globals for CommandLine.cpp parser
	__argc = argc;
	__argv = argv;

#if defined(__APPLE__) && !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
	ConfigureMacAppBundleRuntime();
#endif

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
	// Diagnostic capture: an icon-launched app's stderr goes nowhere we can read,
	// so mirror it to a file in Library/Caches (purgeable, not user-visible). This
	// lets us pull a full engine log after an on-device session — essential for
	// debugging mode-specific issues (e.g. Generals Challenge radar/scripts) that
	// only the user can reproduce. Pull with: devicectl ... copy from
	// Library/Caches/generals-stderr.log. Remove once the relevant bugs are fixed.
	{
		// Quiet DXVK at the source: the d3d8 layer's per-call warns (e.g. an
		// unimplemented render state set every frame) wrote hundreds of MB per
		// long session. The shipped dxvk.conf also sets logLevel=none; the env
		// covers modules that read it before the config.
		setenv("DXVK_LOG_LEVEL", "none", 0);
		const char *diagHome = getenv("HOME");
		if (diagHome != nullptr) {
			char diagPath[1024];
			char prevPath[1024];
			// Documents, not Library/Caches: Caches is purgeable (a device restart or
			// storage pressure can empty it), and Documents is user-reachable via the
			// Files app since the bundle enables UIFileSharingEnabled.
			snprintf(diagPath, sizeof(diagPath), "%s/Documents/generals-stderr.log", diagHome);
			// Keep the previous session's log: a session that ends in a memory kill
			// leaves no OS crash report, so the prior log is often the only evidence.
			snprintf(prevPath, sizeof(prevPath), "%s/Documents/generals-stderr-prev.log", diagHome);
			rename(diagPath, prevPath);
			// Filtered + capped sink instead of a raw freopen: per-frame debug spam
			// (upstream [GX-ISSUE144] font traces, [INI] loader traces, residual DXVK
			// warns) is dropped, and the file stops growing at 8 MB so a marathon
			// session cannot eat device storage. funopen() is fine here: this is
			// Darwin-only code.
			static int s_logFd = -1;
			s_logFd = open(diagPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			if (s_logFd >= 0) {
				static size_t s_logWritten = 0;
				FILE *sink = funopen(nullptr,
					nullptr,
					[](void *, const char *buf, int len) -> int {
						static const size_t kLogCap = 8u * 1024u * 1024u;
						if (s_logFd < 0) return len;
						if (len > 13 &&
						    (memcmp(buf, "[GX-ISSUE144]", 13) == 0 ||
						     memcmp(buf, "[INI] ", 6) == 0 ||
						     memcmp(buf, "warn:  D3D8De", 13) == 0)) {
							return len;  // drop known per-frame spam, report consumed
						}
						if (s_logWritten >= kLogCap) {
							// past the cap, still record errors — the tail of a dying
							// session is this log's whole reason to exist
							static bool s_capMarked = false;
							if (!s_capMarked) {
								s_capMarked = true;
								const char *mark = "[log capped: non-error lines dropped from here]\n";
								write(s_logFd, mark, strlen(mark));
							}
							if (len > 4 && (memcmp(buf, "err:", 4) == 0 ||
							                memcmp(buf, "ERROR", 5) == 0 ||
							                memcmp(buf, "FATAL", 5) == 0)) {
								write(s_logFd, buf, (size_t)len);
							}
							return len;
						}
						ssize_t w = write(s_logFd, buf, (size_t)len);
						if (w > 0) s_logWritten += (size_t)w;
						return len;
					},
					nullptr, nullptr);
				if (sink != nullptr) {
					*stderr = *sink;  // classic Darwin stderr swap; stderr is a FILE, not a macro here
					setvbuf(stderr, nullptr, _IOLBF, 0);  // line-buffered so a crash still flushes recent lines
				}
			}
		}
	}

	// The engine resolves all game data relative to the working directory.
	// Preferred layout: assets ship read-only INSIDE the signed app bundle
	// (<bundle>/GameData), the iOS-sanctioned home for app resources — the
	// install is then fully self-contained. Dev builds packaged without
	// assets fall back to the Documents folder (Files-app accessible).
	// User data (saves, Options.ini) always lives in Library/Application
	// Support via the engine's user-data path; never in the bundle.
	{
		const char *home = getenv("HOME");

		// <bundle>/GameData, derived from the executable path (argv[0])
		char bundleData[1024] = {0};
		if (argc > 0 && argv[0] != nullptr) {
			const char *slash = strrchr(argv[0], '/');
			if (slash != nullptr) {
				const size_t dirLen = (size_t)(slash - argv[0]);
				if (dirLen < sizeof(bundleData) - 16) {
					memcpy(bundleData, argv[0], dirLen);
					snprintf(bundleData + dirLen, sizeof(bundleData) - dirLen, "/GameData");
				}
			}
		}

		bool usingBundleData = false;
		if (bundleData[0] != '\0' && access(bundleData, R_OK) == 0) {
			if (chdir(bundleData) == 0) {
				usingBundleData = true;
				fprintf(stderr, "INFO: iOS working directory (bundle): %s\n", bundleData);
			}
		}
		if (!usingBundleData && home != nullptr) {
			char docs[1024];
			snprintf(docs, sizeof(docs), "%s/Documents", home);
			if (chdir(docs) != 0) {
				fprintf(stderr, "WARNING: chdir(%s) failed: %s\n", docs, strerror(errno));
			} else {
				fprintf(stderr, "INFO: iOS working directory (Documents): %s\n", docs);
			}
		}

		if (home != nullptr) {
			// Keep DXVK's shader cache in Library/Caches: purgeable under
			// storage pressure, excluded from iCloud backup, invisible in the
			// Files app. Must be set before the d3d8 dylib loads.
			char cacheDir[1024];
			snprintf(cacheDir, sizeof(cacheDir), "%s/Library/Caches", home);
			mkdir(cacheDir, 0755);
			setenv("DXVK_STATE_CACHE_PATH", cacheDir, 0);

			if (usingBundleData) {
				// Seed default settings on first run (full detail instead of the
				// 2003 auto-detect, which drops unknown GPUs to Low).
				char userDataDir[1024], optionsPath[1024];
				snprintf(userDataDir, sizeof(userDataDir),
				         "%s/Library/Application Support/GeneralsX/GeneralsZH", home);
				snprintf(optionsPath, sizeof(optionsPath), "%s/Options.ini", userDataDir);
				if (access(optionsPath, F_OK) != 0 && access("DefaultOptions.ini", R_OK) == 0) {
					std::error_code fsError;
					std::filesystem::create_directories(userDataDir, fsError);
					std::filesystem::copy_file("DefaultOptions.ini", optionsPath, fsError);
					if (!fsError) {
						fprintf(stderr, "INFO: Seeded default Options.ini\n");
					}
				}

				// One-time tidy-up: remove asset copies from Documents now that
				// the bundle carries them. Guarded by a sentinel so it truly runs
				// once — Documents is exposed via the Files app, and anything the
				// user places there later (mods, custom maps) must never be touched.
				// "Maps" is deliberately NOT in the list: it is where user maps live.
				char docs[1024];
				snprintf(docs, sizeof(docs), "%s/Documents", home);
				char sentinel[1024];
				snprintf(sentinel, sizeof(sentinel), "%s/.bundle-assets-tidied", docs);
				if (access(sentinel, F_OK) != 0) {
					std::error_code fsError;
					for (const auto &entry : std::filesystem::directory_iterator(docs, fsError)) {
						const std::string name = entry.path().filename().string();
						const bool isShippedAsset =
							(name.size() > 4 && name.compare(name.size() - 4, 4, ".big") == 0) ||
							name == "Data" || name == "Window" || name == "ZH_Generals" ||
							name == "fonts" || name == "_CommonRedist" ||
							name == "dxvk.conf" || name == "GeneralsXZH.dxvk-cache" ||
							name == "GeneralsXZH_d3d9.log";
						if (isShippedAsset) {
							fprintf(stderr, "INFO: tidy-up removing shipped asset copy: %s\n", name.c_str());
							std::error_code removeError;
							std::filesystem::remove_all(entry.path(), removeError);
						}
					}
					if (!fsError) {  // a failed scan must retry next launch, not fail closed forever
						FILE *s = fopen(sentinel, "w");
						if (s) fclose(s);
					}
				}
			}
		}
	}
#endif

	fprintf(stderr, "=================================================\n");
	fprintf(stderr, " Command & Conquer Generals: Zero Hour (Linux)\n");
	fprintf(stderr, " SDL3 + DXVK Build\n");
	fprintf(stderr, "=================================================\n\n");

	try {
		// Initialize critical sections (required by game engine)
		TheAsciiStringCriticalSection = &critSec1;
		TheUnicodeStringCriticalSection = &critSec2;
		TheDmaCriticalSection = &critSec3;
		TheMemoryPoolCriticalSection = &critSec4;
		TheDebugLogCriticalSection = &critSec5;

		// Initialize memory manager early (required by NEW operator)
		initMemoryManager();

		// GeneralsX @bugfix BenderAI 14/02/2026 Initialize Version singleton
		// GameEngine::init() calls updateWindowTitle() which uses TheVersion
		// Must be created before GameMain() to avoid nullptr dereference
		TheVersion = NEW Version;

		// Parse command line (CommandLine class handles argc/argv internally)
		// TheSuperHackers @build felipebraz 10/02/2026 Phase 1.5
		// Store argc/argv for CommandLine parser to access via _NSGetArgc/_NSGetArgv or /proc/self/cmdline
		// For now, let CommandLine::parseCommandLineForStartup() handle this
		CommandLine::parseCommandLineForStartup();

		// GeneralsX @bugfix Copilot 17/05/2026 Skip SDL3 window bootstrap for CLI/headless replay execution.
		const bool isHeadlessMode = (TheGlobalData != nullptr && TheGlobalData->m_headless);
		if (isHeadlessMode) {
			fprintf(stderr, "INFO: Headless mode detected, skipping SDL3 video/Vulkan window initialization\n");
		} else {

		// GeneralsX @bugfix felipebraz 16/02/2026
		// Initialize SDL3 and Vulkan BEFORE creating GameEngine (fighter19 pattern)
		// This prevents LLVM SIGSEGV crash during Vulkan driver enumeration
		// Must be done here, not in SDL3GameEngine::init() which is too late
		fprintf(stderr, "INFO: Initializing SDL3 video subsystem...\n");
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// All mouse events are synthesized by the gesture translator in
		// SDL3GameEngine.cpp; SDL's automatic touch->mouse synthesis would
		// double-deliver finger 1 and fight the two-finger pan logic.
		SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
			fprintf(stderr, "FATAL: Failed to initialize SDL3: %s\n", SDL_GetError());
			return 1;
		}

		// Set DXVK WSI driver before loading Vulkan
		setenv("DXVK_WSI_DRIVER", "SDL3", 1);

		// GeneralsX @bugfix BenderAI 06/03/2026 - Exclude LLVMpipe Vulkan ICD before loading Vulkan.
		// libvulkan_lvp.so crashes during static initialization with LLVM 20.x when the Vulkan
		// loader enumerates all ICDs. Restrict to hardware ICDs first.
		FilterSoftwareVulkanICDs();
		FilterPipeWireOpenAL();

		// Load Vulkan library for DXVK DirectX8→Vulkan translation
		fprintf(stderr, "INFO: Loading Vulkan library...\n");
		if (!SDL_Vulkan_LoadLibrary(nullptr)) {
			fprintf(stderr, "WARNING: Failed to load Vulkan: %s\n", SDL_GetError());
			fprintf(stderr, "WARNING: Continuing without Vulkan (may use software rendering)\n");
		}

		// Create SDL3 window with Vulkan support
		fprintf(stderr, "INFO: Creating SDL3 Vulkan window...\n");
		Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;  // Start hidden, show after D3D init
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Request a native-resolution Metal drawable (e.g. 2868x1320 instead of the
		// 956x440 point size). Without this the swapchain renders at point size and
		// the display upscales 3x, visibly blurring textures and terrain.
		windowFlags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
		TheSDL3Window = SDL_CreateWindow(
			"Command & Conquer Generals: Zero Hour",
			1024, 768,  // Default resolution
			windowFlags
		);

		if (!TheSDL3Window) {
			fprintf(stderr, "FATAL: Failed to create SDL3 window: %s\n", SDL_GetError());
			SDL_Quit();
			return 1;
		}

		// Store window handle globally (cast SDL_Window* to HWND for compatibility)
		ApplicationHWnd = (HWND)TheSDL3Window;
		fprintf(stderr, "INFO: SDL3 window created successfully\n");

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
		// Match the game's internal resolution to the phone screen's aspect ratio.
		// Without this the engine runs its 4:3 default inside the 19.5:9 display:
		// pillarboxed picture and a skewed window->game coordinate mapping. Height
		// stays at the engine's 600px design baseline (UI layouts assume >= 600);
		// width follows the real aspect. Injected as -xres/-yres argv entries so
		// the normal command-line path applies them (user-passed flags still win
		// because the parser lets later arguments override earlier ones... ours go
		// last, so only add them if the user didn't pass explicit -xres/-yres).
		{
			bool userSetRes = false;
			for (int i = 1; i < __argc; ++i) {
				if (strcmp(__argv[i], "-xres") == 0 || strcmp(__argv[i], "-yres") == 0) {
					userSetRes = true;
					break;
				}
			}
			// Use the pixel size of the high-density drawable: the game renders
			// 1:1 into the native-resolution swapchain, and fonts/UI rescale via
			// the engine's resolution-aware font scaling (GlobalLanguage).
			int winW = 0, winH = 0;
			SDL_GetWindowSizeInPixels(TheSDL3Window, &winW, &winH);
			if (!userSetRes && winW > 0 && winH > 0 && winW > winH) {
				static char xresVal[16], yresVal[16];
				static char xresFlag[] = "-xres";
				static char yresFlag[] = "-yres";
				const int yres = winH;
				int xres = winW;
				xres &= ~1;  // keep it even
				snprintf(xresVal, sizeof(xresVal), "%d", xres);
				snprintf(yresVal, sizeof(yresVal), "%d", yres);

				static char* newArgv[64];
				int n = 0;
				for (int i = 0; i < __argc && n < 59; ++i) {
					newArgv[n++] = __argv[i];
				}
				newArgv[n++] = xresFlag;
				newArgv[n++] = xresVal;
				newArgv[n++] = yresFlag;
				newArgv[n++] = yresVal;
				newArgv[n] = nullptr;
				__argv = newArgv;
				__argc = n;
				fprintf(stderr, "INFO: iOS internal resolution set to %sx%s (window %dx%d)\n",
				        xresVal, yresVal, winW, winH);
			}
		}
#endif
		}

		// Call cross-platform game entry point
		exitcode = GameMain();

		fprintf(stderr, "INFO: GameMain() returned with code %d\n", exitcode);

	} catch (const std::exception& e) {
		fprintf(stderr, "FATAL: Unhandled exception in main(): %s\n", e.what());
		exitcode = 1;
	} catch (...) {
		fprintf(stderr, "FATAL: Unknown exception in main()\n");
		exitcode = 1;
	}

	// Cleanup SDL3 resources
	if (TheSDL3Window) {
		SDL_DestroyWindow(TheSDL3Window);
		TheSDL3Window = nullptr;
		ApplicationHWnd = nullptr;
	}
	SDL_Quit();

	// GeneralsX @bugfix BenderAI 14/02/2026 Cleanup Version singleton
	if (TheVersion) {
		delete TheVersion;
		TheVersion = nullptr;
	}

	// GeneralsX @bugfix BenderAI 19/02/2026 Shutdown memory manager BEFORE nulling critical
	// sections. Without this, global pool destructors (ObjectPoolClass) crash during atexit()
	// because they call ::operator delete after the memory manager is already gone (SIGSEGV).
	// Matches WinMain.cpp cleanup order: TheVersion -> shutdownMemoryManager -> null critSecs.
	shutdownMemoryManager();

	// Cleanup critical sections (after memory manager, which may use them during shutdown)
	TheAsciiStringCriticalSection = nullptr;
	TheUnicodeStringCriticalSection = nullptr;
	TheDmaCriticalSection = nullptr;
	TheMemoryPoolCriticalSection = nullptr;
	TheDebugLogCriticalSection = nullptr;

	fprintf(stderr, "\nExiting with code %d\n", exitcode);

	// GeneralsX @bugfix BenderAI 25/02/2026 — use _exit() to skip C++ global destructors.
	// On macOS, __cxa_finalize_ranges runs ObjectPoolClass<X,256> global dtors after main() returns.
	// Those dtors crash with a corrupted BlockListHead (SIGSEGV at 0x4ade32ec4ade0018) because
	// pool block memory was already reused/overwritten during game shutdown.
	// Windows never had this problem — ExitProcess() terminates without running C++ global dtors.
	// _exit() matches that behavior. Explicit cleanup already done above (SDL_Quit, shutdownMemoryManager).
	_exit(exitcode);
}

#endif // !_WIN32
