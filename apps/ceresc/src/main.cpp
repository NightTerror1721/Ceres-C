#include <ceresc/version.h>
#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

// Fase 0 shipped `--version` as the only job of this binary, to prove the whole tree configures,
// links and runs end to end. Every other flag from §11 (--emit-ast, --emit-ir, --run, ...) now
// goes straight to libs/driver, starting Fase 6.
int main(int argc, char** argv)
{
	if (argc > 1 && std::string_view(argv[1]) == "--version")
	{
		std::printf("ceresc %s\n", ceresc::kVersionString);
		return 0;
	}

	std::vector<const char*> args(argv + 1, argv + argc);
	std::optional<ceresc::driver::Options> options = ceresc::driver::parseOptions(args, std::cerr);
	if (!options)
	{
		bool helpRequested = false;
		for (const char* arg : args)
			helpRequested = helpRequested || std::string_view(arg) == "--help" || std::string_view(arg) == "-h";
		return (argc <= 1 || helpRequested) ? 0 : 1;
	}

	return ceresc::driver::run(*options);
}
