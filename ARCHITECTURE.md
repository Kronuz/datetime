# ARCHITECTURE

The internal design of the `datetime` library.

## What this library is

A single compiled translation unit (`datetime.cc`) behind one header
(`datetime.h`), all in the `Datetime` namespace. It turns date/time *strings and
numbers* into structured values and back. Three input vocabularies:

- **Datetime**: ISO 8601, optionally suffixed by `|| <date math>`. Returns
  `tm_t` (year, mon, day, hour, min, sec, fractional sec, `is_utc`).
- **Time**: `HH:MM(:SS(.ffff)([+-]HH:MM))`. Returns `clk_t`.
- **Timedelta**: `[+-]HH:MM(:SS(.ffff))`. Returns `clk_t`.

plus the conversions between those and `time_t` / `double` (Unix seconds), and
the `iso8601(...)` / `*_to_string(...)` formatters.

## Parse paths

`DatetimeParser(string_view)` delegates to a file-local `process_date_datetime`
that does the work in two stages:

1. **Fast ISO 8601 path** (`Iso8601Parser`). A hand-written, allocation-free
   scanner keyed on the string length (10 = date, 19 = date+time, 20 = `...Z`,
   else offset/fractional). It returns a `Format` enum (`VALID` / `INVALID` /
   `OUT_OF_RANGE` / `ERROR`) rather than throwing, so the caller can fall through
   to the slow path on `INVALID`. There are two overloads: one fills a `tm_t`,
   one only validates (used by `isDatetime`).
2. **Regex fallback** (`date_re`). A single big `std::regex` that also captures
   the `|| <date math>` suffix. Only reached when the fast path says `INVALID`,
   which mostly means a non-`T`/non-`-` separator style.

If the string contains `||`, the part before it is parsed as a datetime and the
part after is fed to `processDateMath`.

### Date math

`processDateMath` walks `date_math_re` (`([+-]\d+|/{1,2})([dyMwhms])`) with a
`match_continuous` iterator and calls `computeDateMath` per term. `+N`/`-N` add or
subtract; `/unit` rounds *up* to the end of the unit (e.g. `/M` -> last day,
23:59:59.999999); `//unit` rounds *down* to the start. After each operation the
`tm_t` is renormalized by round-tripping through `timegm` + `gmtime_r`, so
overflow (e.g. day 32) carries correctly. `computeTimeZone` is implemented on top
of `computeDateMath`: an offset of `+05:30` is applied as `-5h -30m` of date math
to convert the wall-clock value to the UTC instant.

`Time` and `Timedelta` use their own length-keyed scanners
(`TimeParser` / `TimedeltaParser`), not the regex.

### Numeric core

`toordinal` computes a proleptic Gregorian day number; `timegm` /
`timestamp(tm_t)` build a Unix timestamp from it (valid only for year > 0, which
is the library's domain); `to_tm_t` goes the other way via `gmtime_r`. Leap-year
and days-in-month logic lives in `isleapYear` / `getDays_month` with two static
`days` / `cumdays` tables.

## Decoupling delta from Xapiand

The library is the *pure half* of Xapiand's `src/datetime.{h,cc}`. The changes
made during extraction, all behavior-preserving except where noted:

- **Exceptions reparented.** `DatetimeError` derived from Xapiand's `ClientError`
  (which is a located-exception `Exception`). Here it derives from
  `std::runtime_error`, and `DateISOError` / `TimeError` / `TimedeltaError`
  derive from `DatetimeError`. The names and the message-string constructors are
  unchanged, so existing `catch (const DatetimeError&)` (and subclass) sites
  still work. The forward `class MsgPack;` and `#include "exception.h"` are gone.
- **`THROW(X, "fmt", a)` -> `throw X(std::format("fmt", a))`.** The
  located-exception `THROW` macro needs a `BaseException`-shaped constructor
  (`__func__, __FILE__, __LINE__, ...`); a `std::runtime_error` subclass does not
  have it, so the throws are spelled out directly.
- **`strings::format` -> `std::format`** (`<format>`), dropping `strings.hh`.
- **`repr(value)` inlined.** Two date-math error messages used Xapiand's `repr`
  to quote the offending token; that is now plain `'{}'` quoting in the
  `std::format` string. One of those messages had a latent bug in the original (a
  `{}` with no matching argument); the extracted version passes the unit and the
  token so it formats correctly.
- **Logging dropped.** The lone `L_CALL` was in the MsgPack overload (now glue);
  `L_ERR` was included but never called. No logging remains, so `log.h` is gone.
- **MsgPack path lifted out.** The `process_date_year/_month/.../_datetime`
  static helpers and the `DatetimeParser(const MsgPack&)` /
  `time_to_double(const MsgPack&)` / `timedelta_to_double(const MsgPack&)`
  overloads were the only users of MsgPack, the `RESERVED_*` tokens, and
  `phf::make_phf` / `hh(...)`. They moved to Xapiand-side glue that delegates back
  to this library's string/double parsers.

## Dependencies

- **strict-stox** (header-only, FetchContent). Every number pulled out of a
  date/time string goes through `strict_stoi` / `strict_stoul` / `strict_stod`,
  whose nothrow `errno_save` variant is what lets the scanners report a bad field
  without throwing mid-parse. This is the library's only third-party dependency.
- **No hashes / phf.** The perfect-hash table was MsgPack-key dispatch in the
  glue half. The pure core does not hash anything, so `hashes` is *not* a
  dependency here (unlike some sibling extractions).
- Standard library: `<regex>`, `<format>`, `<chrono>`, `<ctime>`.

## Trade-offs

- The fast ISO 8601 path duplicates a lot of structure across length cases and
  across the two overloads; it is verbose but allocation-free and is the common
  path. The regex is the catch-all for the less common separator styles and for
  capturing the date-math suffix.
- `timestamp` / `timegm` are only correct for year > 0 (proleptic Gregorian from
  year 1). That is the documented domain; `isValidDate` rejects year < 1.
- Inputs are parsed against the host's `gmtime_r`; the library assumes a 64-bit
  `time_t` for the full datetime range.
