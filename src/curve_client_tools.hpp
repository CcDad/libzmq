/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

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

#ifndef __ZMQ_CURVE_CLIENT_TOOLS_HPP_INCLUDED__
#define __ZMQ_CURVE_CLIENT_TOOLS_HPP_INCLUDED__

#ifdef ZMQ_HAVE_CURVE

#if defined(ZMQ_USE_TWEETNACL)
#include "tweetnacl.h"
#elif defined(ZMQ_USE_LIBSODIUM)
#include "sodium.h"
#endif

#if crypto_box_NONCEBYTES != 24 || crypto_box_PUBLICKEYBYTES != 32             \
  || crypto_box_SECRETKEYBYTES != 32 || crypto_box_ZEROBYTES != 32             \
  || crypto_box_BOXZEROBYTES != 16
#error "CURVE library not built properly"
#endif

#include "wire.hpp"
#include "err.hpp"

namespace zmq
{
struct curve_client_tools_t
{
    static int produce_hello (void *data,
                              const uint8_t *server_key,
                              const uint64_t cn_nonce,
                              const uint8_t *cn_public,
                              const uint8_t *cn_secret)
    {
        uint8_t hello_nonce[crypto_box_NONCEBYTES];
        uint8_t hello_plaintext[crypto_box_ZEROBYTES + 64];
        uint8_t hello_box[crypto_box_BOXZEROBYTES + 80];

        //  Prepare the full nonce
        memcpy (hello_nonce, "CurveZMQHELLO---", 16);
        put_uint64 (hello_nonce + 16, cn_nonce);

        //  Create Box [64 * %x0](C'->S)
        memset (hello_plaintext, 0, sizeof hello_plaintext);

        int rc = crypto_box (hello_box, hello_plaintext, sizeof hello_plaintext,
                             hello_nonce, server_key, cn_secret);
        if (rc == -1)
            return -1;

        uint8_t *hello = static_cast<uint8_t *> (data);

        memcpy (hello, "\x05HELLO", 6);
        //  CurveZMQ major and minor version numbers
        memcpy (hello + 6, "\1\0", 2);
        //  Anti-amplification padding
        memset (hello + 8, 0, 72);
        //  Client public connection key
        memcpy (hello + 80, cn_public, crypto_box_PUBLICKEYBYTES);
        //  Short nonce, prefixed by "CurveZMQHELLO---"
        memcpy (hello + 112, hello_nonce + 16, 8);
        //  Signature, Box [64 * %x0](C'->S)
        memcpy (hello + 120, hello_box + crypto_box_BOXZEROBYTES, 80);

        return 0;
    }

    static int process_welcome (const uint8_t *msg_data,
                                size_t msg_size,
                                const uint8_t *server_key,
                                const uint8_t *cn_secret,
                                uint8_t *cn_server,
                                uint8_t *cn_cookie,
                                uint8_t *cn_precom)
    {
        if (msg_size != 168) {
            errno = EPROTO;
            return -1;
        }

        uint8_t welcome_nonce[crypto_box_NONCEBYTES];
        uint8_t welcome_plaintext[crypto_box_ZEROBYTES + 128];
        uint8_t welcome_box[crypto_box_BOXZEROBYTES + 144];

        //  Open Box [S' + cookie](C'->S)
        memset (welcome_box, 0, crypto_box_BOXZEROBYTES);
        memcpy (welcome_box + crypto_box_BOXZEROBYTES, msg_data + 24, 144);

        memcpy (welcome_nonce, "WELCOME-", 8);
        memcpy (welcome_nonce + 8, msg_data + 8, 16);

        int rc =
          crypto_box_open (welcome_plaintext, welcome_box, sizeof welcome_box,
                           welcome_nonce, server_key, cn_secret);
        if (rc != 0) {
            errno = EPROTO;
            return -1;
        }

        memcpy (cn_server, welcome_plaintext + crypto_box_ZEROBYTES, 32);
        memcpy (cn_cookie, welcome_plaintext + crypto_box_ZEROBYTES + 32,
                16 + 80);

        //  Message independent precomputation
        rc = crypto_box_beforenm (cn_precom, cn_server, cn_secret);
        zmq_assert (rc == 0);

        return 0;
    }

    static bool is_handshake_command_welcome (const uint8_t *msg_data,
                                              const size_t msg_size)
    {
        return is_handshake_command (msg_data, msg_size, "\7WELCOME");
    }

    static bool is_handshake_command_ready (const uint8_t *msg_data,
                                            const size_t msg_size)
    {
        return is_handshake_command (msg_data, msg_size, "\5READY");
    }

    static bool is_handshake_command_error (const uint8_t *msg_data,
                                            const size_t msg_size)
    {
        return is_handshake_command (msg_data, msg_size, "\5ERROR");
    }
private:
  template <size_t N>
  static bool is_handshake_command (const uint8_t *msg_data,
                                    const size_t msg_size,
                                    const char (&prefix)[N])
  {
      return msg_size >= (N - 1) && !memcmp (msg_data, prefix, N - 1);
  }
};
}

#endif

#endif
