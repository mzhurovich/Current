/*******************************************************************************
 The MIT License (MIT)

 Copyright (c) 2017 Grigory Nikolaenko <nikolaenko.grigory@gmail.com>

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 *******************************************************************************/

#include "../../../Bricks/dflags/dflags.h"

#include "../../../Sherlock/sherlock.h"

#include "entry.h"

DEFINE_uint32(entry_length, 1000, "The length of the string member values in the generated entries.");
DEFINE_uint32(entries_count, 100000, "The number of entries in the output data.");
DEFINE_string(output_file, "data.json", "The path to persist the stream to.");

int main(int argc, char** argv) {
  ParseDFlags(&argc, &argv);
  std::cout << "Generate stream consisted of " << FLAGS_entries_count << " entries." << std::endl;

  const char symbols[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  const uint32_t symbols_count = sizeof(symbols) / sizeof(symbols[0]) - 1;
  std::vector<char> pattern(FLAGS_entry_length + 1);

  using stream_t = current::sherlock::Stream<Entry, current::persistence::File>;
  stream_t stream(FLAGS_output_file);

  for (uint32_t i = 0; i < FLAGS_entries_count; ++i) {
    for (uint32_t j = 0; j < FLAGS_entry_length; ++j) {
      pattern[j] = symbols[(i / symbols_count + (i + 1) * j) % symbols_count];
    }
    stream.Publish(Entry(&pattern[0]));
  }

  std::cout << "Successfully generated and saved to " << FLAGS_output_file << std::endl;
  return 0;
}
