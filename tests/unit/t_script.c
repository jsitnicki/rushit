#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <setjmp.h>
#include <cmocka.h>
#include <unistd.h>

#include "lib.h"
#include "logging.h"
#include "script.h"

static int common_setup(void **state)
{
        struct callbacks *cb;

        cb = calloc(1, sizeof(*cb));
        assert_non_null(cb);
        logging_init(cb);
        *state = cb;

        return 0;
}

static int common_teardown(void **state)
{
        struct callbacks *cb = *state;

        logging_exit(cb);
        free(*state);

        return 0;
}

static void t_create_script_engine(void **state)
{
        struct script_engine *se = NULL;
        struct callbacks *cb = *state;
        bool is_client = false;
        int r;

        r = script_engine_create(&se, cb, is_client);
        assert_return_code(r, -r);
        assert_non_null(se);

        se = script_engine_destroy(se);
}

static void t_create_script_slave(void **state)
{
        struct script_engine *se = NULL;
        struct script_slave *ss = NULL;
        struct callbacks *cb = *state;
        bool is_client = false;
        int r;

        r = script_engine_create(&se, cb, is_client);
        assert_return_code(r, -r);
        assert_non_null(se);

        r = script_slave_create(&ss, se);
        assert_return_code(r, -r);
        assert_non_null(ss);

        ss = script_slave_destroy(ss);
        se = script_engine_destroy(se);
}

static int client_engine_setup(void **state)
{
        struct script_engine *se = NULL;
        struct callbacks *cb = *state;
        bool is_client = true;
        int r;

        r = script_engine_create(&se, cb, is_client);
        assert_return_code(r, -r);
        assert_non_null(se);

        *state = se;

        return 0;
}

static int client_engine_teardown(void **state)
{
        struct script_engine *se = *state;

        se = script_engine_destroy(se);
        assert_null(se);

        return 0;
}

static int client_slave_setup(void **state)
{
        struct script_engine *se = NULL;
        struct script_slave *ss = NULL;
        int r;

        r = client_engine_setup(state);
        assert_return_code(r, 0);
        se = *state;

        r = script_slave_create(&ss, se);
        assert_return_code(r, -r);
        assert_non_null(ss);

        *state = ss;

        return 0;
}

static int client_slave_teardown(void **state)
{
        struct script_slave *ss = *state;
        struct script_engine *se = ss->se;
        int r;

        ss = script_slave_destroy(ss);
        assert_null(ss);

        *state = se;
        r = client_engine_teardown(state);
        assert_return_code(r, 0);

        return 0;
}

static void t_hooks_run_without_errors(void **state)
{
        struct script_engine *se = *state;
        const char *test_script[] = {
                "client_socket(function () end)",
                "client_close(function () end)",
                "client_sendmsg(function () end)",
                "client_recvmsg(function () end)",
                "client_recverr(function () end)",
                "server_socket(function () end)",
                "server_close(function () end)",
                "server_sendmsg(function () end)",
                "server_recvmsg(function () end)",
                "server_recverr(function () end)",
                "is_client()",
                "is_server()",
                "tid_iter()",
                NULL,
        };
        const char **ts;
        int r;

        for (ts = test_script; *ts; ts++) {
                r = script_engine_run_string(se, *ts, NULL, NULL);
                assert_return_code(r, -r);
        }
}

static void wait_func(void *done_)
{
        bool *done = done_;
        *done = true;
}

static void t_wait_func_gets_called(void **state)
{
        struct script_engine *se = *state;
        bool wait_done = false;
        int r;

        r = script_engine_run_string(se, "", wait_func, &wait_done);
        assert_return_code(r, -r);
        assert_true(wait_done);
}

static void t_run_socket_hook_from_string(void **state)
{
        const char *script = "client_socket( function () return 42 end )";
        struct script_slave *ss = *state;
        int r;

        r = script_engine_run_string(ss->se, script, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_socket_hook(ss, -1, NULL);
        assert_int_equal(r, 42);
}

static void t_run_socket_hook_from_file(void **state)
{
        const char *script = "client_socket( function () return 42 end )";
        char script_path[] = "/tmp/t_run_socket_hook_from_file.XXXXXX";
        struct script_slave *ss = *state;
        int fd;
        ssize_t r;

        fd = mkstemp(script_path);
        assert_return_code(fd, errno);

        r = write(fd, script, strlen(script));
        assert_return_code(r, errno);

        r = close(fd);
        assert_return_code(r, errno);

        r = script_engine_run_file(ss->se, script_path, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_socket_hook(ss, -1, NULL);
        assert_int_equal(r, 42);

        r = unlink(script_path);
        assert_return_code(r, errno);
}

static void t_run_close_hook(void **state)
{
        const char *script = "client_close( function () return 42 end )";
        struct script_slave *ss = *state;
        int r;

        r = script_engine_run_string(ss->se, script, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_close_hook(ss, -1, NULL);
        assert_int_equal(r, 42);
}

static void t_run_sendmsg_hook(void **state)
{
        const char *script = "client_sendmsg( function () return 11015 end )";
        struct script_slave *ss = *state;
        int r;

        r = script_engine_run_string(ss->se, script, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_sendmsg_hook(ss, -1, NULL, 0);
        assert_int_equal(r, 11015);
}

static void t_run_recvmsg_hook(void **state)
{
        const char *script = "client_recvmsg( function () return 28139 end )";
        struct script_slave *ss = *state;
        int r;

        r = script_engine_run_string(ss->se, script, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_recvmsg_hook(ss, -1, NULL, 0);
        assert_int_equal(r, 28139);
}

static void t_run_recverr_hook(void **state)
{
        const char *script = "client_recverr( function () return 7193 end )";
        struct script_slave *ss = *state;
        int r;

        r = script_engine_run_string(ss->se, script, NULL, NULL);
        assert_return_code(r, -r);

        r = script_slave_recverr_hook(ss, -1, NULL, 0);
        assert_int_equal(r, 7193);
}

#define clinet_engine_unit_test(f) \
        cmocka_unit_test_setup_teardown((f), client_engine_setup, client_engine_teardown)
#define client_slave_unit_test(f) \
        cmocka_unit_test_setup_teardown((f), client_slave_setup, client_slave_teardown)

int main(void)
{
        const struct CMUnitTest tests[] = {
                cmocka_unit_test(t_create_script_engine),
                cmocka_unit_test(t_create_script_slave),
                clinet_engine_unit_test(t_hooks_run_without_errors),
                clinet_engine_unit_test(t_wait_func_gets_called),
                client_slave_unit_test(t_run_socket_hook_from_string),
                client_slave_unit_test(t_run_socket_hook_from_file),
                client_slave_unit_test(t_run_close_hook),
                client_slave_unit_test(t_run_sendmsg_hook),
                client_slave_unit_test(t_run_recvmsg_hook),
                client_slave_unit_test(t_run_recverr_hook),
        };

        return cmocka_run_group_tests(tests, common_setup, common_teardown);
}