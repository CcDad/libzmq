/*
    Copyright (c) 2007-2020 Contributors as noted in the AUTHORS file

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

SETUP_TEARDOWN_TESTCONTEXT

void test (const char *address)
{
    //  Create a publisher
    void *publisher = test_context_socket (ZMQ_PUB);
    char my_endpoint[MAX_SOCKET_STRING];

    //  Bind publisher
    test_bind (publisher, address, my_endpoint, MAX_SOCKET_STRING);

    //  Create a subscriber
    void *subscriber = test_context_socket (ZMQ_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (subscriber, my_endpoint));

    //  Subscribe to all messages.
    TEST_ASSERT_SUCCESS_ERRNO (
      zmq_setsockopt (subscriber, ZMQ_SUBSCRIBE, "", 0));

    //  Wait a bit till the subscription gets to the publisher
    msleep (SETTLE_TIME);

    //  Send an empty message
    send_string_expect_success (publisher, "test", 0);

    //  Receive the message in the subscriber
    recv_string_expect_success (subscriber, "test", 0);

    //  Clean up.
    test_context_socket_close (publisher);
    test_context_socket_close (subscriber);
}

void test_norm ()
{
    test ("norm://127.0.0.1:*");
}

void test_tcp ()
{
    test ("tcp://127.0.0.1:*");
}

void test_inproc ()
{
    test ("inproc://hello-msg");
}

void test_inproc_late_bind ()
{
    char address[] = "inproc://late-hello-msg";

    //  Create a server
    void *server = test_context_socket (ZMQ_SERVER);

    //  set server socket options
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (server, ZMQ_HELLO_MSG, "W", 1));

    //  Create a dealer
    void *client = test_context_socket (ZMQ_CLIENT);
    TEST_ASSERT_SUCCESS_ERRNO (zmq_setsockopt (client, ZMQ_HELLO_MSG, "H", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zmq_connect (client, address));

    //  bind server after the dealer
    TEST_ASSERT_SUCCESS_ERRNO (zmq_bind (server, address));

    // Receive the welcome message from server
    recv_string_expect_success (client, "W", 0);

    // Receive the hello message from client
    recv_string_expect_success (server, "H", 0);

    //  Clean up.
    test_context_socket_close (client);
    test_context_socket_close (server);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_tcp);
    RUN_TEST (test_inproc);
    RUN_TEST (test_inproc_late_bind);
    RUN_TEST (test_norm);
    return UNITY_END ();
}
