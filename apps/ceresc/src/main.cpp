#include <ceresc/version.h>

#include <cstdio>
#include <string_view>

// Fase 0 (see the architecture plan, §13): the only job of this binary today is to prove the
// whole tree configures, links and runs end to end. `--version` is the explicit Fase 0
// deliverable; every other flag from §11 (--emit-ast, --emit-ir, --run, ...) arrives together
// with libs/driver's real implementation, starting Fase 6.
int main(int argc, char** argv)
{
	const std::string_view arg = argc > 1 ? std::string_view(argv[1]) : std::string_view{};

	if (arg.empty() || arg == "--version")
	{
		std::printf("ceresc %s\n", ceresc::kVersionString);
		return 0;
	}

	std::fprintf(stderr, "ceresc: unrecognized option '%.*s'\n", static_cast<int>(arg.size()), arg.data());
	std::fprintf(stderr, "usage: ceresc --version\n");
	return 1;
}
