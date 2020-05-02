/*
    Copyright (c) 2020 Contributors as noted in the AUTHORS file

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

#ifdef ZMQ_USE_FUZZING_ENGINE
#include <fuzzer/FuzzedDataProvider.h>
#endif

#include "testutil.hpp"
#include "testutil_security.hpp"

// Test that the ZMTP engine handles invalid handshake when connecting
// https://rfc.zeromq.org/spec/37/
extern "C" int LLVMFuzzerTestOneInput (const uint8_t *data, size_t size)
{
    setup_test_context ();
    setup_testutil_security_curve ();
    char my_endpoint[MAX_SOCKET_STRING];
    fd_t server = bind_socket_resolve_port ("127.0.0.1", "0", my_endpoint);

    curve_client_data_t curve_client_data = {
      valid_server_public, valid_client_public, valid_client_secret};
    void *client_mon;
    void *client = create_and_connect_client (
      my_endpoint, socket_config_curve_client, &curve_client_data, &client_mon);

    fd_t server_accept =
      TEST_ASSERT_SUCCESS_RAW_ERRNO (accept (server, NULL, NULL));
    for (ssize_t sent = 0; size > 0 && (sent != -1 || errno == EINTR);
         size -= sent > 0 ? sent : 0, data += sent > 0 ? sent : 0)
        sent = send (server_accept, (const char *) data, size, MSG_NOSIGNAL);
    msleep (250);

    close (server_accept);
    close (server);

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (client_mon);
    teardown_test_context ();

    return 0;
}

#ifndef ZMQ_USE_FUZZING_ENGINE
void test_connect_curve_fuzzer ()
{
    TEST_ASSERT_SUCCESS_ERRNO (LLVMFuzzerTestOneInput (
      zmtp_greeting_curve, sizeof (zmtp_greeting_curve)));
}

int main (int argc, char **argv)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_connect_curve_fuzzer);

    return UNITY_END ();
}
#endif
