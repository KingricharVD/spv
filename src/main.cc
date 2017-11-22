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
#include "./util.h"

using namespace spv;

static const char usage_str[] = "Usage: spv [-h|--help] [-v|--version]\n";

int main(int argc, char** argv) {
  static const char short_opts[] = "hv";
  static struct option long_opts[] = {{"help", no_argument, 0, 'h'},
                                      {"version", no_argument, 0, 'v'},
                                      {0, 0, 0, 0}};

  for (;;) {
    int c = getopt_long(argc, argv, short_opts, long_opts, nullptr);
    if (c == -1) {
      break;
    }
    switch (c) {
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

  Client client;
  client.send_version(70001, nullptr);
  return 0;
}
