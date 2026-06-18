/*
OBS Advanced Multiview - NDI runtime loader (issue #11)

Copyright (C) 2025 VTB-LINK
License: GPL-2.0-or-later
*/

#ifdef AMV_ENABLE_NDI_OUTPUT

#include "multiview-ndi-runtime.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QByteArray>
#include <QLibrary>
#include <QString>
#include <QStringList>

#include <mutex>

namespace {

/* NDI 5 uses its own runtime-dir env var; the headers only define the v6 one
 * (NDILIB_REDIST_FOLDER). Try v6 first, then v5, then the bare library name so
 * the OS loader resolves it from the standard search path (PATH on Windows,
 * DYLD/LD paths on macOS/Linux). Cross-platform via Qt's QLibrary, which adds
 * the right prefix/suffix handling per platform (the same approach DistroAV
 * uses). */
constexpr const char *kRuntimeDirEnvV6 = NDILIB_REDIST_FOLDER; /* "NDI_RUNTIME_DIR_V6" */
constexpr const char *kRuntimeDirEnvV5 = "NDI_RUNTIME_DIR_V5";

/* Candidate full paths to try, most specific first. */
QStringList candidate_paths()
{
	const QString libName = QString::fromUtf8(NDILIB_LIBRARY_NAME);
	QStringList paths;

	for (const char *envName : {kRuntimeDirEnvV6, kRuntimeDirEnvV5}) {
		const QByteArray dir = qgetenv(envName);
		if (!dir.isEmpty())
			paths << (QString::fromLocal8Bit(dir) + QLatin1Char('/') + libName);
	}

#if defined(Q_OS_MACOS)
	/* The NDI runtime/Tools installer symlinks the dylib into these dirs. */
	paths << QStringLiteral("/usr/local/lib/") + libName;
	paths << QStringLiteral("/Library/NDI SDK for Apple/lib/macOS/") + libName;
#elif defined(Q_OS_LINUX)
	/* Common locations the NDI redist / SDK drops libndi.so on Linux. */
	paths << QStringLiteral("/usr/lib/") + libName;
	paths << QStringLiteral("/usr/lib/x86_64-linux-gnu/") + libName;
	paths << QStringLiteral("/usr/lib64/") + libName;
	paths << QStringLiteral("/usr/local/lib/") + libName;
#endif

	/* Bare name last: let the OS loader search its standard paths. */
	paths << libName;
	return paths;
}

/* Locate and load the NDI runtime library. Returns a loaded QLibrary (caller
 * owns it) or nullptr. Does NOT resolve the table or initialize. */
QLibrary *load_runtime_library()
{
	for (const QString &path : candidate_paths()) {
		auto *lib = new QLibrary(path);
		if (lib->load())
			return lib;
		delete lib;
	}
	return nullptr;
}

} /* anonymous namespace */

std::shared_ptr<NdiRuntime> NdiRuntime::acquire()
{
	static std::mutex mtx;
	static std::weak_ptr<NdiRuntime> cached;
	static bool warned_load_failed = false;

	std::lock_guard<std::mutex> lock(mtx);

	if (auto sp = cached.lock())
		return sp;

	QLibrary *lib_handle = load_runtime_library();
	if (!lib_handle) {
		if (!warned_load_failed) {
			obs_log(LOG_WARNING,
				"[multiview-output/ndi] NDI runtime not found ('%s'). "
				"Install the NDI runtime from %s to enable NDI output.",
				NDILIB_LIBRARY_NAME, NDILIB_REDIST_URL);
			warned_load_failed = true;
		}
		return nullptr;
	}

	/* v5 table for v5+v6 runtime compatibility (see header). */
	using load_fn_t = const NDIlib_v5 *(*)(void);
	auto load_fn = reinterpret_cast<load_fn_t>(lib_handle->resolve("NDIlib_v5_load"));
	const NDIlib_v5 *lib = load_fn ? load_fn() : nullptr;
	if (!lib) {
		obs_log(LOG_WARNING, "[multiview-output/ndi] NDIlib_v5_load missing or returned null");
		lib_handle->unload();
		delete lib_handle;
		return nullptr;
	}

	if (!lib->initialize()) {
		/* Almost always an unsupported (too old) CPU. */
		obs_log(LOG_WARNING, "[multiview-output/ndi] NDIlib initialize failed (unsupported CPU?)");
		lib_handle->unload();
		delete lib_handle;
		return nullptr;
	}

	warned_load_failed = false;
	obs_log(LOG_INFO, "[multiview-output/ndi] NDI runtime loaded ('%s')", lib->version());

	auto sp = std::shared_ptr<NdiRuntime>(new NdiRuntime(lib_handle, lib));
	cached = sp;
	return sp;
}

bool NdiRuntime::available()
{
	/* The NDI runtime's presence is fixed for the session, so probe once and
	 * cache the result. reconcile() reaches this every frame while NDI output
	 * is enabled (via MultiviewOutputManager::backend_available) — we don't
	 * want a load probe per frame. A runtime installed mid-session is picked up
	 * on the next restart (matching how OBS NDI plugins resolve it at load). */
	static std::mutex mtx;
	static int cached = -1;

	std::lock_guard<std::mutex> lock(mtx);
	if (cached >= 0)
		return cached != 0;

	bool ok = false;
	if (QLibrary *probe = load_runtime_library()) {
		ok = probe->resolve("NDIlib_v5_load") != nullptr;
		probe->unload();
		delete probe;
	}

	cached = ok ? 1 : 0;
	return ok;
}

NdiRuntime::~NdiRuntime()
{
	/* All senders created from this table must already be destroyed (backends
	 * release their shared_ptr only after send_destroy). */
	if (lib_)
		lib_->destroy();
	if (module_) {
		auto *lib_handle = static_cast<QLibrary *>(module_);
		lib_handle->unload();
		delete lib_handle;
	}
}

#endif /* AMV_ENABLE_NDI_OUTPUT */
