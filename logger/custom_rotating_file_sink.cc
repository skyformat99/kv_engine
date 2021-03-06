//
// Copyright(c) 2015 Gabi Melman.
// Distributed under the MIT License (http://opensource.org/licenses/MIT)
//

/* -*- MODE: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "custom_rotating_file_sink.h"

#include <platform/dirutils.h>
#include <platform/make_unique.h>

static unsigned long find_first_logfile_id(const std::string& basename) {
    unsigned long id = 0;

    auto files = cb::io::findFilesWithPrefix(basename);
    for (auto& file : files) {
        // the format of the name should be:
        // fnm.number.txt
        auto index = file.rfind(".txt");
        if (index == std::string::npos) {
            continue;
        }

        file.resize(index);
        index = file.rfind('.');
        if (index != std::string::npos) {
            try {
                unsigned long value = std::stoul(file.substr(index + 1));
                if (value > id) {
                    id = value + 1;
                }
            } catch (...) {
                // Ignore
            }
        }
    }

    return id;
}

template <class Mutex>
custom_rotating_file_sink<Mutex>::custom_rotating_file_sink(
        const spdlog::filename_t& base_filename,
        std::size_t max_size,
        const std::string& log_pattern)
    : _base_filename(base_filename),
      _max_size(max_size),
      _current_size(0),
      _file_helper(std::make_unique<spdlog::details::file_helper>()),
      _next_file_id(find_first_logfile_id(base_filename)) {
    formatter = std::make_shared<spdlog::pattern_formatter>(
            log_pattern, spdlog::pattern_time_type::local);
    _file_helper->open(calc_filename());
    _current_size = _file_helper->size(); // expensive. called only once
    addHook(openingLogfile);
}

/* In addition to the functionality of spdlog's rotating_file_sink,
 * this class adds hooks marking the start and end of a logfile.
 */
template <class Mutex>
void custom_rotating_file_sink<Mutex>::_sink_it(
        const spdlog::details::log_msg& msg) {
    _current_size += msg.formatted.size();
    if (_current_size > _max_size) {
        std::unique_ptr<spdlog::details::file_helper> next =
                std::make_unique<spdlog::details::file_helper>();
        try {
            next->open(calc_filename(), true);
            addHook(closingLogfile);
            std::swap(_file_helper, next);
            _current_size = msg.formatted.size();
            addHook(openingLogfile);
        } catch (...) {
            // Keep on logging to the this file, but try swap at the next
            // insert of data (didn't use the next file we need to
            // roll back the next_file_id to avoid getting a hole ;-)
            _next_file_id--;
        }
    }
    _file_helper->write(msg);
}

template <class Mutex>
void custom_rotating_file_sink<Mutex>::_flush() {
    _file_helper->flush();
}

/* Takes a message, formats it and writes it to file */
template <class Mutex>
void custom_rotating_file_sink<Mutex>::addHook(const std::string& hook) {
    spdlog::details::log_msg msg;
    msg.time = spdlog::details::os::now();
    msg.level = spdlog::level::info;
    msg.raw << hook;

    if (hook == openingLogfile) {
        msg.raw << fmt::StringRef(_file_helper->filename().data(),
                                  _file_helper->filename().size());
    }
    formatter->format(msg);
    _current_size += msg.formatted.size();

    _file_helper->write(msg);
}

template <class Mutex>
spdlog::filename_t custom_rotating_file_sink<Mutex>::calc_filename() {
    std::conditional<std::is_same<spdlog::filename_t::value_type, char>::value,
                     fmt::MemoryWriter,
                     fmt::WMemoryWriter>::type w;

    char fname[1024];
    unsigned long try_id = _next_file_id;
    do {
        sprintf(fname, "%s.%06lu.txt", _base_filename.c_str(), try_id++);
    } while (access(fname, F_OK) == 0);

    _next_file_id = try_id;

    w.write(SPDLOG_FILENAME_T("{}"), fname);
    return w.str();
}

template <class Mutex>
custom_rotating_file_sink<Mutex>::~custom_rotating_file_sink() {
    addHook(closingLogfile);
}

template class custom_rotating_file_sink<std::mutex>;
template class custom_rotating_file_sink<spdlog::details::null_mutex>;
