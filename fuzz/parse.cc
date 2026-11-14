#include "datetime.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {
template<class Call> void parse(Call&& call) {
	try { call(); } catch (const DatetimeError&) {}
}
}
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
	const std::string input(reinterpret_cast<const char*>(data), size);
	const std::string_view text(input);
	Datetime::tm_t iso;
	(void)Datetime::Iso8601Parser(text, iso);
	(void)Datetime::Iso8601Parser(text);
	parse([&] { (void)Datetime::DatetimeParser(text); });
	parse([&] { (void)Datetime::TimeParser(text); });
	parse([&] { (void)Datetime::TimedeltaParser(text); });
	parse([&] {
		Datetime::tm_t base(2024, 2, 29, 12, 30, 45);
		Datetime::processDateMath(text, base);
	});
	parse([&] { (void)Datetime::isDate(text); });
	parse([&] { (void)Datetime::isDatetime(text); });
	parse([&] { (void)Datetime::isTime(text); });
	parse([&] { (void)Datetime::isTimedelta(text); });
	return 0;
}
