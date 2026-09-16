// 01 - the smallest program that goes through the whole pipeline.
//
// Ceres-C has no standard library and no preprocessor, so there is no <stdio.h> to include and no
// printf to call. A character reaches the terminal the same way a hand-written CASM program does
// it: by storing a byte into the TerminalDevice's output register at 0xFF000004 (CeresASM's
// docs/07-IO-Devices-and-Ports.md). Every example here starts with the same two or three lines,
// on purpose - nothing is hidden behind a runtime.
//
// `main` does not return to anyone: the generated code halts the machine through the
// SystemControlDevice instead, so `return 0` here only means "stop", never an exit code.
//
//     ceresc examples/01_return_constant.c --run

void put(char c)
{
    char* terminal = (char*)0xFF000004;
    *terminal = c;
}

int main(void)
{
    put('4');
    put('2');
    put('\n');
    return 0;
}
