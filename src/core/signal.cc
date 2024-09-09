/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <stdexcept>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/signal.hh>
#include <seastar/core/reactor.hh>
#endif

namespace seastar {

void handle_signal(int signo, noncopyable_function<void ()>&& handler, bool once) {
    auto& r = engine();
    if (once) {
        r._signals.handle_signal_once(signo, std::move(handler));
    } else {
        r._signals.handle_signal(signo, std::move(handler));
    }
}

}