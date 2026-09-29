#include <ceresc/driver/ceres_locator.h>

#include <cstdlib>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#elif defined(__APPLE__)
#	include <mach-o/dyld.h>
#	include <cstdint>
#endif

namespace ceresc::driver
{
	namespace
	{
		namespace fs = std::filesystem;

#if defined(_WIN32)
		constexpr std::string_view kExecutableName = "ceres.exe";
		constexpr char kPathSeparator = ';';
#else
		constexpr std::string_view kExecutableName = "ceres";
		constexpr char kPathSeparator = ':';
#endif

		bool isExecutableFile(const fs::path& path)
		{
			std::error_code error;
			if (!fs::is_regular_file(path, error))
				return false;
#if defined(_WIN32)
			return true;
#else
			const fs::perms permissions = fs::status(path, error).permissions();
			return !error && (permissions & (fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec)) != fs::perms::none;
#endif
		}

		fs::path absolutePath(const fs::path& path)
		{
			std::error_code error;
			fs::path result = fs::absolute(path, error);
			return (error ? path : result).lexically_normal();
		}

		// What a --ceres-path or a CERES_PATH names: the executable itself, or the directory holding it.
		std::optional<fs::path> resolveNamed(const fs::path& given)
		{
			if (isExecutableFile(given))
				return absolutePath(given);
			std::error_code error;
			if (fs::is_directory(given, error) && isExecutableFile(given / kExecutableName))
				return absolutePath(given / kExecutableName);
			return std::nullopt;
		}

		// The installation directory a --ceres-path or a CERES_PATH names: the directory, or the one a file is in.
		fs::path installDirectory(const fs::path& given)
		{
			std::error_code error;
			return absolutePath(fs::is_regular_file(given, error) ? given.parent_path() : given);
		}

		// The running ceresc, or empty when the system does not say.
		fs::path executablePath()
		{
#if defined(_WIN32)
			std::wstring buffer(MAX_PATH, L'\0');
			for (;;)
			{
				const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
				if (length == 0)
					return {};
				if (length < buffer.size())
				{
					buffer.resize(length);
					return fs::path(buffer);
				}
				buffer.resize(buffer.size() * 2);
			}
#elif defined(__APPLE__)
			std::uint32_t size = 0;
			_NSGetExecutablePath(nullptr, &size);
			std::vector<char> buffer(size + 1, '\0');
			if (_NSGetExecutablePath(buffer.data(), &size) != 0)
				return {};
			std::error_code error;
			const fs::path canonical = fs::canonical(buffer.data(), error);
			return error ? fs::path(buffer.data()) : canonical;
#else
			std::error_code error;
			const fs::path path = fs::read_symlink("/proc/self/exe", error);
			return error ? fs::path{} : path;
#endif
		}

		std::vector<std::string> splitPath(std::string_view path)
		{
			std::vector<std::string> directories;
			std::string current;
			auto flush = [&]
			{
				// A Windows PATH may quote an entry that has a space in it.
				if (current.size() >= 2 && current.front() == '"' && current.back() == '"')
					current = current.substr(1, current.size() - 2);
				if (!current.empty())
					directories.push_back(current);
				current.clear();
			};
			for (char c : path)
			{
				if (c == kPathSeparator)
					flush();
				else
					current += c;
			}
			flush();
			return directories;
		}
	}

	CeresEnvironment CeresEnvironment::fromProcess()
	{
		CeresEnvironment environment;
		if (const char* value = std::getenv("CERES_PATH"))
			environment.ceresPath = value;
		if (const char* value = std::getenv("PATH"))
			environment.path = value;
		if (const fs::path executable = executablePath(); !executable.empty())
			environment.executableDirectory = executable.parent_path();
		return environment;
	}

	CeresLookup locateCeres(std::string_view explicitPath, const CeresEnvironment& environment)
	{
		CeresLookup lookup;

		// Resolves a place that was given, and decides the whole lookup with it either way.
		auto named = [&](std::string_view given, std::string_view label)
		{
			if (std::optional<fs::path> found = resolveNamed(fs::path(std::string(given))))
			{
				lookup.executable = *found;
				lookup.source = std::string(label);
				return;
			}
			lookup.error = std::format(
				"ceresc: {} names '{}', which is neither the `{}` executable nor a directory that holds it\n",
				label, given, kExecutableName);
		};

		if (!explicitPath.empty())
		{
			named(explicitPath, "--ceres-path");
			return lookup;
		}
		if (environment.ceresPath && !environment.ceresPath->empty())
		{
			named(*environment.ceresPath, "CERES_PATH");
			if (!lookup.found())
				lookup.error += "  (unset CERES_PATH to fall back to the PATH search, or pass --ceres-path to override it)\n";
			return lookup;
		}

		if (!environment.executableDirectory.empty() && isExecutableFile(environment.executableDirectory / kExecutableName))
		{
			lookup.executable = absolutePath(environment.executableDirectory / kExecutableName);
			lookup.source = "ceresc's directory";
			return lookup;
		}

		const std::vector<std::string> directories = splitPath(environment.path);
		for (const std::string& directory : directories)
		{
			fs::path candidate = fs::path(directory) / kExecutableName;
			if (isExecutableFile(candidate))
			{
				lookup.executable = absolutePath(candidate);
				lookup.source = "PATH";
				return lookup;
			}
		}

		lookup.error = std::format(
			"ceresc: cannot find the `{}` executable, which --run needs. Looked in:\n"
			"  --ceres-path   not given\n"
			"  CERES_PATH     not set\n"
			"  ceresc's own directory{}\n"
			"  PATH           {} director{}, none holds `{}`\n"
			"Set CERES_PATH to the directory Ceres is installed in, put the one that holds it on PATH, or pass --ceres-path.\n",
			kExecutableName,
			environment.executableDirectory.empty() ? std::string(" (unknown)") : " " + environment.executableDirectory.string(),
			directories.size(), directories.size() == 1 ? "y" : "ies", kExecutableName);
		return lookup;
	}

	StdlibLookup locateStdlib(std::string_view explicitPath, const CeresEnvironment& environment)
	{
		// Where to look, with what says so: the flag alone when it is given, else CERES_PATH and ceresc's directory.
		std::vector<std::pair<fs::path, std::string_view>> places;
		if (!explicitPath.empty())
			places.emplace_back(installDirectory(fs::path(std::string(explicitPath))), "--ceres-path");
		else
		{
			if (environment.ceresPath && !environment.ceresPath->empty())
				places.emplace_back(installDirectory(fs::path(*environment.ceresPath)), "CERES_PATH");
			if (!environment.executableDirectory.empty())
				places.emplace_back(absolutePath(environment.executableDirectory), "ceresc's directory");
		}

		StdlibLookup lookup;
		for (const auto& [directory, label] : places)
		{
			const fs::path include = directory / "stdlib" / "include";
			std::error_code error;
			if (fs::is_directory(include, error))
			{
				lookup.includeDirectory = include;
				lookup.libraryDirectory = directory / "stdlib" / "lib";
				return lookup;
			}
		}

		lookup.error = "ceresc: --stdlib: cannot find the C library (stdlib/include in the directory Ceres is installed in).";
		if (places.empty())
			lookup.error += " CERES_PATH is not set, and where ceresc is is unknown.\n";
		else
		{
			lookup.error += " Looked in:\n";
			for (const auto& [directory, label] : places)
				lookup.error += std::format("  {:<20} {}\n", label, directory.string());
		}
		lookup.error += "Set CERES_PATH to the directory Ceres is installed in, or pass --ceres-path.\n";
		return lookup;
	}
}
