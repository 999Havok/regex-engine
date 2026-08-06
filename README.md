# regex-engine

A small regular-expression engine written from scratch in C99, with **no
backtracking**: a pattern is compiled directly into a minimised DFA, so
matching runs in worst-case time linear in the length of the input — a
pathological pattern like `(a+)+b` that makes backtracking engines explode
resolves in microseconds here.

The only dependency is libc.

---

## Contents

- [The one idea](#the-one-idea)
- [How it works: a worked example](#how-it-works-a-worked-example)
- [Why direct DFA construction (not Thompson NFA + subset construction)](#why-direct-dfa-construction)
- [Benchmarks](#benchmarks)
- [Building and using it](#building-and-using-it)
- [Supported syntax](#supported-syntax)
- [What is deliberately not supported, and why](#what-is-deliberately-not-supported-and-why)
- [Project layout](#project-layout)
- [Limitations and honesty](#limitations-and-honesty)

---

## The one idea

A regular expression describes a *regular language*, and every regular language
is recognised by a **deterministic finite automaton** (DFA): a fixed set of
states, and from each state exactly one transition per input byte. Matching with
a DFA is then trivial — start in the start state, follow one transition per input
byte, and check whether you end in an accepting state. That is one array lookup
per byte, no matter how the pattern is shaped, so matching is **O(length of
input)** and cannot backtrack.

The interesting part is turning the pattern text into that DFA. This engine uses
the *direct* construction of Aho, Sethi & Ullman (the "Dragon Book", §3.9),
which builds the DFA straight from the syntax tree, skipping the usual
intermediate NFA and its ε-transitions entirely.

## How it works: a worked example

The pipeline has five stages:

```
pattern text
   │  parser.c        recursive descent
   ▼
syntax tree
   │  positions.c     nullable / firstpos / lastpos / followpos
   ▼
follow tables
   │  dfa.c           subset construction
   ▼
DFA
   │  minimize.c      Hopcroft
   ▼
minimised DFA  ──  match.c  ──▶  match / search
```

Let's trace the textbook pattern `(a|b)*abb`. Everything below is produced by the
`regexdump` tool (`./build/regexdump '(a|b)*abb'`), so it is guaranteed to match
the code.

**1. Parse, and augment with an endmarker `#`.** Each leaf that consumes input
gets a numbered *position*. The endmarker (position 6) is a synthetic leaf that
marks "a full match ends here".

```
CONCAT
  CONCAT
    CONCAT
      STAR
        ALT
          CHARSET pos=0 [a]      position 1
          CHARSET pos=1 [b]      position 2
      CHARSET pos=2 [a]          position 3
    CHARSET pos=3 [b]            position 4
  CHARSET pos=4 [b]              position 5
  END # pos=5                    position 6
```

(Positions are printed 1-based below, following the book's convention.)

**2. Compute `nullable`, `firstpos`, `lastpos` for every node.**

- `nullable(n)` — can this subexpression match the empty string?
- `firstpos(n)` — which positions can be the *first* symbol matched?
- `lastpos(n)` — which positions can be the *last* symbol matched?

```
node     kind     nullable  firstpos    lastpos
n2       alt         0       {1, 2}      {1, 2}
n3       star        1       {1, 2}      {1, 2}
n5       concat      0       {1, 2, 3}   {3}
n7       concat      0       {1, 2, 3}   {4}
n9       concat      0       {1, 2, 3}   {5}     (root)
```

The root's `firstpos` `{1,2,3}` becomes the DFA's start state.

**3. Build the `followpos` table.** `followpos(i)` is the set of positions that
can immediately follow position `i` somewhere in the language. Two rules
generate it:

- for a concatenation `c₁c₂`, every position in `lastpos(c₁)` is followed by all
  of `firstpos(c₂)`;
- for a star `(c)*`, every position in `lastpos(c)` is followed by all of
  `firstpos(c)` (the loop-back edge).

```
position       followpos
1 (a)          {1, 2, 3}
2 (b)          {1, 2, 3}
3 (a)          {4}
4 (b)          {5}
5 (b)          {6}
6 (#)          {}
```

**4. Subset construction.** A DFA state *is* a set of positions. The start state
is the root's `firstpos`. From a state, the move on a byte `x` is the union of
`followpos(p)` over every position `p` in the state whose symbol is `x`. Any
state containing the endmarker (position 6) is accepting.

```
state          a    b   position set
>1        2    1   {1, 2, 3}
 2        2    3   {1, 2, 3, 4}
 3        2    4   {1, 2, 3, 5}
 4   *    2    1   {1, 2, 3, 6}
(> = start, * = accepting; state 0 is the dead state)
```

Read the accepting row: on `a` go to state 2, on `b` back to state 1 — exactly
the machine that recognises "any string of a's and b's ending in `abb`".

**5. Hopcroft minimisation** merges states that cannot be distinguished by any
future input. For `(a|b)*abb` the subset construction was already minimal (5
states including the dead state, 5 after), but for many patterns it is not —
e.g. `(a*b*)+` shrinks from 4 states to 2, and `(ab|cd)+` from 7 to 5. The test
suite asserts these reductions.

## Why direct DFA construction

The more commonly taught route is **Thompson construction** (regex → NFA with
ε-transitions) followed by **subset construction** (NFA → DFA), which needs an
ε-closure computation at every step.

The direct method reaches the same DFA but skips the NFA:

- **No ε-transitions and no ε-closure.** `followpos` already encodes exactly the
  "what can come next" information that ε-closure recovers at runtime, computed
  once at compile time from `firstpos`/`lastpos`.
- **Fewer moving parts.** There is no intermediate NFA data structure to build,
  store, and walk; subset construction reads the `followpos` table directly.
- **It maps cleanly onto bitsets.** Positions are small integers, so states are
  just position sets — a bitset — and a "move" is a handful of bitset unions.

The trade-off is that the direct method is specific to converting a *parsed*
regex; Thompson NFAs compose more freely (you can glue two NFAs together). For a
compile-once, match-many engine that composability doesn't matter, so the
simpler direct construction wins.

Either way the crucial property is the same and is the whole point: the result
is a **DFA**, which matches in linear time without backtracking.

## Benchmarks

Generated by `make bench` on an Apple M-series laptop (Apple clang, `-O2`).
Times are single runs — read them as orders of magnitude. The comparison is
against the system POSIX `<regex.h>`, plus a deliberately naive backtracking
matcher (`bench/backtrack.c`) included as a foil.

### 1. The catastrophic case: `(a+)+b` on `"aa…a!"`

`n` a's followed by `!`, which never matches — the input that makes backtracking
engines melt.

| n (a's) | ours (ms) | POSIX (ms) | naive backtracking (ms) |
|--------:|----------:|-----------:|------------------------:|
| 10 | 0.0000 | 0.002 | 0.052 |
| 14 | 0.0000 | 0.001 | 0.838 |
| 18 | 0.0000 | 0.001 | 11.268 |
| 20 | 0.0000 | 0.002 | 32.838 |
| 22 | 0.0000 | 0.001 | 96.538 |
| 24 | 0.0000 | 0.001 | 370.588 |
| 26 | 0.0000 | 0.001 | budget exceeded |

The naive backtracker roughly triples every time `n` grows by 2 — textbook
exponential blowup — and blows its step budget by `n = 26`. Our engine and POSIX
stay flat.

**An important honesty note:** the system POSIX regex does *not* blow up here
either. Both macOS's BSD libc and glibc implement POSIX regex with automaton
simulation, *not* backtracking, so they share the immunity. The engines that
actually explode on `(a+)+b` are the backtracking ones — PCRE, Perl, Python's
`re`, Java `java.util.regex`, JavaScript — which is why the foil is here to make
the failure mode visible. This project's contribution is not "faster than
POSIX"; it is showing the direct-DFA construction end to end, with the linear
guarantee that rules the blowup out by design.

### 2. Ambiguous repetition: `(a|aa)+$` on a long run of a's

| n (a's) | ours (ms) | POSIX (ms) |
|--------:|----------:|-----------:|
| 16 | 0.0000 | 0.001 |
| 24 | 0.0000 | 0.001 |
| 32 | 0.0000 | 0.001 |
| 40 | 0.0000 | 0.001 |

Another pattern with exponentially many ways to split the input; both
automaton-based engines shrug.

### 3. The common case: full match of `([a-z]+ )*[a-z]+` on a large input

POSIX is anchored (`^…$`) so it verifies the whole string, like our full match.

| input size | ours (ms) | POSIX (ms) |
|-----------:|----------:|-----------:|
| 262 144 bytes | 0.664 | 12.705 |
| 524 288 bytes | 1.346 | 26.494 |
| 1 048 576 bytes | 3.075 | 51.830 |
| 2 097 152 bytes | 5.556 | 105.990 |

Both are linear in the input; our per-byte constant happens to be smaller here.

**Where POSIX (and others) legitimately win:** this engine does more work *up
front*, building and minimising a DFA, and the benchmark times only the match,
not compilation. For a one-shot match against a short string — compile, match
once, throw away — POSIX's cheaper setup and a mature backtracking engine's
literal-prefix and other optimisations can easily come out ahead. The DFA
approach pays off when a compiled pattern is matched against a lot of input.

## Building and using it

```bash
make            # build the static library and the regexdump tool
make test       # build and run the test suite
make memcheck   # run the suite under valgrind, or ASan/UBSan + leaks as a fallback
make bench      # build and run the benchmarks
make tools      # build build/regexdump
make clean
```

Everything compiles with `-std=c99 -Wall -Wextra -Werror` and is clean.

The API is four functions (`include/regex.h`):

```c
#include "regex.h"

const char *err = NULL;
Regex *re = regex_compile("(a|b)*abb", &err);
if (!re) {
    fprintf(stderr, "bad pattern: %s\n", err);  /* includes a character offset */
    return 1;
}

regex_match(re, "aababb");        /* 1 — full match: the whole text is in the language */

size_t start, end;
if (regex_search(re, "xxabbyy", &start, &end)) {
    /* leftmost-longest: start/end is the half-open byte range of the match */
}

regex_free(re);
```

- `regex_compile` returns `NULL` and sets `*err` to a static string (with the
  offset) on a malformed pattern. Library code never calls `abort`/`exit`.
- `regex_match` is a full match — the entire text must be in the language.
- `regex_search` finds the leftmost start position and the longest match there.

Inspect any pattern's internals with the CLI:

```bash
./build/regexdump '(a|b)*abb'
```

## Supported syntax

| Feature | Syntax |
|---|---|
| concatenation | `abc` |
| alternation | `a|b` |
| Kleene star | `a*` |
| one or more | `a+`  (desugars to `aa*`) |
| optional | `a?`  (desugars to `(a|ε)`) |
| grouping | `(ab)` |
| character class | `[abc]`, ranges `[a-z]`, negation `[^0-9]`, mixes `[a-fA-F0-9]` |
| wildcard | `.`  (any byte except newline) |
| escapes | `\n \t \r \\ \. \* \+ \? \( \) \[ \] \| \-` |
| shorthand classes | `\d \w \s` and negations `\D \W \S` |
| anchors | `^` (start of text), `$` (end of text) |

Inside a class, `]` as the first character is a literal, and `\d`/`\w`/`\s` etc.
work as members.

Anchors are single-line: `^` asserts position 0, `$` asserts end of text (there
is no multiline mode). They are modelled as two zero-width boundary symbols fed
to the DFA at the start and end of the text, so they stay within the same
table-driven, backtracking-free matcher.

## What is deliberately not supported, and why

**Backreferences** (e.g. `(a+)\1`) and **lookaround** (e.g. `(?=…)`, `(?<!…)`).

This is not laziness — it is the whole point of the project. A language with
backreferences is provably **not regular** (matching `(a+)\1` requires
remembering and re-comparing an arbitrary earlier substring, which no finite
automaton can do). Lookaround likewise pushes beyond what a single forward DFA
pass expresses. Supporting either feature forces an engine back onto
**backtracking** — and backtracking is exactly the source of the catastrophic
blowup this engine is built to avoid. Leaving them out is what *buys* the linear
guarantee.

If you need backreferences or lookaround, you fundamentally need a different
(and slower-in-the-worst-case) kind of engine.

## Project layout

```
include/regex.h     public API (4 functions)
src/bitset.{c,h}    heap-allocated bitset of positions (no 64-position cap)
src/parser.c        pattern text -> syntax tree (recursive descent)
src/ast.c           node construction, cloning, freeing, debug dump
src/positions.c     nullable / firstpos / lastpos / followpos
src/dfa.c           subset construction
src/minimize.c      Hopcroft minimisation
src/match.c         compile pipeline glue + the matching loops
src/internal.h      shared internal declarations
tests/test_regex.c  assertion-based test runner (no framework)
bench/bench.c       benchmark harness vs POSIX + naive backtracking foil
tools/regexdump.c   CLI: dumps the tree, follow tables, and DFA for a pattern
Makefile
```

A note on **bitsets**: the classic lab version of this algorithm packs a
position set into a single `unsigned long long`, which caps a pattern at 64
positions. Here position sets are a heap-allocated array of 64-bit words
(`src/bitset.c`), so pattern size is bounded only by memory.

## Limitations and honesty

- **Search is O(n·L), not O(n).** `regex_search` finds a leftmost-longest match
  by trying each start position, where `L` is the match length at that start.
  There is no *exponential* blowup (each attempt is a linear DFA walk), but it is
  not the truly-linear single-pass search a production engine (grep, RE2) does.
  `regex_match` (full match) is genuinely O(n).
- **Compile does eager work.** Subset construction plus Hopcroft runs at
  `regex_compile` time. For most patterns the DFA is tiny, but subset
  construction is worst-case exponential in the number of positions for
  adversarial patterns; the *matching* guarantee is unaffected.
- **The alphabet is bytes.** Input is treated as bytes, not Unicode code points.
  UTF-8 text matches fine byte-for-byte, but `.` and classes operate on bytes.
- **`*err` is a static buffer.** The error string from a failed `regex_compile`
  points into a static buffer, so it is overwritten by the next failing compile
  and the call is not thread-safe in that narrow respect. Compiled `Regex`
  objects are independent and fine to use concurrently.
- **valgrind on Apple Silicon.** valgrind does not support arm64 macOS, so
  `make memcheck` falls back to AddressSanitizer + UBSan (memory errors) plus
  the macOS `leaks` tool (leak detection). The suite reports zero leaks and zero
  errors under both that fallback and valgrind on Linux.
