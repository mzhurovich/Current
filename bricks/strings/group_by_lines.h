/*******************************************************************************
The MIT License (MIT)

Copyright (c) 2020 Dmitry "Dima" Korolev <dmitry.korolev@gmail.com>

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

#ifndef BRICKS_STRINGS_GROUP_BY_LINES_H
#define BRICKS_STRINGS_GROUP_BY_LINES_H

#include <functional>
#include <string>

namespace current {
namespace strings {

template <typename F = std::function<void(const char*)>>
class GenericStatefulGroupByLines final {
 private:
  F f_;
  std::string residual_;

 public:
  explicit GenericStatefulGroupByLines(F&& f) : f_(std::move(f)) {}
  void Feed(const std::string& s) {
    Feed(s.c_str());
  }
  void Feed(const char* s) {
    // NOTE: `Feed`() will only call the callback upon seeing the newline in the input data block. If the last line
    // does not end with a newline, it will not be forwarded until the `StatefulGroupByLines` instance is destroyed.
    while (true) {
      while (*s && *s != '\n') {
        residual_ += *s++;
      }
      if (*s == '\n') {
        f_(residual_.c_str());
        residual_.clear();
        ++s;
      } else {
        return;
      }
    }
  }
  ~GenericStatefulGroupByLines() {
    if (!residual_.empty()) {
      // Upon destruction, process the the last incomplete line, if necessary.
      f_(residual_.c_str());
    }
  }
};

using StatefulGroupByLines = GenericStatefulGroupByLines<std::function<void(const std::string&)>>;

}  // namespace strings
}  // namespace current

#endif  // BRICKS_STRINGS_GROUP_BY_LINES_H