// Copyright (c) 2017 Evan Klitzke <evan@eklitzke.org>
//
// This file is part of SPV.
//
// SPV is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version.
//
// SPV is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
// A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// SPV. If not, see <http://www.gnu.org/licenses/>.

#include <getopt.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "./client.h"
#include "./config.h"
#include "./encoder.h"
#include "./logging.h"
#include "./util.h"
#include "./uvw.h"

static const char usage_str[] = "Usage: spv [-h|--help] [-v|--version]\n";

int main(int argc, char** argv) {
  DEFINE_LOGGER
  static const char short_opts[] = "dhv";
  static struct option long_opts[] = {{"debug", no_argument, 0, 'd'},
                                      {"help", no_argument, 0, 'h'},
                                      {"version", no_argument, 0, 'v'},
                                      {0, 0, 0, 0}};

  for (;;) {
    int c = getopt_long(argc, argv, short_opts, long_opts, nullptr);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'd':
        spdlog::set_level(spdlog::level::debug);
        break;
      case 'h':
        std::cout << SPV_VERSION_STR << "\n\n" << usage_str;
        return 0;
        break;
      case 'v':
        std::cout << SPV_VERSION_STR << "\n";
        return 0;
        break;
      case '?':
        // getopt_long should already have printed an error message
        break;
      default:
        std::cerr << "unrecognized command line flag: " << optarg << "\n";
        abort();
    }
  }

  log->info("main started, creating client");
  spv::Client client;
  client.send_version_to_seeds();
  uvw::Loop::getDefault()->run();
  return 0;
}
