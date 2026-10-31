# Changelog

All notable changes to `datetime`. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); the library has no
releases, so entries are dated.

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

### Changed

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
