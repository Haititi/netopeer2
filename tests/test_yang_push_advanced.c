/**
 * @file test_yang_push.c
 * @author Tadeas Vintrlik <xvintr04@stud.fit.vutbr.cz>
 * @brief tests yang push notifications
 *
 * @copyright
 * Copyright (c) 2019 - 2021 Deutsche Telekom AG.
 * Copyright (c) 2017 - 2021 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE

#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <cmocka.h>
#include <libyang/libyang.h>
#include <nc_client.h>
#include <sysrepo.h>

#include "np_test.h"
#include "np_test_config.h"

static int
local_setup(void **state)
{
    struct np_test *st;
    char test_name[256];
    const char *modules[] = {NP_TEST_MODULE_DIR "/edit1.yang", NP_TEST_MODULE_DIR "/edit2.yang"};
    int rc;

    /* get test name */
    np_glob_setup_test_name(test_name);

    /* setup environment */
    rc = np_glob_setup_env(test_name);
    assert_int_equal(rc, 0);

    /* setup netopeer2 server */
    rc = np_glob_setup_np2(state, test_name, modules, sizeof modules / sizeof *modules);
    assert_int_equal(rc, 0);
    st = *state;

    /* enable replay support */
    assert_int_equal(SR_ERR_OK, sr_set_module_replay_support(st->conn, "edit1", 1));

    return 0;
}

static int
teardown_common(void **state)
{
    struct np_test *st = *state;
    const char *data;
    char *cmd;
    int ret;

    /* Remove the notifications */
    if (asprintf(&cmd, "rm -rf %s/%s/data/notif/notif1.notif*", NP_SR_REPOS_DIR, st->test_name) == -1) {
        return 1;
    }

    ret = system(cmd);
    free(cmd);

    if (ret == -1) {
        return 1;
    } else if (!WIFEXITED(ret) || WEXITSTATUS(ret)) {
        return 1;
    }

    /* reestablish NETCONF connection */
    nc_session_free(st->nc_sess, NULL);
    st->nc_sess = nc_connect_unix(st->socket_path, NULL);
    assert_non_null(st->nc_sess);

    /* Remove the data */
    data = "<first xmlns=\"ed1\" xmlns:xc=\"urn:ietf:params:xml:ns:netconf:base:1.0\" xc:operation=\"remove\"/>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    data = "<top xmlns=\"ed2\" xmlns:xc=\"urn:ietf:params:xml:ns:netconf:base:1.0\" xc:operation=\"remove\"/>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    return 0;
}

static int
local_teardown(void **state)
{
    struct np_test *st = *state;
    const char *modules[] = {"edit1", "edit2"};

    if (!st) {
        return 0;
    }

    /* disable replay support */
    assert_int_equal(SR_ERR_OK, sr_set_module_replay_support(st->conn, "edit1", 0));

    /* close netopeer2 server */
    return np_glob_teardown(state, modules, sizeof modules / sizeof *modules);
}

static void
test_on_change_stop_time(void **state)
{
    struct np_test *st = *state;
    const char *template, *data;
    char *ntf, *stop_time;
    struct timespec ts;

    /* Get stop_time */
    assert_int_not_equal(-1, clock_gettime(CLOCK_REALTIME, &ts));
    assert_int_equal(ly_time_ts2str(&ts, &stop_time), LY_SUCCESS);

    /* Establish onchange push with stop_time */
    st->rpc = nc_rpc_establishpush_onchange("ietf-datastores:running", NULL, stop_time, NULL, 0, 0,
            NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    free(stop_time);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Check for subscription-terminated notification */
    RECV_NOTIF(st);
    template =
            "<subscription-terminated xmlns=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">\n"
            "  <id>%d</id>\n"
            "  <reason xmlns:sn=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">sn:no-such-subscription</reason>\n"
            "</subscription-terminated>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* Insert some data */
    data = "<first xmlns=\"ed1\">TestFirst</first>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    /* No notification should arrive since the subscription has been terminated */
    ASSERT_NO_NOTIF(st);
    FREE_TEST_VARS(st);
}

static void
test_on_change_modify_fail(void **state)
{
    struct np_test *st = *state;
    char *stop_time;
    struct timespec ts;

    /* Establish on-change push */
    st->rpc = nc_rpc_establishpush_onchange("ietf-datastores:running", NULL, NULL, NULL, 0, 0,
            NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Get stop_time */
    assert_int_not_equal(-1, clock_gettime(CLOCK_REALTIME, &ts));
    assert_int_equal(ly_time_ts2str(&ts, &stop_time), LY_SUCCESS);

    /* Modify the stop_time, should fail since wrong rpc is used */
    SEND_RPC_MODSUB(st, st->ntf_id, "<first xmlns=\"ed1\"/>", stop_time);
    ASSERT_RPC_ERROR(st);
    FREE_TEST_VARS(st);
    free(stop_time);
}

static void
test_on_change_modify_stoptime(void **state)
{
    struct np_test *st = *state;
    const char *template, *data;
    char *ntf, *stop_time;
    struct timespec ts;

    /* Establish on-change push */
    st->rpc = nc_rpc_establishpush_onchange("ietf-datastores:running", NULL, NULL, NULL, 0, 0,
            NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Get stop_time */
    assert_int_not_equal(-1, clock_gettime(CLOCK_REALTIME, &ts));
    assert_int_equal(ly_time_ts2str(&ts, &stop_time), LY_SUCCESS);

    /* Modify the stop_time */
    st->rpc = nc_rpc_modifypush_onchange(st->ntf_id, "ietf-datastores:running", NULL, stop_time, 0,
            NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    free(stop_time);
    RECV_SUBMOD_NOTIF(st);

    /* Check for subscription-terminated notification */
    RECV_NOTIF(st);
    template =
            "<subscription-terminated xmlns=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">\n"
            "  <id>%d</id>\n"
            "  <reason xmlns:sn=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">sn:no-such-subscription</reason>\n"
            "</subscription-terminated>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* Insert some data */
    data = "<first xmlns=\"ed1\">TestFirst</first>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    /* No notification should arrive since the subscription has been terminated */
    ASSERT_NO_NOTIF(st);
    FREE_TEST_VARS(st);
}

static void
test_on_change_modify_filter(void **state)
{
    struct np_test *st = *state;
    const char *template, *data;
    char *ntf;

    /* Establish on-change push */
    st->rpc = nc_rpc_establishpush_onchange("ietf-datastores:running", NULL, NULL, NULL, 0, 0,
            NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Modify the filter */
    st->rpc = nc_rpc_modifypush_onchange(st->ntf_id, "ietf-datastores:running", "<first xmlns=\"ed1\"/>", NULL, 0,
            NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    RECV_SUBMOD_NOTIF(st);

    /* Insert some data */
    data = "<first xmlns=\"ed1\">TestFirst</first>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    /* Receive a notification with the new data */
    RECV_NOTIF(st);
    template =
            "<push-change-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-changes>\n"
            "    <yang-patch>\n"
            "      <patch-id>patch-1</patch-id>\n"
            "      <edit>\n"
            "        <edit-id>edit-1</edit-id>\n"
            "        <operation>create</operation>\n"
            "        <target>/edit1:first</target>\n"
            "        <value>\n"
            "          <first xmlns=\"ed1\">TestFirst</first>\n"
            "        </value>\n"
            "      </edit>\n"
            "    </yang-patch>\n"
            "  </datastore-changes>\n"
            "</push-change-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* Insert some data */
    data =
            "<top xmlns=\"ed2\">\n"
            "  <name>Test</name>\n"
            "</top>\n";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    /* This notification should not pass the new filter */
    ASSERT_NO_NOTIF(st);
}

static int
setup_test_periodic_modify_filter(void **state)
{
    struct np_test *st = *state;
    const char *data;

    data =
            "<top xmlns=\"ed2\">\n"
            "  <name>Test</name>\n"
            "</top>\n";
    SR_EDIT(st, data);
    return 0;
}

static void
test_periodic_modify_filter(void **state)
{
    struct np_test *st = *state;
    const char *template;
    char *ntf;

    /* Establish periodic push */
    st->rpc = nc_rpc_establishpush_periodic("ietf-datastores:running", NULL, NULL, NULL, 25, NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Receive a notification */
    RECV_NOTIF(st);
    template =
            "<push-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-contents>\n"
            "    <top xmlns=\"ed2\">\n"
            "      <name>Test</name>\n"
            "    </top>\n"
            "  </datastore-contents>\n"
            "</push-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* Modify the filter */
    st->rpc = nc_rpc_modifypush_periodic(st->ntf_id, "ietf-datastores:running", "<first xmlns=\"ed1\"/>", NULL, 25,
            NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    RECV_SUBMOD_NOTIF(st);

    /* Receive a notification */
    RECV_NOTIF(st);
    template =
            "<push-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-contents/>\n"
            "</push-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);
}

static void
test_periodic_modify_period(void **state)
{
    struct np_test *st = *state;
    const char *template;
    char *ntf;

    /* Establish periodic push */
    st->rpc = nc_rpc_establishpush_periodic("ietf-datastores:running", NULL, NULL, NULL, 50, NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Receive a notification */
    RECV_NOTIF(st);
    template =
            "<push-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-contents/>\n"
            "</push-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* Modify the period */
    st->rpc = nc_rpc_modifypush_periodic(st->ntf_id, "ietf-datastores:running", NULL, NULL, 1000, NULL,
            NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    RECV_SUBMOD_NOTIF(st);

    /* Receive a notification */
    RECV_NOTIF(st);
    template =
            "<push-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-contents/>\n"
            "</push-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    /* No notification should arrive in timeout since the period is too long */
    ASSERT_NO_NOTIF(st);
}

static void
test_periodic_deletesub(void **state)
{
    struct np_test *st = *state;
    const char *template;
    char *ntf;

    /* Establish periodic push */
    st->rpc = nc_rpc_establishpush_periodic("ietf-datastores:running", NULL, NULL, NULL, 50, NULL, NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    /* Receive a notification */
    RECV_NOTIF(st);
    template =
            "<push-update xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-push\">\n"
            "  <id>%d</id>\n"
            "  <datastore-contents/>\n"
            "</push-update>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    FREE_TEST_VARS(st);

    st->rpc = nc_rpc_deletesub(st->ntf_id);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);

    /* Check for subscription-terminated notification */
    RECV_NOTIF(st);
    template =
            "<subscription-terminated xmlns=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">\n"
            "  <id>%d</id>\n"
            "  <reason xmlns:sn=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">sn:no-such-subscription</reason>\n"
            "</subscription-terminated>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    lyd_free_tree(st->op);
    lyd_free_tree(st->envp);
    ASSERT_OK_REPLY(st);
    FREE_TEST_VARS(st);

    /* No other notification should arrive */
    ASSERT_NO_NOTIF(st);
}

static void
test_onchange_deletesub(void **state)
{
    struct np_test *st = *state;
    const char *data, *template;
    char *ntf;

    /* Establish onchange push with stop_time */
    st->rpc = nc_rpc_establishpush_onchange("ietf-datastores:running", NULL, NULL, NULL, 0, 0, NULL,
            NC_PARAMTYPE_CONST);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);
    ASSERT_OK_SUB_NTF(st);
    FREE_TEST_VARS(st);

    st->rpc = nc_rpc_deletesub(st->ntf_id);
    st->msgtype = nc_send_rpc(st->nc_sess, st->rpc, 1000, &st->msgid);
    assert_int_equal(st->msgtype, NC_MSG_RPC);

    /* Check for subscription-terminated notification */
    RECV_NOTIF(st);
    template =
            "<subscription-terminated xmlns=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">\n"
            "  <id>%d</id>\n"
            "  <reason xmlns:sn=\"urn:ietf:params:xml:ns:yang:ietf-subscribed-notifications\">sn:no-such-subscription</reason>\n"
            "</subscription-terminated>\n";
    assert_int_not_equal(-1, asprintf(&ntf, template, st->ntf_id));
    assert_string_equal(st->str, ntf);
    free(ntf);
    lyd_free_tree(st->op);
    lyd_free_tree(st->envp);
    ASSERT_OK_REPLY(st);
    FREE_TEST_VARS(st);

    /* Insert some data */
    data = "<first xmlns=\"ed1\">TestFirst</first>";
    SR_EDIT(st, data);
    FREE_TEST_VARS(st);

    /* No notification should arrive since the subscription has been terminated */
    ASSERT_NO_NOTIF(st);
}

int
main(int argc, char **argv)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_teardown(test_on_change_stop_time, teardown_common),
        cmocka_unit_test_teardown(test_on_change_modify_fail, teardown_common),
        cmocka_unit_test_teardown(test_on_change_modify_stoptime, teardown_common),
        cmocka_unit_test_teardown(test_on_change_modify_filter, teardown_common),
        cmocka_unit_test_setup_teardown(test_periodic_modify_filter, setup_test_periodic_modify_filter,
                teardown_common),
        cmocka_unit_test_teardown(test_periodic_modify_period, teardown_common),
        cmocka_unit_test_teardown(test_periodic_deletesub, teardown_common),
        cmocka_unit_test_teardown(test_onchange_deletesub, teardown_common),
    };

    nc_verbosity(NC_VERB_WARNING);
    sr_log_stderr(SR_LL_WRN);
    parse_arg(argc, argv);
    return cmocka_run_group_tests(tests, local_setup, local_teardown);
}
