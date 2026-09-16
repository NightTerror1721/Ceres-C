// A header, and the three things the preprocessor in this version can do with one.
//
// `#pragma once` is load-bearing here rather than stylistic: without `#ifndef` - which this version
// does not have - it is the ONLY way to stop a header included twice from declaring everything
// twice. See docs/08-Preprocessor.md.

#pragma once

// An object-like macro. Macros with arguments are not supported; this is a name for a number, and
// that covers most of what a header uses one for.
#define TERMINAL_OUT 0xFF000004

// Prototypes. A function has external linkage by default, so nothing needs to be said about it
// beyond its shape - which is exactly what the other units need to know.
void put(char c);
void putstr(const char* text);
void putint(int value);

// A variable another unit defines. `extern` is what makes this a declaration rather than a second
// definition: it reserves no storage, it just says the name exists somewhere.
extern int writeCount;
