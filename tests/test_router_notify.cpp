/*
    Copyright (c) 2007-2017 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <unity.h>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

void test_setsockopt_router_notify ()
{
    void *router = test_context_socket (ZMQ_ROUTER);
    int notify_opt;

    // valid values
    notify_opt = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &notify_opt, sizeof (notify_opt)));

    notify_opt = ZMQ_NOTIFY_CONNECT;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &notify_opt, sizeof (notify_opt)));

    notify_opt = ZMQ_NOTIFY_DISCONNECT;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &notify_opt, sizeof (notify_opt)));

    notify_opt = ZMQ_NOTIFY_CONNECT | ZMQ_NOTIFY_DISCONNECT;
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &notify_opt, sizeof (notify_opt)));

    // value boundary
    notify_opt = -1;
    TEST_ASSERT_FAILURE_ERRNO (
      EINVAL, zmq_setsockopt (router, ZMQ_ROUTER_NOTIFY, &notify_opt,
                              sizeof (notify_opt)));

    notify_opt = (ZMQ_NOTIFY_CONNECT | ZMQ_NOTIFY_DISCONNECT) + 1;
    TEST_ASSERT_FAILURE_ERRNO (
      EINVAL, zmq_setsockopt (router, ZMQ_ROUTER_NOTIFY, &notify_opt,
                              sizeof (notify_opt)));

    test_context_socket_close (router);

    //check a non-router socket type
    void *dealer = test_context_socket (ZMQ_DEALER);
    notify_opt = ZMQ_NOTIFY_CONNECT;
    TEST_ASSERT_FAILURE_ERRNO (
      EINVAL, zmq_setsockopt (dealer, ZMQ_ROUTER_NOTIFY, &notify_opt,
                              sizeof (notify_opt)));
    test_context_socket_close (dealer);
}


void test_router_notify_helper (int notify_opt)
{
    void *router = test_context_socket (ZMQ_ROUTER);
    int opt_more;
    size_t opt_more_length = sizeof (opt_more);
    int opt_events;
    size_t opt_events_length = sizeof (opt_events);


    // valid values
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &notify_opt, sizeof (notify_opt)));

    TEST_ASSERT_SUCCESS_ERRNO (zmq_bind (router, ENDPOINT_0));

    void *dealer = test_context_socket (ZMQ_DEALER);
    const char *dealer_routing_id = "X";

    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (dealer, ZMQ_ROUTING_ID, dealer_routing_id, 1));

    // dealer connects
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (dealer, ENDPOINT_0));

    // connection notification msg
    if (notify_opt & ZMQ_NOTIFY_CONNECT) {
        // routing-id only message of the connect
        recv_string_expect_success (router, dealer_routing_id,
                                    0);             // 1st part: routing-id
        recv_string_expect_success (router, "", 0); // 2nd part: empty
        TEST_ASSERT_SUCCESS_ERRNO (
          zmq_getsockopt (router, ZMQ_RCVMORE, &opt_more, &opt_more_length));
        TEST_ASSERT_EQUAL (0, opt_more);
    }

    // test message from the dealer
    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, dealer_routing_id, 0);
    recv_string_expect_success (router, "Hello", 0);
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_getsockopt (router, ZMQ_RCVMORE, &opt_more, &opt_more_length));
    TEST_ASSERT_EQUAL (0, opt_more);

    // dealer disconnects
    TEST_ASSERT_SUCCESS_ERRNO (zmq_disconnect (dealer, ENDPOINT_0));

    // need one more process_commands() (???)
    msleep (SETTLE_TIME);
    zmq_getsockopt (dealer, ZMQ_EVENTS, &opt_events, &opt_events_length);

    // connection notification msg
    if (notify_opt & ZMQ_NOTIFY_DISCONNECT) {
        // routing-id only message of the connect
        printf ("ehe\n");
        recv_string_expect_success (router, dealer_routing_id,
                                    0);             // 1st part: routing-id
        recv_string_expect_success (router, "", 0); // 2nd part: empty
        TEST_ASSERT_SUCCESS_ERRNO (
          zmq_getsockopt (router, ZMQ_RCVMORE, &opt_more, &opt_more_length));
        TEST_ASSERT_EQUAL (0, opt_more);
    }

    test_context_socket_close (dealer);
    test_context_socket_close (router);
}


void test_router_notify_connect ()
{
    test_router_notify_helper (ZMQ_NOTIFY_CONNECT);
}


void test_router_notify_disconnect ()
{
    test_router_notify_helper (ZMQ_NOTIFY_DISCONNECT);
}


void test_router_notify_both ()
{
    test_router_notify_helper (ZMQ_NOTIFY_CONNECT | ZMQ_NOTIFY_DISCONNECT);
}


void test_handshake_fail ()
{
    void *router = test_context_socket (ZMQ_ROUTER);
    int opt_timeout = 200;
    int opt_notify = ZMQ_NOTIFY_CONNECT | ZMQ_NOTIFY_DISCONNECT;

    // valid values
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_ROUTER_NOTIFY, &opt_notify, sizeof (opt_notify)));

    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (
      router, ZMQ_RCVTIMEO, &opt_timeout, sizeof (opt_timeout)));

    TEST_ASSERT_SUCCESS_ERRNO (zmq_bind (router, ENDPOINT_0));

    // send something on raw tcp
    void *stream = test_context_socket (ZMQ_STREAM);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (stream, ENDPOINT_0));

    send_string_expect_success (stream, "not-a-handshake", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zmq_disconnect (stream, ENDPOINT_0));
    test_context_socket_close (stream);

    // no notification delivered
    char buffer[255];
    TEST_ASSERT_FAILURE_ERRNO (EAGAIN,
                               zmq_recv (router, buffer, sizeof (buffer), 0));

    test_context_socket_close (router);
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_setsockopt_router_notify);
    RUN_TEST (test_router_notify_connect);
    RUN_TEST (test_router_notify_disconnect);
    RUN_TEST (test_router_notify_both);
    RUN_TEST (test_handshake_fail);

    return UNITY_END ();
}
