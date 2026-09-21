#include <ceresc/driver/ceres_locator.h>

#include <cstdlib>
#include <format>
#include <string_view>
#include <vector>

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
			"  PATH           {} director{}, none holds `{}`\n"
			"Set CERES_PATH to the directory that holds it, put that directory on PATH, or pass --ceres-path.\n",
			kExecutableName, directories.size(), directories.size() == 1 ? "y" : "ies", kExecutableName);
		return lookup;
	}
}
