// SPDX-License-Identifier: Apache-2.0

#include "btop.hpp"

#include <iterator>
#include <string_view>
#include <vector>

auto main(int argc, const char* argv[]) -> int {
	std::vector<std::string_view> args(std::next(argv), std::next(argv, argc));
	return btop_main(args);
}
