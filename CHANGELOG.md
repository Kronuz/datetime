# Changelog

All notable changes to `datetime`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

Upgrading a pin from before 2026-10-30: [the consumer upgrade note](https://github.com/Kronuz/cpp-libraries/blob/main/docs/UPGRADING-2026-10.md#datetime) lists what changes for callers, how to find affected inputs, the commits not to pin, and the speedup.

## 2026-11-01

### Fixed

- `TimedeltaParser("")` read `timedelta[0]` of an empty view. It still throws `TimedeltaError`,
  now without the out-of-bounds read (an assertion failure under libstdc++'s hardening,
  which `test/diff_test.cc` found once it compared the time parsers). The frozen reference
  has the same read, so the test runs only the library on an empty timedelta (class 9).

### Changed

- `TimeParser` and `TimedeltaParser` bound a fraction to 1 to 62 digits themselves, as
  `DatetimeParser`, `Iso8601Parser` and the loose scanner already did. They had passed the
  fraction's width to `strict_stod`, so the limit was strict-stox's: strict-stox up to
  `21b7941` read 63 bytes and rejected a longer field, and strict-stox from `c1c9bc1` (its
  exact decimal path) reads any width, so a 63-digit fraction would have parsed after a pin
  bump. The widths of the grammar are now constants of `datetime.cc` (`MAX_FRACTION_DIGITS`
  = 62, `MAX_TERM_CHARS` = 63, the number of a date-math term), checked before a field
  reaches strict-stox; no result changes against a release built with `21b7941`.
- strict-stox moves from `21b7941` to `752cf9f` (its main). On the 2-byte and 3-byte
  fields `datetime` hands it (every 1- and 2-byte string, 4,913 3-byte strings) and on
  1 to 62 digit fractions (24,925 strings) the two versions return identical values and
  errno; they differ only above 62 digits (63 to 2000 digits: rejected before, accepted now).
- `test/diff_test.cc` covers `TimeParser`, `TimedeltaParser`, `isTime` and `isTimedelta`
  against the frozen reference (random, mutated, every byte at every position of 13
  templates, and the widths around the limit); they were checked only by `test/test.cc`.
- Class 8 of `test/diff_test.cc`: a field wider than the grammar (a fraction over 62 digits
  in a datetime, time or timedelta; a date-math number over 63 characters). The frozen
  reference passes the field to strict-stox, so its verdict is strict-stox's width:
  with `21b7941` it equals the library's, with `c1c9bc1` or later it accepts
  `+0...010d` (64 characters, the value 10) and a 63-digit fraction. That is 19,985 mismatches in 13.9M checks
  on the first attempt of the pin bump (the pin was
  `c1c9bc1`: 19,974 date-math terms, 11 loose-format fractions). The test now asserts the
  library against the oracle there (an exact error, from the widths above, the reference on the
  terms before a wide one, and the reference's message text) and `reference == library` everywhere
  else; it passes with both pins (14.1M checks, 0 mismatches; 22,400 wide terms and 322
  wide fractions hit). A mutant that raises or lowers either width by one, and one that removes
  the time/timedelta bound, each fail a named check; with `datetime.cc` reverted to
  `672fde6` the run fails on `TimeParser wide fraction`.

### Added

- `bench/compare.cc` (`datetime_compare`, built with `-DDATETIME_BUILD_BENCH=ON` beside
  `datetime_bench`) times the parsers, `timestamp`, `to_tm_t` and `iso8601` against
  independent implementations: a hand-written ISO 8601 parser and formatter, days-from-civil
  and civil-from-days, libc `strptime` + `timegm`, and abseil's `ParseTime`, `FromCivil` and
  `FormatTime` when abseil is installed. Before this, the benchmark compared only the frozen
  reference. The numbers, on an Apple M4 Pro and an Intel i9-9980HK, are in the README:
  `datetime` takes 1.2 to 3.2x the hand parser's time and about a sixth of `ParseTime`'s.
- `datetime_compare --check` is the CTest test `datetime_compare`: every comparator must
  return the library's results on 4096 random inputs per row. A mutant that moves the
  day of the year, and one that moves January and February to the previous year in
  `civil_from_days`, make it fail.
- CI builds the benchmarks and runs the check.

## 2026-10-31

### Fixed

- Month arithmetic. `normalizeMonths` mapped month 12k (k >= 2) to month 0
  (`mon %= 12`), so `+12M` from December threw `Month must be in 1..12`.
  `2020-12-15||+12M` was an error and is now `2021-12-15`. `normalizeMonths` and
  the date-math step both map any month to 1..12 and carry whole years with
  floor division (`03f36fa`). The oracle is a month arithmetic model written in
  `test/ref.h` on top of days-from-civil. `test/datemath_test.cc` checks every
  start month, 15 years (leap and century edges), the days 1, 15, 28, 29, 30
  and 31, and N in -240..240,
  with the end-of-month clamp, and checks `normalizeMonths` over -500..500.
  A base `tm_t` whose month is a multiple of 12 above 12 normalizes to month 12
  (before, to month 0); only those inputs and the `+12M` class change result, and
  `test/diff_test.cc` asserts old != new there and new == oracle.
- A loose-format offset (`2020/01/02 03:04:05+05:30`) sets `is_utc`, as the ISO
  path does for an applied offset. The oracle is the previous result with
  `is_utc` forced true (`03f36fa`).
- `Iso8601Parser(str, tm)` and `Iso8601Parser(str)` (and so `isDatetime` and
  `DatetimeParser`) return the same `Format` for every input. They were two
  hand-copied scanners that had diverged: a negative hour, minute or second
  (`T-3:04:05`) was accepted by the filling overload and is now `OUT_OF_RANGE` in
  both, and a fraction dot with no digits (`...:05.`, `...:05.Z`) or with more
  than 62 digits was accepted by the validating overload and is now `ERROR` in
  both (`fe618d8`). One `iso8601_scan<Fill>` template serves both.
- A loose-format string ending in `Z` (`2020/01/02 03:04:05Z`) parses as UTC
  (`is_utc` set), like the ISO form. It always threw
  `Invalid format in Datetime Math unit: 'h'. '' must be numeric` before
  (`fe618d8`).
- Date-math terms such as `+2147483647d` overflowed `int` (undefined behavior).
  They are computed in 64-bit arithmetic; the supported years are 1 to `INT_MAX`
  and a result beyond them throws `Year is out of range`. `toordinal` no longer
  overflows `int` for huge years (`fe618d8`). The oracle is days-from-civil in
  128-bit plus an exhaustive 1..9999 brute-force day counter.

- A timestamp before 1970 with a fraction. `timestamp(tm_t)` subtracted the
  fraction from a negative whole-second count (1969-12-31T23:59:58.9 was -2.9, so
  23:59:58.9, 23:59:59.0 and 23:59:59.1 gave -2.9, -1.0 and -1.1, out of order),
  and `to_tm_t(double)` truncated a negative timestamp toward zero and dropped its
  fraction (`-2.9` was 23:59:58.0, `-0.5` was 1970-01-01T00:00:00). The fraction is
  the distance above the whole second, before 1970 as after: 23:59:58.9 is -1.1,
  `to_tm_t(-2.9)` is 23:59:57.1, and `iso8601(double)` follows. Timestamps from
  1970 on and integral timestamps are unchanged. The oracle is days-from-civil
  (`test/ref.h`, 128-bit) for the whole seconds plus the fraction; fractions of an
  eighth are compared for equality at every year from 1 to 9999, and
  `test/datemath_test.cc` checks the order of the instants of the last second of
  1969 and 200,000 random timestamps. `test/diff_test.cc` lists the class
  (`timestamp` before 1970 with a fraction, `to_tm_t(double)` and `iso8601(double)`
  of a negative timestamp with a fraction), asserts old != new and new == oracle
  there, and enforces old == new elsewhere.
- `normalizeMonths(int&, int&)` looped once per twelve months for a month below 1
  (about 180 million iterations for `INT_MIN`), and `year += (mon - 1) / 12`
  overflowed `int` for a year near `INT_MAX`. It uses floor division in 64-bit
  arithmetic, and a year that leaves `int` throws `Year is out of range` and leaves
  both arguments unchanged. The string parsers never reach it with such months.
  The oracle is a 128-bit model over every combination of extreme months and years;
  the test also bounds the time of 100 extreme calls at half a second.
- The 1-to-62-digit limit of a fraction no longer depends on `strict_stod`'s buffer.
  `parse_fsec` rejects 0 and 63 or more digits itself; it relied on `strict_stod`
  failing for inputs longer than 63 bytes, so a `strict-stox` that accepts them
  would have made `Iso8601Parser(str, tm)` accept a fraction that
  `Iso8601Parser(str)` rejects.

### Changed

- `toordinal`, `timegm` and `timestamp(tm_t)` compute from `days_from_civil` after
  the same validation, instead of a second calendar path (a cumulative-days table
  and a hand-written ordinal). The exceptions, their messages and their order are
  unchanged: year below 1, day below 1, month outside 1..12, day beyond the month
  (`test/datemath_test.cc` pins each, and `test/diff_test.cc` compares the three
  functions, including the month normalized in place, with the original on random
  valid and invalid fields). The unsigned day count used for years >= 1 needs no sign
  handling in its divisions, and a month in 1..12 skips `normalizeMonths`.
  `bench/bench.cc` has rows for the four functions (the frozen original is the
  column on the left, with the calendar path these functions had before; three
  interleaved rounds on an otherwise idle machine): on the M4 Pro `toordinal` 3.1 -> 1.8 ns,
  `timegm(tm_t)` 4.7 -> 3.0 ns, `timestamp(tm_t)` 4.4 -> 2.9 ns; on the i9 5.7 ->
  3.4 ns, 9.0 -> 5.1 ns and 11.2 -> 6.9 ns. The other rows of `bench/bench.cc` are the
  same before and after within 3% on both machines (date math, `...||+1d/d`, 54 ns on
  the M4 Pro and 98 ns on the i9, calls `timegm`), except two on paths this change does
  not touch: `parse ...+hh:mm` on the M4 Pro (28.4 -> 30.1 ns) and
  `parse 2020/01/02 03:04:05` on the i9 (39.8 -> 44.7 ns, 17.4 -> 17.3 on the M4 Pro).
  The frozen original in the same binary does not move, and a variant that kept
  `parse_fsec` as it was and put the digit limit at its two callers moved the same two
  rows by a similar amount (and `parse .ffffffZ` by 7% on the i9), so the cost follows
  the layout of the scanner functions, not their code.
- The seven-digit-and-longer fraction path of `parse_fsec` is a separate function that
  is not inlined, so the microsecond path stays as small as it was where it is inlined
  into the scanner (`parse .ffffffZ` was 7% slower on the i9 before the split).
- `test/diff_test.cc` draws every random value in its own statement. Operands of
  `+` and call arguments are unsequenced, so `f(rnd(), rnd())` and
  `(chance(50) ? "+" : "-") + digits(...)` took their draws in an order the
  compiler chose, and gcc and clang tested different inputs (one Linux job failed
  on `03f36fa` and the others passed). The test prints a digest of every input it
  generates and compares it with the recorded one for the default run. The digest of
  the default run was 300aac9e19bc9afe with clang and ee72110919bac91f with gcc on
  x86-64 before the change, and is f96573b230c52919 with both after it (`c654278`).
- `.gitignore` covers build directories (`build*/`, `cmake-build-*/`, `dbg/`, `ubsan/`,
  `asan/`, `tsan/`, `*.dSYM/`, `.scratch/`). `dbg/`, committed by `7acfefd`, is
  untracked since `ed7426b`; it stays in history.
- The validating `Iso8601Parser` overload checks the fraction by digit count
  (1 to 62) instead of parsing its value, which cost 19% on `isDatetime` for a
  `.ffffffZ` input (`2359df3`).
- `test/diff_test.cc` lists each deliberate difference from the frozen original as
  an input class (negative field, fraction, loose `Z`, month, loose offset),
  asserts old != new and new == oracle there, and enforces old == new everywhere
  else. A base month that is a multiple of 12 above 12 that throws in both
  versions is compared with the old code on the base with the month already
  normalized (`03f36fa`).

### Kept

- Year 1 with an offset that moves the instant before 0001-01-01 throws
  `Year is out of range`: the instant is outside the supported years.

## 2026-10-30

### Changed

- The regex and `strto*` parsing paths are replaced by hand-written scanners
  (`5d4f2cb`). Results are identical to the previous implementation on every
  input, accepted or rejected; `test/old/` holds a frozen copy and
  `datetime_diff_test` compares both on random, mutated and every-byte inputs.
  - Fixed-width ISO fields are read as plain digits. Odd bytes take an exact
    emulation of the `strtoul` grammar (leading whitespace and sign), so `"+5"`
    and `" 5"` still parse. Fractions up to 6 digits are integer microseconds.
    ISO parse 41-230 ns -> 8-26 ns (5-9x).
  - The loose formats (`2020/01/02 3:04`) and the `|| date math` suffix use hand
    scanners of the `date_re` / `date_math_re` grammars instead of `std::regex`.
    Loose parse 1320 ns -> 17 ns (~80x); date math 760 ns -> 50 ns (~15x). The
    exported `date_re` and `date_math_re` objects stay for ABI compatibility and
    are unused.
  - A UTC offset is one integer sum instead of two date-math strings and two
    calendar round trips (~9x). A generic fallback runs the offsets the integer
    path does not cover.
  - `iso8601` writes digits into a stack buffer instead of a chain of
    `std::format` calls (~9x). Fractions round the exact binary value in 128-bit
    integers, ties to even, like `{:.6f}`.
  - `to_tm_t` and date math use an integer civil-from-days conversion instead of
    `gmtime_r`: 35 ns -> 7 ns (~5x).
  - Benchmark: `-DDATETIME_BUILD_BENCH=ON` builds `datetime_bench`, old against
    new.

### Added

- `test/diff_test.cc`, a differential test against the frozen original in
  `test/old/` (namespace `DatetimeOld`).
- `test/datemath_test.cc`, date math and calendar checks against independent
  references.
- `bench/bench.cc`, the benchmark above.

## 2026-10-28

### Changed

- The 2026 Kronuz copyright sits above Dubalu's lines; Dubalu's years end at 2020
  (`fce3d4d`).

## Initial extraction

The library is the pure string and number half of Xapiand's
`src/datetime.{h,cc}`. The differences from that file:

- `DatetimeError` derived from Xapiand's `ClientError` (a located exception). It
  derives from `std::runtime_error`, and `DateISOError`, `TimeError` and
  `TimedeltaError` derive from `DatetimeError`. Names and message-string
  constructors are unchanged.
- `THROW(X, "fmt", a)` became `throw X(std::format("fmt", a))`, and
  `strings::format` became `std::format`.
- `repr(value)` in two date-math error messages became plain `'{}'` quoting. One
  of those messages had a `{}` with no matching argument; the extracted version
  passes the unit and the token.
- Logging was dropped (`L_CALL` was only in the MsgPack overload; `L_ERR` was
  included and never called).
- The MsgPack path (`process_date_year` ... `process_date_datetime`, and the
  `DatetimeParser(const MsgPack&)`, `time_to_double(const MsgPack&)` and
  `timedelta_to_double(const MsgPack&)` overloads) moved to Xapiand-side glue that
  delegates back to this library's string and double parsers.
