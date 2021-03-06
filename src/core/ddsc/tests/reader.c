/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include <assert.h>

#include "ddsc/dds.h"
#include "os/os.h"
#include "Space.h"
#include "RoundTrip.h"
#include <criterion/criterion.h>
#include <criterion/logging.h>
#include <criterion/theories.h>


#if 0
#define PRINT_SAMPLE(info, sample) cr_log_info("%s (%d, %d, %d)\n", info, sample.long_1, sample.long_2, sample.long_3);
#else
#define PRINT_SAMPLE(info, sample)
#endif



/**************************************************************************************************
 *
 * Test fixtures
 *
 *************************************************************************************************/
#define MAX_SAMPLES                 7
/*
 * By writing, disposing, unregistering, reading and re-writing, the following
 * data will be available in the reader history.
 * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
 * ----------------------------------------------------------
 * |    0   |    0   |    0   |     read | old | alive      |
 * |    1   |    0   |    0   |     read | old | disposed   |
 * |    2   |    1   |    0   |     read | old | no_writers |
 * |    3   |    1   |    1   | not_read | old | alive      |
 * |    4   |    2   |    1   | not_read | new | disposed   |
 * |    5   |    2   |    1   | not_read | new | no_writers |
 * |    6   |    3   |    2   | not_read | new | alive      |
 */
#define SAMPLE_LAST_READ_SST      (2)
#define SAMPLE_LAST_OLD_VST       (3)
#define SAMPLE_IST(idx)           (((idx % 3) == 0) ? DDS_IST_ALIVE              : \
                                   ((idx % 3) == 1) ? DDS_IST_NOT_ALIVE_DISPOSED : \
                                                      DDS_IST_NOT_ALIVE_NO_WRITERS )
#define SAMPLE_VST(idx)           ((idx <= SAMPLE_LAST_OLD_VST ) ? DDS_VST_OLD  : DDS_VST_NEW)
#define SAMPLE_SST(idx)           ((idx <= SAMPLE_LAST_READ_SST) ? DDS_SST_READ : DDS_SST_NOT_READ)

static dds_entity_t g_participant = 0;
static dds_entity_t g_subscriber  = 0;
static dds_entity_t g_topic       = 0;
static dds_entity_t g_reader      = 0;
static dds_entity_t g_writer      = 0;
static dds_entity_t g_waitset     = 0;
static dds_qos_t    *g_qos        = NULL;
static dds_qos_t    *g_qos_null   = NULL;
static dds_listener_t *g_listener = NULL;
static dds_listener_t *g_list_null= NULL;

static void*              g_loans[MAX_SAMPLES];
static void*              g_samples[MAX_SAMPLES];
static Space_Type1        g_data[MAX_SAMPLES];
static dds_sample_info_t  g_info[MAX_SAMPLES];

static char*
create_topic_name(const char *prefix, char *name, size_t size)
{
    /* Get semi random g_topic name. */
    os_procId pid = os_procIdSelf();
    uintmax_t tid = os_threadIdToInteger(os_threadIdSelf());
    (void) snprintf(name, size, "%s_pid%"PRIprocId"_tid%"PRIuMAX"", prefix, pid, tid);
    return name;
}

static void
reader_init(void)
{
    Space_Type1 sample = { 0 };
    dds_attach_t triggered;
    dds_return_t ret;
    char name[100];

    g_qos = dds_qos_create();
    g_listener = dds_listener_create(NULL);

    g_participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    cr_assert_gt(g_participant, 0, "Failed to create prerequisite g_participant");

    g_subscriber = dds_create_subscriber(g_participant, NULL, NULL);
    cr_assert_gt(g_subscriber, 0, "Failed to create prerequisite g_subscriber");

    g_waitset = dds_create_waitset(g_participant);
    cr_assert_gt(g_waitset, 0, "Failed to create g_waitset");

    g_topic = dds_create_topic(g_participant, &Space_Type1_desc, create_topic_name("ddsc_reader_test", name, sizeof name), NULL, NULL);
    cr_assert_gt(g_topic, 0, "Failed to create prerequisite g_topic");

    /* Create a reader that keeps last sample of all instances. */
    dds_qset_history(g_qos, DDS_HISTORY_KEEP_LAST, 1);
    g_reader = dds_create_reader(g_participant, g_topic, g_qos, NULL);
    cr_assert_gt(g_reader, 0, "Failed to create prerequisite g_reader");

    /* Create a reader that will not automatically dispose unregistered samples. */
    dds_qset_writer_data_lifecycle(g_qos, false);
    g_writer = dds_create_writer(g_participant, g_topic, g_qos, NULL);
    cr_assert_gt(g_writer, 0, "Failed to create prerequisite g_writer");

    /* Sync g_reader to g_writer. */
    ret = dds_set_enabled_status(g_reader, DDS_SUBSCRIPTION_MATCHED_STATUS);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to set prerequisite g_reader status");
    ret = dds_waitset_attach(g_waitset, g_reader, g_reader);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to attach prerequisite g_reader");
    ret = dds_waitset_wait(g_waitset, &triggered, 1, DDS_SECS(1));
    cr_assert_eq(ret, 1, "Failed prerequisite dds_waitset_wait g_reader r");
    cr_assert_eq(g_reader, (dds_entity_t)(intptr_t)triggered, "Failed prerequisite dds_waitset_wait g_reader a");
    ret = dds_waitset_detach(g_waitset, g_reader);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to detach prerequisite g_reader");

    /* Sync g_writer to g_reader. */
    ret = dds_set_enabled_status(g_writer, DDS_PUBLICATION_MATCHED_STATUS);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to set prerequisite g_writer status");
    ret = dds_waitset_attach(g_waitset, g_writer, g_writer);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to attach prerequisite g_writer");
    ret = dds_waitset_wait(g_waitset, &triggered, 1, DDS_SECS(1));
    cr_assert_eq(ret, 1, "Failed prerequisite dds_waitset_wait g_writer r");
    cr_assert_eq(g_writer, (dds_entity_t)(intptr_t)triggered, "Failed prerequisite dds_waitset_wait g_writer a");
    ret = dds_waitset_detach(g_waitset, g_writer);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to detach prerequisite g_writer");

    /* Initialize reading buffers. */
    memset (g_data, 0, sizeof (g_data));
    for (int i = 0; i < MAX_SAMPLES; i++) {
        g_samples[i] = &g_data[i];
    }
    for (int i = 0; i < MAX_SAMPLES; i++) {
        g_loans[i] = NULL;
    }

    /* Write all samples. */
    for (int i = 0; i < MAX_SAMPLES; i++) {
        dds_instance_state_t ist = SAMPLE_IST(i);
        sample.long_1 = i;
        sample.long_2 = i/2;
        sample.long_3 = i/3;

        PRINT_SAMPLE("INIT: Write     ", sample);
        ret = dds_write(g_writer, &sample);
        cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite write");

        if (ist == DDS_IST_NOT_ALIVE_DISPOSED) {
            PRINT_SAMPLE("INIT: Dispose   ", sample);
            ret = dds_dispose(g_writer, &sample);
            cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite dispose");
        }
        if (ist == DDS_IST_NOT_ALIVE_NO_WRITERS) {
            PRINT_SAMPLE("INIT: Unregister", sample);
            ret = dds_unregister_instance(g_writer, &sample);
            cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite unregister");
        }
    }

    /* Read samples to get read&old_view states. */
    ret = dds_read(g_reader, g_samples, g_info, MAX_SAMPLES, SAMPLE_LAST_OLD_VST + 1);
    cr_assert_eq(ret, SAMPLE_LAST_OLD_VST + 1, "Failed prerequisite read");
    for(int i = 0; i < ret; i++) {
        Space_Type1 *s = (Space_Type1*)g_samples[i];
        PRINT_SAMPLE("INIT: Read      ", (*s));
    }

    /* Re-write the samples that should be not_read&old_view. */
    for (int i = SAMPLE_LAST_READ_SST + 1; i <= SAMPLE_LAST_OLD_VST; i++) {
        dds_instance_state_t ist = SAMPLE_IST(i);
        sample.long_1 = i;
        sample.long_2 = i/2;
        sample.long_3 = i/3;

        PRINT_SAMPLE("INIT: Rewrite   ", sample);
        ret = dds_write(g_writer, &sample);
        cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite write");

        if ((ist == DDS_IST_NOT_ALIVE_DISPOSED) && (i != 4)) {
            PRINT_SAMPLE("INIT: Dispose   ", sample);
            ret = dds_dispose(g_writer, &sample);
            cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite dispose");
        }
        if (ist == DDS_IST_NOT_ALIVE_NO_WRITERS) {
            PRINT_SAMPLE("INIT: Unregister", sample);
            ret = dds_unregister_instance(g_writer, &sample);
            cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite unregister");
        }
    }

}

static void
reader_fini(void)
{
    dds_qos_delete(g_qos);
    dds_listener_delete(g_listener);
    dds_delete(g_reader);
    dds_delete(g_writer);
    dds_delete(g_waitset);
    dds_delete(g_topic);
    dds_delete(g_participant);
}

static dds_return_t
samples_cnt(void)
{
    dds_return_t ret;
    ret = dds_read(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_geq(ret, 0, "Failed samples count read: %d", dds_err_nr(ret));
    return ret;
}

/**************************************************************************************************
 *
 * These will check the reader creation in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_reader_create, valid) = {
        DataPoints(dds_entity_t*,    &g_subscriber, &g_participant),
        DataPoints(dds_qos_t**,      &g_qos_null,   &g_qos        ),
        DataPoints(dds_listener_t**, &g_list_null,  &g_listener   ),
};
Theory((dds_entity_t *ent, dds_qos_t **qos, dds_listener_t **listener), ddsc_reader_create, valid, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t rdr;
    dds_return_t ret;
    rdr = dds_create_reader(*ent, g_topic, *qos, *listener);
    cr_assert_gt(rdr, 0, "Failed dds_create_reader(%p, %p, %p): %d", ent, *qos, *listener, dds_err_nr(rdr));
    ret = dds_delete(rdr);
    cr_assert_eq(ret, DDS_RETCODE_OK);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_reader_create, invalid_qos_participant, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t rdr;
    dds_qos_t *qos = dds_qos_create();
    /* Set invalid reader data lifecycle policy */
    OS_WARNING_MSVC_OFF(28020); /* Disable SAL warning on intentional misuse of the API */
    dds_qset_reader_data_lifecycle(qos, DDS_SECS(-1), DDS_SECS(-1));
    OS_WARNING_MSVC_ON(28020);
    rdr = dds_create_reader(g_participant, g_topic, qos, NULL);
    cr_assert_eq(dds_err_nr(rdr), DDS_RETCODE_INCONSISTENT_POLICY, "returned %d", dds_err_nr(rdr));
    dds_qos_delete(qos);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_reader_create, invalid_qos_subscriber, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t rdr;
    dds_qos_t *qos = dds_qos_create();
    /* Set invalid reader data lifecycle policy */
    OS_WARNING_MSVC_OFF(28020); /* Disable SAL warning on intentional misuse of the API */
    dds_qset_reader_data_lifecycle(qos, DDS_SECS(-1), DDS_SECS(-1));
    OS_WARNING_MSVC_ON(28020);
    rdr = dds_create_reader(g_subscriber, g_topic, qos, NULL);
    cr_assert_eq(dds_err_nr(rdr), DDS_RETCODE_INCONSISTENT_POLICY, "returned %d", dds_err_nr(rdr));
    dds_qos_delete(qos);
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_reader_create, non_participants_non_topics) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_reader, &g_waitset),
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_reader, &g_waitset),
};
Theory((dds_entity_t *par, dds_entity_t *top), ddsc_reader_create, non_participants_non_topics, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t rdr;
    /* The only valid permutation is when par is actual the participant and top is
     * actually the topic. So, don't test that permutation. */
    cr_assume((par != &g_participant) || (top != &g_topic));
    rdr = dds_create_reader(*par, *top, NULL, NULL);
    cr_assert_eq(dds_err_nr(rdr), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(rdr));
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_read, invalid_buffers) = {
        DataPoints(void**,             g_samples, g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,    (dds_sample_info_t*)0   ),
        DataPoints(size_t,             0,        3,  MAX_SAMPLES   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs), ddsc_read, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_samples) || (si != g_info) || (bufsz == 0) || (maxs == 0) || (bufsz < maxs));
    /* TODO: CHAM-306, currently, a buffer is automatically 'promoted' to a loan when a buffer is
     * provided with NULL pointers. So, in fact, there's currently no real difference between calling
     * dds_read() dds_read_wl() (except for the provided bufsz). This will change, which means that
     * the given buffer should contain valid pointers, which again means that 'loan intended' buffer
     * should result in bad_parameter.
     * However, that's not the case yet. So don't test it. */
    cr_assume(buf != g_loans);
    ret = dds_read(g_reader, buf, si, bufsz, maxs);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_read, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_read(rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_read, non_readers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    ret = dds_read(*rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read, already_deleted, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_read(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read, valid, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;

    ret = dds_read(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read::valid: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/






/**************************************************************************************************
 *
 * These will check the read_wl in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_read_wl, invalid_buffers) = {
        DataPoints(void**,             g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,  (dds_sample_info_t*)0   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, uint32_t maxs), ddsc_read_wl, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_loans) || (si != g_info) || (maxs == 0));
    ret = dds_read_wl(g_reader, buf, si, maxs);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_wl, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_read_wl, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_read_wl(rdr, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_wl, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_read_wl, non_readers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    ret = dds_read_wl(*rdr, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_wl, already_deleted, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_read_wl(g_reader, g_loans, g_info, MAX_SAMPLES);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_wl, valid, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;

    ret = dds_read_wl(g_reader, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read::valid: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read_mask in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask, invalid_buffers) = {
        DataPoints(void**,             g_samples, g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,    (dds_sample_info_t*)0   ),
        DataPoints(size_t,             0,        3,  MAX_SAMPLES   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs), ddsc_read_mask, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_samples) || (si != g_info) || (bufsz == 0) || (maxs == 0) || (bufsz < maxs));
    /* TODO: CHAM-306, currently, a buffer is automatically 'promoted' to a loan when a buffer is
     * provided with NULL pointers. So, in fact, there's currently no real difference between calling
     * dds_read_mask() dds_read_mask_wl() (except for the provided bufsz). This will change, which means that
     * the given buffer should contain valid pointers, which again means that 'loan intended' buffer
     * should result in bad_parameter.
     * However, that's not the case yet. So don't test it. */
    cr_assume(buf != g_loans);
    ret = dds_read_mask(g_reader, buf, si, bufsz, maxs, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_read_mask, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_read_mask(rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_read_mask, non_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    ret = dds_read_mask(*rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, already_deleted, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, any, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask::any: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, not_read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES - (SAMPLE_LAST_READ_SST + 1), "# read %d, expected %d", ret, MAX_SAMPLES - (SAMPLE_LAST_READ_SST + 1));
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask::not_read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_READ_SST + 1 + i;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, SAMPLE_LAST_READ_SST + 1, "# read %d, expected %d", ret, SAMPLE_LAST_READ_SST + 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = DDS_SST_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES - (SAMPLE_LAST_OLD_VST + 1), "# read %d, expected %d", ret, MAX_SAMPLES - (SAMPLE_LAST_OLD_VST + 1));
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask::new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_OLD_VST + 1 + i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_NEW;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, not_new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, SAMPLE_LAST_OLD_VST + 1, "# read %d, expected %d", ret, SAMPLE_LAST_OLD_VST + 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::not_new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 3, "# read %d, expected %d", ret, 3);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i * 3;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, not_alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 4, "# read %d, expected %d", ret, 4);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i <= 1) ? i + 1 : i + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,      true);
        cr_assert_eq(g_info[i].sample_state,    expected_sst);
        cr_assert_eq(g_info[i].view_state,      expected_vst);
        cr_assert_eq(g_info[i].instance_state,  expected_ist);
        cr_assert_neq(g_info[i].instance_state, DDS_IST_ALIVE);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, disposed_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 2, "# read %d, expected %d", ret, 2);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::disposed_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 1;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_DISPOSED;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, no_writers_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 2, "# read %d, expected %d", ret, 2);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::no_writers_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_NO_WRITERS;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask, combination_of_states, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 1, "# read %d, expected %d", ret, 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask::combination_of_states: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = 3;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read_mask_wl in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask_wl, invalid_buffers) = {
        DataPoints(void**,             g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,  (dds_sample_info_t*)0   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, uint32_t maxs), ddsc_read_mask_wl, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_loans) || (si != g_info) || (maxs == 0));
    ret = dds_read_mask_wl(g_reader, buf, si, maxs, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask_wl, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_read_mask_wl, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_read_mask_wl(rdr, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_read_mask_wl, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_read_mask_wl, non_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    ret = dds_read_mask_wl(*rdr, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, already_deleted, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, any, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::any: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, not_read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES - (SAMPLE_LAST_READ_SST + 1), "# read %d, expected %d", ret, MAX_SAMPLES - (SAMPLE_LAST_READ_SST + 1));
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::not_read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_READ_SST + 1 + i;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, SAMPLE_LAST_READ_SST + 1, "# read %d, expected %d", ret, SAMPLE_LAST_READ_SST + 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = DDS_SST_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, MAX_SAMPLES - (SAMPLE_LAST_OLD_VST + 1), "# read %d, expected %d", ret, MAX_SAMPLES - (SAMPLE_LAST_OLD_VST + 1));
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_OLD_VST + 1 + i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_NEW;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, not_new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, SAMPLE_LAST_OLD_VST + 1, "# read %d, expected %d", ret, SAMPLE_LAST_OLD_VST + 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::not_new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 3, "# read %d, expected %d", ret, 3);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i * 3;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, not_alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 4, "# read %d, expected %d", ret, 4);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i <= 1) ? i + 1 : i + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,      true);
        cr_assert_eq(g_info[i].sample_state,    expected_sst);
        cr_assert_eq(g_info[i].view_state,      expected_vst);
        cr_assert_eq(g_info[i].instance_state,  expected_ist);
        cr_assert_neq(g_info[i].instance_state, DDS_IST_ALIVE);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, disposed_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 2, "# read %d, expected %d", ret, 2);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::disposed_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 1;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_DISPOSED;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, no_writers_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 2, "# read %d, expected %d", ret, 2);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::no_writers_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_NO_WRITERS;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_read_mask_wl, combination_of_states, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_read_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, 1, "# read %d, expected %d", ret, 1);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_read_mask_wl::combination_of_states: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = 3;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* All samples should still be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES, "# samples %d, expected %d", ret, MAX_SAMPLES);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the take in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_take, invalid_buffers) = {
        DataPoints(void**,             g_samples, g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,    (dds_sample_info_t*)0   ),
        DataPoints(size_t,             0,        3,  MAX_SAMPLES   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs), ddsc_take, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_samples) || (si != g_info) || (bufsz == 0) || (maxs == 0) || (bufsz < maxs));
    /* TODO: CHAM-306, currently, a buffer is automatically 'promoted' to a loan when a buffer is
     * provided with NULL pointers. So, in fact, there's currently no real difference between calling
     * dds_take() dds_take_wl() (except for the provided bufsz). This will change, which means that
     * the given buffer should contain valid pointers, which again means that 'loan intended' buffer
     * should result in bad_parameter.
     * However, that's not the case yet. So don't test it. */
    cr_assume(buf != g_loans);
    ret = dds_take(g_reader, buf, si, bufsz, maxs);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_take, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_take(rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_take, non_readers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    ret = dds_take(*rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take, already_deleted, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* Try to take with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_take(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take, valid, .init=reader_init, .fini=reader_fini)
{
    dds_return_t expected_cnt = MAX_SAMPLES;
    dds_return_t ret;

    ret = dds_take(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_read::valid: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read_wl in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_take_wl, invalid_buffers) = {
        DataPoints(void**,             g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,  (dds_sample_info_t*)0   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, uint32_t maxs), ddsc_take_wl, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_loans) || (si != g_info) || (maxs == 0));
    ret = dds_take_wl(g_reader, buf, si, maxs);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_wl, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_take_wl, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_take_wl(rdr, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_wl, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_take_wl, non_readers, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    ret = dds_take_wl(*rdr, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_wl, already_deleted, .init=reader_init, .fini=reader_fini)
{
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_take_wl(g_reader, g_loans, g_info, MAX_SAMPLES);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_wl, valid, .init=reader_init, .fini=reader_fini)
{
    dds_return_t expected_cnt = MAX_SAMPLES;
    dds_return_t ret;

    ret = dds_take_wl(g_reader, g_loans, g_info, MAX_SAMPLES);
    cr_assert_eq(ret, MAX_SAMPLES, "# read %d, expected %d", ret, MAX_SAMPLES);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take::valid: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read_mask in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask, invalid_buffers) = {
        DataPoints(void**,             g_samples, g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,    (dds_sample_info_t*)0   ),
        DataPoints(size_t,             0,        3,  MAX_SAMPLES   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs), ddsc_take_mask, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_samples) || (si != g_info) || (bufsz == 0) || (maxs == 0) || (bufsz < maxs));
    /* TODO: CHAM-306, currently, a buffer is automatically 'promoted' to a loan when a buffer is
     * provided with NULL pointers. So, in fact, there's currently no real difference between calling
     * dds_take_mask() dds_take_mask_wl() (except for the provided bufsz). This will change, which means that
     * the given buffer should contain valid pointers, which again means that 'loan intended' buffer
     * should result in bad_parameter.
     * However, that's not the case yet. So don't test it. */
    cr_assume(buf != g_loans);
    ret = dds_take_mask(g_reader, buf, si, bufsz, maxs, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_take_mask, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_take_mask(rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_take_mask, non_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    ret = dds_take_mask(*rdr, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, already_deleted, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, any, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = MAX_SAMPLES;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask::any: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, not_read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask::not_read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_READ_SST + 1 + i;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = DDS_SST_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask::new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_OLD_VST + 1 + i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_NEW;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, not_new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::not_new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i * 3;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, not_alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i <= 1) ? i + 1 : i + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,      true);
        cr_assert_eq(g_info[i].sample_state,    expected_sst);
        cr_assert_eq(g_info[i].view_state,      expected_vst);
        cr_assert_eq(g_info[i].instance_state,  expected_ist);
        cr_assert_neq(g_info[i].instance_state, DDS_IST_ALIVE);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, disposed_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE;
    dds_return_t expected_cnt = 2;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::disposed_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 1;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_DISPOSED;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, no_writers_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t expected_cnt = 2;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::no_writers_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_NO_WRITERS;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, combination_of_states, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t expected_cnt = 1;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_samples[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask::combination_of_states: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = 3;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask, take_instance_last_sample)
{
#define WOULD_CRASH
#ifdef WOULD_CRASH
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_sample_state_t expected_sst = DDS_SST_NOT_READ;
    int expected_long_3 = 3;
#else
    uint32_t mask = DDS_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_sample_state_t expected_sst = DDS_SST_READ;
    int expected_long_3 = 2;
#endif
    dds_return_t expected_cnt = 1;
    Space_Type1 sample = { 0 };
    dds_attach_t triggered;
    dds_return_t ret;
    char name[100];

    /* We need other readers/writers/data to force the crash. */
    g_qos = dds_qos_create();
    dds_qset_history(g_qos, DDS_HISTORY_KEEP_ALL, DDS_LENGTH_UNLIMITED);
    g_participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    cr_assert_gt(g_participant, 0, "Failed to create prerequisite g_participant");
    g_waitset = dds_create_waitset(g_participant);
    cr_assert_gt(g_waitset, 0, "Failed to create g_waitset");
    g_topic = dds_create_topic(g_participant, &Space_Type1_desc, create_topic_name("ddsc_reader_test", name, 100), NULL, NULL);
    cr_assert_gt(g_topic, 0, "Failed to create prerequisite g_topic");
    g_reader = dds_create_reader(g_participant, g_topic, g_qos, NULL);
    cr_assert_gt(g_reader, 0, "Failed to create prerequisite g_reader");
    g_writer = dds_create_writer(g_participant, g_topic, NULL, NULL);
    cr_assert_gt(g_writer, 0, "Failed to create prerequisite g_writer");

    /* Sync g_reader to g_writer. */
    ret = dds_set_enabled_status(g_reader, DDS_SUBSCRIPTION_MATCHED_STATUS);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to set prerequisite g_reader status");
    ret = dds_waitset_attach(g_waitset, g_reader, g_reader);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to attach prerequisite g_reader");
    ret = dds_waitset_wait(g_waitset, &triggered, 1, DDS_SECS(1));
    cr_assert_eq(ret, 1, "Failed prerequisite dds_waitset_wait g_reader r");
    cr_assert_eq(g_reader, (dds_entity_t)(intptr_t)triggered, "Failed prerequisite dds_waitset_wait g_reader a");
    ret = dds_waitset_detach(g_waitset, g_reader);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to detach prerequisite g_reader");

    /* Sync g_writer to g_reader. */
    ret = dds_set_enabled_status(g_writer, DDS_PUBLICATION_MATCHED_STATUS);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to set prerequisite g_writer status");
    ret = dds_waitset_attach(g_waitset, g_writer, g_writer);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to attach prerequisite g_writer");
    ret = dds_waitset_wait(g_waitset, &triggered, 1, DDS_SECS(1));
    cr_assert_eq(ret, 1, "Failed prerequisite dds_waitset_wait g_writer r");
    cr_assert_eq(g_writer, (dds_entity_t)(intptr_t)triggered, "Failed prerequisite dds_waitset_wait g_writer a");
    ret = dds_waitset_detach(g_waitset, g_writer);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed to detach prerequisite g_writer");

    /* Initialize reading buffers. */
    memset (g_data, 0, sizeof (g_data));
    for (int i = 0; i < MAX_SAMPLES; i++) {
        g_samples[i] = &g_data[i];
    }

    /* Generate following data:
     *  | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
     *  ----------------------------------------------------------
     *  |    0   |    1   |    2   |     read | old | alive      |
     *  |    0   |    1   |    3   | not_read | old | alive      |
     */
    sample.long_1 = 0;
    sample.long_2 = 1;
    sample.long_3 = 2;
    ret = dds_write(g_writer, &sample);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite write");
    ret = dds_read(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES);
    cr_assert_eq(ret, 1, "Failed prerequisite read");
    sample.long_3 = 3;
    ret = dds_write(g_writer, &sample);
    cr_assert_eq(ret, DDS_RETCODE_OK, "Failed prerequisite write");

    /* Take just one sample of the instance (the last one). */
    ret = dds_take_mask(g_reader, g_samples, g_info, MAX_SAMPLES, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *s = (Space_Type1*)g_samples[i];

        /*
         *  | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         *  ----------------------------------------------------------
         *  |    0   |    1   |    2   |     read | old | alive      | <--- no worries
         *  |    0   |    1   |    3   | not_read | old | alive      | <--- crashed
         */
        PRINT_SAMPLE("ddsc_take_mask::crash: Take", (*s));

        /* Check data. */
        cr_assert_eq(s->long_1, 0);
        cr_assert_eq(s->long_2, 1);
        cr_assert_eq(s->long_3, expected_long_3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     DDS_VST_OLD);
        cr_assert_eq(g_info[i].instance_state, DDS_IST_ALIVE);
    }

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, 1, "# samples %d, expected %d", ret,  1);

    /*
     * So far so good.
     * But now the problem appeared:
     * The reader crashed when deleting....
     */
    dds_delete(g_reader);

    /* Before the crash was fixed, we didn't come here. */
    dds_delete(g_writer);
    dds_delete(g_waitset);
    dds_delete(g_topic);
    dds_delete(g_participant);
    dds_qos_delete(g_qos);
}
/*************************************************************************************************/





/**************************************************************************************************
 *
 * These will check the read_mask_wl in various ways.
 *
 *************************************************************************************************/
/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask_wl, invalid_buffers) = {
        DataPoints(void**,             g_loans, (void**)0),
        DataPoints(dds_sample_info_t*, g_info,  (dds_sample_info_t*)0   ),
        DataPoints(uint32_t,           0,        3,  MAX_SAMPLES   ),
};
Theory((void **buf, dds_sample_info_t *si, uint32_t maxs), ddsc_take_mask_wl, invalid_buffers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* The only valid permutation is when non of the buffer values are
     * invalid. So, don't test that. */
    cr_assume((buf != g_loans) || (si != g_info) || (maxs == 0));
    ret = dds_take_mask_wl(g_reader, buf, si, maxs, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_BAD_PARAMETER, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask_wl, invalid_readers) = {
        DataPoints(dds_entity_t, -2, -1, 0, 1, 100, INT_MAX, INT_MIN),
};
Theory((dds_entity_t rdr), ddsc_take_mask_wl, invalid_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_entity_t exp = DDS_RETCODE_BAD_PARAMETER * -1;
    dds_return_t ret;

    ret = dds_take_mask_wl(rdr, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), dds_err_nr(exp), "returned %d != expected %d", dds_err_nr(ret), dds_err_nr(exp));
}
/*************************************************************************************************/

/*************************************************************************************************/
TheoryDataPoints(ddsc_take_mask_wl, non_readers) = {
        DataPoints(dds_entity_t*, &g_participant, &g_topic, &g_writer, &g_subscriber, &g_waitset),
};
Theory((dds_entity_t *rdr), ddsc_take_mask_wl, non_readers, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    ret = dds_take_mask_wl(*rdr, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(dds_err_nr(ret), DDS_RETCODE_ILLEGAL_OPERATION, "returned %d", dds_err_nr(ret));
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, already_deleted, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t ret;
    /* Try to read with a deleted reader. */
    dds_delete(g_reader);
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_expect_eq(dds_err_nr(ret), DDS_RETCODE_ALREADY_DELETED);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, any, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = MAX_SAMPLES;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::any: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, not_read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::not_read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_READ_SST + 1 + i;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, read_sample_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::read_sample_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = DDS_SST_READ;
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = SAMPLE_LAST_OLD_VST + 1 + i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_NEW;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, not_new_view_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::not_new_view_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t expected_cnt = 3;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      | <---
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      | <---
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = i * 3;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, not_alive_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t expected_cnt = 4;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::alive_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i <= 1) ? i + 1 : i + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = SAMPLE_IST(expected_long_1);

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,      true);
        cr_assert_eq(g_info[i].sample_state,    expected_sst);
        cr_assert_eq(g_info[i].view_state,      expected_vst);
        cr_assert_eq(g_info[i].instance_state,  expected_ist);
        cr_assert_neq(g_info[i].instance_state, DDS_IST_ALIVE);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, disposed_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_DISPOSED_INSTANCE_STATE;
    dds_return_t expected_cnt = 2;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   | <---
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   | <---
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::disposed_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 1;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_DISPOSED;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, no_writers_instance_state, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_ANY_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_NOT_ALIVE_NO_WRITERS_INSTANCE_STATE;
    dds_return_t expected_cnt = 2;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers | <---
         * |    3   |    1   |    1   | not_read | old | alive      |
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers | <---
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::no_writers_instance_state: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = (i * 3) + 2;
        dds_sample_state_t   expected_sst    = SAMPLE_SST(expected_long_1);
        dds_view_state_t     expected_vst    = SAMPLE_VST(expected_long_1);
        dds_instance_state_t expected_ist    = DDS_IST_NOT_ALIVE_NO_WRITERS;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/

/*************************************************************************************************/
Test(ddsc_take_mask_wl, combination_of_states, .init=reader_init, .fini=reader_fini)
{
    uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_NOT_NEW_VIEW_STATE | DDS_ALIVE_INSTANCE_STATE;
    dds_return_t expected_cnt = 1;
    dds_return_t ret;

    /* Read all samples that matches the mask. */
    ret = dds_take_mask_wl(g_reader, g_loans, g_info, MAX_SAMPLES, mask);
    cr_assert_eq(ret, expected_cnt, "# read %d, expected %d", ret, expected_cnt);
    for(int i = 0; i < ret; i++) {
        Space_Type1 *sample = (Space_Type1*)g_loans[i];

        /*
         * | long_1 | long_2 | long_3 |    sst   | vst |    ist     |
         * ----------------------------------------------------------
         * |    0   |    0   |    0   |     read | old | alive      |
         * |    1   |    0   |    0   |     read | old | disposed   |
         * |    2   |    1   |    0   |     read | old | no_writers |
         * |    3   |    1   |    1   | not_read | old | alive      | <---
         * |    4   |    2   |    1   | not_read | new | disposed   |
         * |    5   |    2   |    1   | not_read | new | no_writers |
         * |    6   |    3   |    2   | not_read | new | alive      |
         */
        PRINT_SAMPLE("ddsc_take_mask_wl::combination_of_states: Read", (*sample));

        /* Expected states. */
        int                  expected_long_1 = 3;
        dds_sample_state_t   expected_sst    = DDS_SST_NOT_READ;
        dds_view_state_t     expected_vst    = DDS_VST_OLD;
        dds_instance_state_t expected_ist    = DDS_IST_ALIVE;

        /* Check data. */
        cr_assert_eq(sample->long_1, expected_long_1  );
        cr_assert_eq(sample->long_2, expected_long_1/2);
        cr_assert_eq(sample->long_3, expected_long_1/3);

        /* Check states. */
        cr_assert_eq(g_info[i].valid_data,     true);
        cr_assert_eq(g_info[i].sample_state,   expected_sst);
        cr_assert_eq(g_info[i].view_state,     expected_vst);
        cr_assert_eq(g_info[i].instance_state, expected_ist);
    }

    ret = dds_return_loan(g_reader, g_loans, ret);
    cr_assert_eq (ret, DDS_RETCODE_OK);

    /* Only samples that weren't taken should be available. */
    ret = samples_cnt();
    cr_assert_eq(ret, MAX_SAMPLES - expected_cnt, "# samples %d, expected %d", ret,  MAX_SAMPLES - expected_cnt);
}
/*************************************************************************************************/
