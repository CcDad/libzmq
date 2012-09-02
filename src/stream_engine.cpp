/*
    Copyright (c) 2009-2011 250bpm s.r.o.
    Copyright (c) 2007-2009 iMatix Corporation
    Copyright (c) 2007-2011 Other contributors as noted in the AUTHORS file

    This file is part of 0MQ.

    0MQ is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    0MQ is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "platform.hpp"
#if defined ZMQ_HAVE_WINDOWS
#include "windows.hpp"
#else
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#endif

#include <string.h>
#include <new>

#include "stream_engine.hpp"
#include "io_thread.hpp"
#include "session_base.hpp"
#include "config.hpp"
#include "err.hpp"
#include "ip.hpp"
#include "likely.hpp"
#include "wire.hpp"

zmq::stream_engine_t::stream_engine_t (fd_t fd_, const options_t &options_, const std::string &endpoint_) :
    s (fd_),
    inpos (NULL),
    insize (0),
    decoder (in_batch_size, options_.maxmsgsize),
    input_error (false),
    outpos (NULL),
    outsize (0),
    encoder (out_batch_size),
    handshaking (true),
    greeting_bytes_read (0),
    greeting_size (0),
    session (NULL),
    options (options_),
    endpoint (endpoint_),
    plugged (false)
{
    //  Put the socket into non-blocking mode.
    unblock_socket (s);
    //  Set the socket buffer limits for the underlying socket.
    if (options.sndbuf) {
        int rc = setsockopt (s, SOL_SOCKET, SO_SNDBUF,
            (char*) &options.sndbuf, sizeof (int));
#ifdef ZMQ_HAVE_WINDOWS
		wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }
    if (options.rcvbuf) {
        int rc = setsockopt (s, SOL_SOCKET, SO_RCVBUF,
            (char*) &options.rcvbuf, sizeof (int));
#ifdef ZMQ_HAVE_WINDOWS
		wsa_assert (rc != SOCKET_ERROR);
#else
        errno_assert (rc == 0);
#endif
    }

#ifdef SO_NOSIGPIPE
    //  Make sure that SIGPIPE signal is not generated when writing to a
    //  connection that was already closed by the peer.
    int set = 1;
    int rc = setsockopt (s, SOL_SOCKET, SO_NOSIGPIPE, &set, sizeof (int));
    errno_assert (rc == 0);
#endif
}

zmq::stream_engine_t::~stream_engine_t ()
{
    zmq_assert (!plugged);

    if (s != retired_fd) {
#ifdef ZMQ_HAVE_WINDOWS
		int rc = closesocket (s);
		wsa_assert (rc != SOCKET_ERROR);
#else
		int rc = close (s);
        errno_assert (rc == 0);
#endif
		s = retired_fd;
    }
}

void zmq::stream_engine_t::plug (io_thread_t *io_thread_,
    session_base_t *session_)
{
    zmq_assert (!plugged);
    plugged = true;

    //  Connect to session object.
    zmq_assert (!session);
    zmq_assert (session_);
    session = session_;

    //  Connect to I/O threads poller object.
    io_object_t::plug (io_thread_);
    handle = add_fd (s);

    //  We need to detect whether our peer is using the versioned
    //  protocol. The detection is done in two steps. First, we read
    //  first two bytes and check if the long format of length is in use.
    //  If so, we receive and check the 'flags' field. If the rightmost bit
    //  is 1, the peer is using versioned protocol.
    greeting_size = 2;

    //  Send the 'length' and 'flags' fields of the identity message.
    //  The 'length' field is encoded in the long format.
    outpos = greeting_output_buffer;
    outpos [outsize++] = 0xff;
    put_uint64 (&outpos [outsize], options.identity_size + 1);
    outsize += 8;
    outpos [outsize++] = 0x7f;

    set_pollin (handle);
    set_pollout (handle);
    //  Flush all the data that may have been already received downstream.
    in_event ();
}

void zmq::stream_engine_t::unplug ()
{
    zmq_assert (plugged);
    plugged = false;

    //  Cancel all fd subscriptions.
    rm_fd (handle);

    //  Disconnect from I/O threads poller object.
    io_object_t::unplug ();

    //  Disconnect from session object.
    encoder.set_msg_source (NULL);
    decoder.set_msg_sink (NULL);
    session = NULL;
}

void zmq::stream_engine_t::terminate ()
{
    unplug ();
    delete this;
}

void zmq::stream_engine_t::in_event ()
{
    //  If still handshaking, receive and prcess the greeting message.
    if (unlikely (handshaking))
        if (!handshake ())
            return;

    bool disconnection = false;

    //  If there's no data to process in the buffer...
    if (!insize) {

        //  Retrieve the buffer and read as much data as possible.
        //  Note that buffer can be arbitrarily large. However, we assume
        //  the underlying TCP layer has fixed buffer size and thus the
        //  number of bytes read will be always limited.
        decoder.get_buffer (&inpos, &insize);
        insize = read (inpos, insize);

        //  Check whether the peer has closed the connection.
        if (insize == (size_t) -1) {
            insize = 0;
            disconnection = true;
        }
    }

    //  Push the data to the decoder.
    size_t processed = decoder.process_buffer (inpos, insize);

    if (unlikely (processed == (size_t) -1)) {
        disconnection = true;
    }
    else {

        //  Stop polling for input if we got stuck.
        if (processed < insize)
            reset_pollin (handle);

        //  Adjust the buffer.
        inpos += processed;
        insize -= processed;
    }

    //  Flush all messages the decoder may have produced.
    session->flush ();

    //  Input error has occurred. If the last decoded
    //  message has already been accepted, we terminate
    //  the engine immediately. Otherwise, we stop
    //  waiting for input events and postpone the termination
    //  until after the session has accepted the message.
    if (disconnection) {
        input_error = true;
        if (decoder.stalled ())
            reset_pollin (handle);
        else
            error ();
    }
}

void zmq::stream_engine_t::out_event ()
{
    //  If write buffer is empty, try to read new data from the encoder.
    if (!outsize) {

        outpos = NULL;
        encoder.get_data (&outpos, &outsize);

        //  If there is no data to send, stop polling for output.
        if (outsize == 0) {
            reset_pollout (handle);
            return;
        }
    }

    //  If there are any data to write in write buffer, write as much as
    //  possible to the socket. Note that amount of data to write can be
    //  arbitratily large. However, we assume that underlying TCP layer has
    //  limited transmission buffer and thus the actual number of bytes
    //  written should be reasonably modest.
    int nbytes = write (outpos, outsize);

    //  IO error has occurred. We stop waiting for output events.
    //  The engine is not terminated until we detect input error;
    //  this is necessary to prevent losing incomming messages.
    if (nbytes == -1) {
        reset_pollout (handle);
        return;
    }

    outpos += nbytes;
    outsize -= nbytes;

    //  If we are still handshaking and there are no data
    //  to send, stop polling for output.
    if (unlikely (handshaking))
        if (outsize == 0)
            reset_pollout (handle);
}

void zmq::stream_engine_t::activate_out ()
{
    set_pollout (handle);

    //  Speculative write: The assumption is that at the moment new message
    //  was sent by the user the socket is probably available for writing.
    //  Thus we try to write the data to socket avoiding polling for POLLOUT.
    //  Consequently, the latency should be better in request/reply scenarios.
    out_event ();
}

void zmq::stream_engine_t::activate_in ()
{
    if (input_error) {
        //  There was an input error but the engine could not
        //  be terminated (due to the stalled decoder).
        //  Flush the pending message and terminate the engine now.
        decoder.process_buffer (inpos, 0);
        zmq_assert (!decoder.stalled ());
        session->flush ();
        error ();
        return;
    }

    set_pollin (handle);

    //  Speculative read.
    in_event ();
}

int zmq::stream_engine_t::receive_greeting ()
{
    zmq_assert (greeting_bytes_read < greeting_size);

    while (greeting_bytes_read < greeting_size) {
        const int n = read (greeting + greeting_bytes_read,
                            greeting_size - greeting_bytes_read);
        if (n == -1)
            return -1;
        if (n == 0)
            return 0;

        greeting_bytes_read += n;

        if (greeting_bytes_read < greeting_size)
            continue;

        if (greeting_size == 2) {
            //  We have received the first two bytes from the peer.
            //  If the first byte is not 0xff, we know that the
            //  peer is using unversioned protocol.
            if (greeting [0] != 0xff)
                break;

            //  This may still be a long identity message (either
            //  254 or 255 bytes long). We need to receive 8 more
            //  bytes so we can inspect the potential 'flags' field.
            greeting_size = 10;
        }
        else
        if (greeting_size == 10) {
            //  Inspect the rightmost bit of the 10th byte (which coincides
            //  with the 'flags' field if a regular message was sent).
            //  Zero indicates this is a header of identity message
            //  (i.e. the peer is using the unversioned protocol).
            if (!(greeting [9] & 0x01))
                break;

            //  This is truly a handshake and we can now send the rest of
            //  the greeting message out.

            if (outsize == 0)
                set_pollout (handle);

            zmq_assert (outpos != NULL);

            outpos [outsize++] = 1; // Protocol version
            outpos [outsize++] = 1; // Remaining length (1 byte for v1)
            outpos [outsize++] = options.type;  // Socket type

            //  Read the 'version' and 'remaining_length' fields.
            greeting_size = 12;
        }
        else
        if (greeting_size == 12) {
            //  We have received the greeting message up to
            //  the 'remaining_length' field. Receive the remaining
            //  bytes of the greeting.
            greeting_size += greeting [11];
        }
    }

    return 0;
}

bool zmq::stream_engine_t::handshake ()
{
    zmq_assert (handshaking);
    zmq_assert (greeting_bytes_read < greeting_size);

    int rc = receive_greeting ();
    if (rc == -1) {
        error ();
        return false;
    }

    if (greeting_bytes_read < greeting_size)
        return false;

    //  We have received either a header of identity message
    //  or the whole greeting.

    encoder.set_msg_source (session);
    decoder.set_msg_sink (session);

    zmq_assert (greeting [0] != 0xff || greeting_bytes_read >= 10);

    //  Is the peer using the unversioned protocol?
    //  If so, we send and receive rests of identity
    //  messages.
    if (greeting [0] != 0xff || !(greeting [9] & 0x01)) {
        //  We have already sent the message header.
        //  Since there is no way to tell the encoder to
        //  skip the message header, we simply throw that
        //  header data away.
        const size_t header_size = options.identity_size + 1 >= 255 ? 10 : 2;
        unsigned char tmp [10], *bufferp = tmp;
        size_t buffer_size = header_size;
        encoder.get_data (&bufferp, &buffer_size);
        zmq_assert (buffer_size == header_size);

        //  Make sure the decoder sees the data we have already received.
        inpos = greeting;
        insize = greeting_bytes_read;

        //  To allow for interoperability with peers that do not forward
        //  their subscriptions, we inject a phony subsription
        //  message into the incomming message stream. To put this
        //  message right after the identity message, we temporarily
        //  divert the message stream from session to ourselves.
        if (options.type == ZMQ_PUB || options.type == ZMQ_XPUB)
            decoder.set_msg_sink (this);
    }

    // Start polling for output if necessary.
    if (outsize == 0)
        set_pollout (handle);

    //  Handshaking was successful.
    //  Switch into the normal message flow.
    handshaking = false;

    return true;
}

int zmq::stream_engine_t::push_msg (msg_t *msg_)
{
    zmq_assert (options.type == ZMQ_PUB || options.type == ZMQ_XPUB);

    //  The first message is identity.
    //  Let the session process it.
    int rc = session->push_msg (msg_);
    errno_assert (rc == 0);

    //  Inject the subscription message so that the ZMQ 2.x peer
    //  receives our messages.
    rc = msg_->init_size (1);
    errno_assert (rc == 0);
    *(unsigned char*) msg_->data () = 1;
    rc = session->push_msg (msg_);
    session->flush ();

    //  Once we have injected the subscription message, we can
    //  Divert the message flow back to the session.
    decoder.set_msg_sink (session);

    return rc;
}

void zmq::stream_engine_t::error ()
{
    zmq_assert (session);
    session->monitor_event (ZMQ_EVENT_DISCONNECTED, endpoint.c_str(), s);
    session->detach ();
    unplug ();
    delete this;
}

int zmq::stream_engine_t::write (const void *data_, size_t size_)
{
#ifdef ZMQ_HAVE_WINDOWS

    int nbytes = send (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be written to the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative write).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;
		
    //  Signalise peer failure.
    if (nbytes == SOCKET_ERROR && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAEHOSTUNREACH ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);
    return nbytes;

#else

    ssize_t nbytes = send (s, data_, size_, 0);

    //  Several errors are OK. When speculative write is being done we may not
    //  be able to write a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1 && (errno == ECONNRESET || errno == EPIPE ||
          errno == ETIMEDOUT))
        return -1;

    errno_assert (nbytes != -1);
    return (size_t) nbytes;

#endif
}

int zmq::stream_engine_t::read (void *data_, size_t size_)
{
#ifdef ZMQ_HAVE_WINDOWS

    int nbytes = recv (s, (char*) data_, (int) size_, 0);

    //  If not a single byte can be read from the socket in non-blocking mode
    //  we'll get an error (this may happen during the speculative read).
    if (nbytes == SOCKET_ERROR && WSAGetLastError () == WSAEWOULDBLOCK)
        return 0;

    //  Connection failure.
    if (nbytes == SOCKET_ERROR && (
          WSAGetLastError () == WSAENETDOWN ||
          WSAGetLastError () == WSAENETRESET ||
          WSAGetLastError () == WSAECONNABORTED ||
          WSAGetLastError () == WSAETIMEDOUT ||
          WSAGetLastError () == WSAECONNRESET ||
          WSAGetLastError () == WSAECONNREFUSED ||
          WSAGetLastError () == WSAENOTCONN))
        return -1;

    wsa_assert (nbytes != SOCKET_ERROR);

    //  Orderly shutdown by the other peer.
    if (nbytes == 0)
        return -1; 

    return nbytes;

#else

    ssize_t nbytes = recv (s, data_, size_, 0);

    //  Several errors are OK. When speculative read is being done we may not
    //  be able to read a single byte from the socket. Also, SIGSTOP issued
    //  by a debugging tool can result in EINTR error.
    if (nbytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK ||
          errno == EINTR))
        return 0;

    //  Signalise peer failure.
    if (nbytes == -1 && (errno == ECONNRESET || errno == ECONNREFUSED ||
          errno == ETIMEDOUT || errno == EHOSTUNREACH || errno == ENOTCONN))
        return -1;

    errno_assert (nbytes != -1);

    //  Orderly shutdown by the peer.
    if (nbytes == 0)
        return -1;

    return (size_t) nbytes;

#endif
}

