#include <ceresc/version.h>
#include <ceresc/driver/driver.h>
#include <ceresc/driver/options.h>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string_view>
#include <vector>

// The whole binary: parse the command line, hand it to libs/driver, return what it says.
//
// Fase 0 shipped `--version` as the only job of this file, to prove the tree configures, links and
// runs end to end. It is now an ordinary flag like every other one (libs/driver/src/options.cpp),
// which is why it works anywhere on the line instead of only as argv[1] - the only thing left here
// is printing it, since the version string is the application's, not the driver library's.
//
// Three exit codes, and nothing else decides them: 0 when the pipeline succeeded or the user asked
// a question (--help/--version), 1 when the command line was malformed, and whatever
// driver::run() returns otherwise - which passes a `ceres asm`/`ceres run` failure straight
// through (§11).
int main(int argc, char** argv)
{
	std::vector<const char*> args(argv + 1, argv + argc);

	// No arguments at all is a request for help, not an error: print usage and succeed.
	if (args.empty())
	{
		ceresc::driver::printUsage(std::cout);
		return 0;
	}

	bool helpRequested = false;
	for (const char* arg : args)
		helpRequested = helpRequested || std::string_view(arg) == "--help" || std::string_view(arg) == "-h";

	if (helpRequested)
	{
		ceresc::driver::printUsage(std::cout);
		return 0;
	}

	std::optional<ceresc::driver::Options> options = ceresc::driver::parseOptions(args, std::cerr);
	if (!options)
		return 1;

	if (options->showVersion)
	{
		std::printf("ceresc %s\n", ceresc::kVersionString);
		return 0;
	}

	return ceresc::driver::run(*options);
}
