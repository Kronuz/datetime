# AGENTS.md

Working notes for agents modifying this repository. For the design read
`ARCHITECTURE.md`; for usage read `README.md`. This file covers the repo layout,
how to build and test, the invariants you must not break, and the traps.

## Repo map

```
datetime.h                   Datetime namespace API: tm_t / clk_t / Format, the parsers/formatters, DatetimeError hierarchy. Header.
datetime.cc                  The pure-core implementation: ISO 8601 + date-math + time + timedelta parsing/formatting. Compiled.
test/diff_test.cc            Differential test against the frozen reference in test/old/ (CTest `datetime_diff`).
test/old/                    Frozen reference datetime.h/.cc, namespace DatetimeOld. Do not edit.
test/datemath_test.cc        Years 1..9999, huge date-math terms, months, loose offsets, pre-1970 fractions, normalizeMonths and toordinal/timegm validation against independent references (CTest `datetime_datemath`).
test/ref.h                   The references: days-from-civil, the month arithmetic model.
bench/bench.cc               Benchmark against test/old/ (-DDATETIME_BUILD_BENCH=ON).
bench/compare.cc             Benchmark against independent comparators: a hand-written ISO parser and formatter, days-from-civil, libc strptime + timegm, and abseil when found (datetime_compare, same option). `--check` is CTest `datetime_compare`: every comparator must agree.
test/test.cc                 Runnable smoke test: ISO parses, date math, timestamp round-trip, time/timedelta, throwing paths, predicates.
examples/demo.cc             A runnable tour of every input form (not a test).
CHANGELOG.md                 Dated changes, behavior changes and benchmark history.
CMakeLists.txt               STATIC library `datetime` (+ alias datetime::datetime); FetchContents strict-stox; CTest tests `datetime`, `datetime_diff`, `datetime_datemath` and, with the bench option, `datetime_compare`.
LICENSE                      MIT, Copyright (c) 2015-2019 Dubalu LLC.
README.md                    What it is, install, quick start.
docs/API.md                  Usage, scope, and everything it parses and formats.
ARCHITECTURE.md              Parse paths, numeric core, how the library differs from Xapiand.
```

This is a compiled library (one `.cc`), not header-only. The CMake target is a
`STATIC` library that links `strict_stox::strict_stox` PUBLIC and requests
`cxx_std_20`.

## Build and run the test

```sh
cmake -B build && cmake --build build && ctest --test-dir build
```

Expected output ends with `all datetime tests passed`, exit 0. The test target is
`datetime_test`; the registered CTest name is `datetime`.

## Verification and benchmarks

The method shared by the Kronuz libraries is in
[docs/PERFORMANCE.md](https://github.com/Kronuz/cpp-libraries/blob/main/docs/PERFORMANCE.md).

- **Tests.** `ctest` runs `datetime`, `datetime_diff` and `datetime_datemath`, and
  `datetime_compare` when the benchmarks are built.
- **Differential.** `datetime_diff` (`datetime_diff_test 20000`) compares every parser and
  formatter with the frozen reference in `test/old/` (namespace `DatetimeOld`, never edited) on
  random, mutated and exhaustive input. The classes where results differ are listed at the top of
  `test/diff_test.cc`, each with its oracle; the test prints a digest of its inputs and checks it
  against the recorded one, so every compiler tests the same inputs.
- **Oracles.** `datetime_datemath` checks years 1 to 9999 and huge date-math terms against
  Howard Hinnant's days-from-civil and a month arithmetic model (`test/ref.h`) and the extremes
  against 128-bit arithmetic. `datetime_compare --check` requires a hand-written ISO 8601 parser
  and formatter, the civil-calendar algorithms, libc `strptime` + `timegm` and, when installed,
  abseil to return the library's results.
- **Sanitizers.** CI (`.github/workflows/ci.yml`, which calls the shared
  [cmake.yml](https://github.com/Kronuz/cpp-libraries/blob/main/.github/workflows/cmake.yml))
  builds with `-DDATETIME_BUILD_BENCH=ON` and runs every test under ASan+UBSan and under TSan,
  clang on Linux. The ASan+UBSan job (UBSan is the check on the 64-bit date math):

  ```sh
  CC=clang CXX=clang++ cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DDATETIME_BUILD_BENCH=ON \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -D_GLIBCXX_ASSERTIONS" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
  cmake --build build-asan && ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
  ```

- **Benchmarks.** `-DDATETIME_BUILD_BENCH=ON` builds `datetime_bench` (`bench/bench.cc`: each row
  times the frozen reference, `old`, and the library, `new`, and stops on any disagreement) and
  `datetime_compare` (`bench/compare.cc`, the comparators above; abseil through
  `find_package(absl)`, its rows skipped when absent). The README's Performance section has the
  numbers. To compare two commits, build `datetime_bench` at each and run both with
  `tools/ab_compare.py` from a clone of [cpp-libraries](https://github.com/Kronuz/cpp-libraries)
  beside this one; it runs A, B and A again (the A/A control) every round, and `--column 2` reads
  the library's time:

  ```sh
  git worktree add ../datetime-base BASE_COMMIT
  cmake -S ../datetime-base -B build-base -DCMAKE_BUILD_TYPE=Release -DDATETIME_BUILD_BENCH=ON && cmake --build build-base --target datetime_bench
  cmake -B build-bench -DCMAKE_BUILD_TYPE=Release -DDATETIME_BUILD_BENCH=ON && cmake --build build-bench --target datetime_bench
  python3 ../cpp-libraries/tools/ab_compare.py run --rounds 7 --format unit --column 2 \
    build-base/datetime_bench build-bench/datetime_bench
  ```

## Conventions

- **C++20.** The target requests `cxx_std_20` to stay uniform with the sibling
  libraries and because the code uses `std::format`. Don't drop it.
- **One external dependency: strict-stox.** The only third-party include is
  `strict_stox.hh`. Do not add `hashes` / `phf` / MsgPack back; that machinery
  belongs to the Xapiand glue, not here (see "Standalone vs. Xapiand").
- **Filenames are stable.** `datetime.h` / `datetime.cc` keep their original
  Xapiand names so a consumer that already `#include "datetime.h"` just needs
  this repo on the include path. Don't rename them.
- Tabs for indentation, double quotes in code, no em dashes in prose.

## Load-bearing invariants

- **The exception hierarchy is `std::runtime_error`-based.** `DatetimeError :
  std::runtime_error`, and `DateISOError` / `TimeError` / `TimedeltaError` :
  `DatetimeError`. Names and the message-string constructors are the contract:
  Xapiand catches these exact types. Don't reintroduce a `ClientError` /
  located-exception base, and don't change which parser throws which subclass
  (datetime/date-math -> `DatetimeError`, time -> `TimeError`, timedelta ->
  `TimedeltaError`).
- **`Iso8601Parser` returns a `Format`, it does not throw.** The `INVALID` case
  is load-bearing: `process_date_datetime` falls through to the loose-format
  scanner (`scan_date_re`) on `INVALID`. If you make it throw, that fallback dies.
- **Date-math rounding direction.** `/unit` rounds *up* (end of unit, fsec =
  `DATETIME_MAX_FSEC`); `//unit` rounds *down* (start of unit, fsec 0). The test
  pins both; keep them straight.
- **Results equal the frozen reference except in the listed classes.** `test/old/`
  is the frozen reference implementation (namespace `DatetimeOld`) and
  `datetime_diff_test` compares every parser and formatter against it. The input
  classes that differ (negative fields, fractions, loose `Z`, months, loose
  offsets, pre-1970 timestamp fractions, fields wider than the grammar) are listed at the top of `test/diff_test.cc`, each with its oracle.
  Date fields are read by `field()`, which emulates the `strtoul` grammar
  (`"+5"` and `" 5"` are fields). Keep the differential test green; a result
  that changes outside those classes needs an owner decision.
- **`strict_*` nothrow variant** serves fractions over 6 digits and the
  time/timedelta scanners; branch on `errno_save` rather than catching.
- **Year > 0 domain.** `timegm` / `timestamp` / `toordinal` are proleptic
  Gregorian from year 1; `isValidDate` rejects year < 1. Don't feed year 0.

## How to extend

- **Add a format/unit.** Extend the relevant length case in `Iso8601Parser` (both
  overloads) or a `computeDateMath` unit. Keep the nothrow `strict_*` + branch
  pattern and renormalize via `timegm` + `set_from_secs` after a math op.
- **Always extend the smoke test.** `test/test.cc` is the only executable check.
  Cover the new happy path and at least one malformed input that must throw.

## Traps

- **`/d` rounds to end-of-day, not start.** Easy to get backwards when writing a
  test expectation (this exact mistake is why the test documents both `/d` and
  `//d`).
- **`computeTimeZone` is date math under the hood.** A `+05:30` offset is applied
  as `-5h -30m`, so an offset shifts the stored `tm_t` to the UTC instant (and
  can roll the date). That is intended; don't "fix" it to keep wall-clock fields.
- **Two `Iso8601Parser` overloads must stay in sync.** One fills a `tm_t`, one
  only validates; both are `iso8601_scan<Fill>`, which runs every check in both
  and only skips the writes. Keep it that way: `test/diff_test.cc` asserts both
  verdicts equal a reference verdict (`ref_iso`) for every input.
- **Date math is 64-bit (`add_term`).** `+N`/`-N` never touch `int` arithmetic;
  supported years are 1..`INT_MAX`. `test/datemath_test.cc` checks it against a
  days-from-civil reference and is meant to be run under UBSan too.
- **Loose `Z` and loose offsets are UTC** (`is_utc = true`), like the ISO path.
- **`normalizeMonths` yields 1..12** for any month, in constant time, and throws
  `Year is out of range` when the carried year leaves `int`;
  `test/datemath_test.cc` checks every start month and N in -240..240 against the
  month model in `test/ref.h`, and the extremes against 128-bit arithmetic.
- **The calendar has one engine.** `toordinal`, `timegm`, `timestamp` and `to_tm_t`
  go through `days_from_civil` / `civil_from_days`; do not add a second table-based
  day count. The fraction of a timestamp is added to the whole seconds (floor), before
  1970 too.
- **Field widths are datetime's own.** A fraction has 1 to 62 digits (`MAX_FRACTION_DIGITS`) and
  the number of a date-math term at most 63 characters (`MAX_TERM_CHARS`), in every
  scanner, checked before a field reaches strict-stox. The frozen reference passes the
  field to strict-stox, so its answer on a wider one follows strict-stox's width; the test
  asserts only the library there (class 8). Bound a new numeric field the same way.
- **Random test inputs must not depend on the compiler.** Operands of `+` and call
  arguments are unsequenced: draw into named locals, one statement per draw.
  `test/diff_test.cc` prints a digest of the inputs it generates and compares it
  with the recorded one for the default run; change the constant only when the
  generator changes on purpose.

## Standalone vs. Xapiand

This is the **pure string/number half** of
[Xapiand](https://github.com/Kronuz/Xapiand)'s `src/datetime.{h,cc}`. It differs
from that file by decoupling: the exceptions derive from `std::runtime_error`;
`THROW(...)` is `throw X(std::format(...))`; `strings::format` is `std::format`;
`repr(...)` quoting is inline; there is no logging.

The **MsgPack structured-object half** (the `process_date_*` static helpers and
the `DatetimeParser(const MsgPack&)` / `time_to_double(const MsgPack&)` /
`timedelta_to_double(const MsgPack&)` overloads) is not part of this library. It
is the only user of MsgPack, the `RESERVED_*` tokens, and `phf::make_phf` / `hh(...)`,
and it lives in Xapiand as glue (`datetime_msgpack.{h,cc}`) that pulls the value
out of the MsgPack and delegates to this library's string/double parsers. Keep
that boundary: anything MsgPack- or reserved-token-shaped does not belong here.
