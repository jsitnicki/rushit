/*
 * Copyright 2016 Google Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "common.h"
#include "flags.h"
#include "lib.h"

static void check_options(struct options *opts, struct callbacks *cb)
{
        CHECK(cb, opts->test_length >= 1,
              "Test length must be at least 1 second.");
        CHECK(cb, opts->maxevents >= 1,
              "Number of epoll events must be positive.");
        CHECK(cb, opts->num_flows >= 1,
              "There must be at least 1 flow.");
        CHECK(cb, opts->num_threads >= 1,
              "There must be at least 1 thread.");
        if (opts->client) {
                CHECK(cb, opts->num_flows >= opts->num_threads,
                      "There should not be less flows than threads.");
        }
        CHECK(cb, opts->request_size > 0,
              "Request size must be positive.");
        CHECK(cb, opts->response_size > 0,
              "Response size must be positive.");
        CHECK(cb, opts->interval > 0,
              "Interval must be positive.");
        CHECK(cb, opts->min_rto >= 0,
              "TCP_MIN_RTO must be positive.");
        CHECK(cb, opts->min_rto < (1U << 31) / 1000000,
              "TCP_MIN_RTO * 1,000,000 must be less than 2^31 (nanoseconds).");
        CHECK(cb, opts->max_pacing_rate >= 0,
              "Max pacing rate must be non-negative.");
        CHECK(cb, opts->max_pacing_rate <= UINT32_MAX,
              "Max pacing rate cannot exceed 32 bits.");
        CHECK(cb, opts->buffer_size > 0,
              "Buffer size must be positive.");
        CHECK(cb, opts->client || (opts->local_host == NULL),
              "local_host may only be set for clients.");
        CHECK(cb, opts->listen_backlog <= procfile_int(PROCFILE_SOMAXCONN, cb),
              "listen() backlog cannot exceed " PROCFILE_SOMAXCONN);
}

int main(int argc, char **argv)
{
        struct options opts = {0};
        struct callbacks cb = {0};
        struct flags_parser *fp;
        int exit_code = 0;

        logging_init(&cb);

        fp = flags_parser_create(&opts, &cb);
        DEFINE_FLAG(fp, int,          magic,         42,       0,  "Magic number used by control connections");
        DEFINE_FLAG(fp, int,          min_rto,       0,        0,  "TCP_MIN_RTO (ms)");
        DEFINE_FLAG(fp, int,          maxevents,     1000,     0,  "Number of epoll events per epoll_wait() call");
        DEFINE_FLAG(fp, int,          num_flows,     1,       'F', "Total number of flows");
        DEFINE_FLAG(fp, int,          num_threads,   1,       'T', "Number of threads");
        DEFINE_FLAG(fp, int,          num_clients,   1,        0,  "Number of clients");
        DEFINE_FLAG(fp, int,          test_length,   10,      'l', "Test length in seconds");
        DEFINE_FLAG(fp, int,          request_size,  1,       'Q', "Number of bytes in a request from client to server");
        DEFINE_FLAG(fp, int,          response_size, 1,       'R', "Number of bytes in a response from server to client");
        DEFINE_FLAG(fp, int,          buffer_size,   65536,   'B', "Number of bytes that each read()/send() can transfer at once");
        DEFINE_FLAG(fp, int,          listen_backlog, 128,     0,  "Backlog size for listen()");
        DEFINE_FLAG(fp, int,          suicide_length, 0,      's', "Suicide length in seconds");
        DEFINE_FLAG(fp, bool,         ipv4,          false,   '4', "Set desired address family to AF_INET");
        DEFINE_FLAG(fp, bool,         ipv6,          false,   '6', "Set desired address family to AF_INET6");
        DEFINE_FLAG(fp, bool,         client,        false,   'c', "Is client?");
        DEFINE_FLAG(fp, bool,         debug,         false,   'd', "Set SO_DEBUG socket option");
        DEFINE_FLAG(fp, bool,         dry_run,       false,   'n', "Turn on dry-run mode");
        DEFINE_FLAG(fp, bool,         pin_cpu,       false,   'U', "Pin threads to CPU cores");
        DEFINE_FLAG(fp, bool,         logtostderr,   false,   'V', "Log to stderr");
        DEFINE_FLAG(fp, bool,         nonblocking,   false,    0,  "Make sure syscalls are all nonblocking");
        DEFINE_FLAG(fp, double,       interval,      1.0,     'I', "For how many seconds that a sample is generated");
        DEFINE_FLAG(fp, long long,    max_pacing_rate, 0,     'm', "SO_MAX_PACING_RATE value; use as 32-bit unsigned");
        DEFINE_FLAG_PARSER(fp, max_pacing_rate, parse_max_pacing_rate);
        DEFINE_FLAG(fp, const char *, local_host,    NULL,    'L', "Local hostname or IP address");
        DEFINE_FLAG(fp, const char *, host,          NULL,    'H', "Server hostname or IP address");
        DEFINE_FLAG(fp, const char *, control_port,  "12866", 'C', "Server control port");
        DEFINE_FLAG(fp, const char *, port,          "12867", 'P', "Server data port");
        DEFINE_FLAG(fp, const char *, all_samples,   NULL,    'A', "Print all samples? If yes, this is the output file name");
        DEFINE_FLAG_HAS_OPTIONAL_ARGUMENT(fp, all_samples);
        DEFINE_FLAG_PARSER(fp, all_samples, parse_all_samples);
        DEFINE_FLAG(fp, struct percentiles, percentiles, { .chosen = { false } }, 'p',  "Latency percentiles");
        DEFINE_FLAG_PARSER(fp, percentiles, parse_percentiles);
        DEFINE_FLAG_PRINTER(fp, percentiles, print_percentiles);
        flags_parser_run(fp, argc, argv);
        if (opts.logtostderr)
                cb.logtostderr(cb.logger);
        flags_parser_dump(fp);
        flags_parser_destroy(fp);

        opts.enable_write = true;
        opts.enable_read = true;

        /* XXX: Fixed mode. Always multiplex server port. */
        opts.reuseport = true;

        check_options(&opts, &cb);
        if (opts.suicide_length) {
                if (create_suicide_timeout(opts.suicide_length)) {
                        PLOG_FATAL(&cb, "create_suicide_timeout");
                        goto exit;
                }
        }
        exit_code = tcp_rr(&opts, &cb);
exit:
        logging_exit(&cb);
        return exit_code;
}
