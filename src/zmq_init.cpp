/*
    Copyright (c) 2007-2011 iMatix Corporation
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

#include <string.h>

#include "zmq_init.hpp"
#include "transient_session.hpp"
#include "named_session.hpp"
#include "socket_base.hpp"
#include "zmq_engine.hpp"
#include "io_thread.hpp"
#include "session.hpp"
#include "uuid.hpp"
#include "blob.hpp"
#include "wire.hpp"
#include "err.hpp"

zmq::zmq_init_t::zmq_init_t (io_thread_t *io_thread_,
      socket_base_t *socket_, session_t *session_, fd_t fd_,
      const options_t &options_) :
    own_t (io_thread_, options_),
    ephemeral_engine (NULL),
    received (false),
    socket (socket_),
    session (session_),
    io_thread (io_thread_)
{
    //  Create the engine object for this connection.
    engine = new (std::nothrow) zmq_engine_t (fd_, options);
    alloc_assert (engine);

    //  Generate an unique identity.
    peer_identity.resize (17);
    peer_identity [0] = 0;
    generate_uuid (&peer_identity [1]);

    //  Create a list of props to send.
    msg_t msg;
    int rc = msg.init_size (4);
    errno_assert (rc == 0);
    unsigned char *data = (unsigned char*) msg.data ();
    put_uint16 (data, prop_type);
    put_uint16 (data + 2, options.type);
    msg.set_flags (msg_t::more);
    to_send.push_back (msg);

    if (!options.identity.empty ()) {
        rc = msg.init_size (2 + options.identity.size ());
        errno_assert (rc == 0);
        data = (unsigned char*) msg.data ();
        put_uint16 (data, prop_identity);
        memcpy (data + 2, options.identity.data (), options.identity.size ());
        msg.set_flags (msg_t::more);
        to_send.push_back (msg);
    }

    //  Remove the MORE flag from the last prop.
    to_send.back ().reset_flags (msg_t::more);
}

zmq::zmq_init_t::~zmq_init_t ()
{
    if (engine)
        engine->terminate ();

    //  If there are unsent props still queued deallocate them.
    for (to_send_t::iterator it = to_send.begin (); it != to_send.end ();
          ++it) {
        int rc = it->close ();
        errno_assert (rc == 0);
    }
    to_send.clear ();
}

bool zmq::zmq_init_t::read (msg_t *msg_)
{
    //  If the identity was already sent, do nothing.
    if (to_send.empty ())
        return false;

    //  Pass next property to the engine.
    *msg_ = to_send.front ();
    to_send.erase (to_send.begin ());

    //  Try finalize initialization.
    finalise_initialisation ();

    return true;
}

bool zmq::zmq_init_t::write (msg_t *msg_)
{
    //  If identity was already received, we are not interested
    //  in subsequent messages.
    if (received)
        return false;

    size_t size = msg_->size ();
    unsigned char *data = (unsigned char*) msg_->data ();

    //  There should be at least property type in the message.
    zmq_assert (size >= 2);
    uint16_t prop = get_uint16 (data);

    switch (prop) {
    case prop_type:
        {
            zmq_assert (size == 4);
            //  TODO: Check whether the type is OK.
            //  uint16_t type = get_uint16 (data + 2);
            //  ...
            break;
        };
    case prop_identity:
        {
             peer_identity.assign (data + 2, size - 2);
             break;
        }
    default:
        zmq_assert (false);
    }

    if (!msg_->check_flags (msg_t::more)) {
        received = true;
        finalise_initialisation ();
    }

    return true;
}

void zmq::zmq_init_t::flush ()
{
    //  Check if there's anything to flush.
    if (!received)
        return;

    //  Initialization is done, dispatch engine.
    if (ephemeral_engine)
        dispatch_engine ();
}

void zmq::zmq_init_t::detach ()
{
    //  This function is called by engine when disconnection occurs.

    //  If there is an associated session, send it a null engine to let it know
    //  that connection process was unsuccesful.
    if (session)
        send_attach (session, NULL, blob_t (), true);

    //  The engine will destroy itself, so let's just drop the pointer here and
    //  start termination of the init object.
    engine = NULL;
    terminate ();
}

void zmq::zmq_init_t::process_plug ()
{
    zmq_assert (engine);
    engine->plug (io_thread, this);
}

void zmq::zmq_init_t::process_unplug ()
{
    if (engine)
        engine->unplug ();
}

void zmq::zmq_init_t::finalise_initialisation ()
{
     //  Unplug and prepare to dispatch engine.
     if (to_send.empty () && received) {
        ephemeral_engine = engine;
        engine = NULL;
        ephemeral_engine->unplug ();
        return;
    }
}

void zmq::zmq_init_t::dispatch_engine ()
{
    if (to_send.empty () && received) {

        //  Engine must be detached.
        zmq_assert (!engine);
        zmq_assert (ephemeral_engine);

        //  If we know what session we belong to, it's easy, just send the
        //  engine to that session and destroy the init object. Note that we
        //  know about the session only if this object is owned by it. Thus,
        //  lifetime of this object in contained in the lifetime of the session
        //  so the pointer cannot become invalid without notice.
        if (session) {
            send_attach (session, ephemeral_engine, peer_identity, true);
            terminate ();
            return;
        }

        //  All the cases below are listener-based. Therefore we need the socket
        //  reference so that new sessions can bind to that socket.
        zmq_assert (socket);

        //  We have no associated session. If the peer has no identity we'll
        //  create a transient session for the connection. Note that
        //  seqnum is incremented to account for attach command before the
        //  session is launched. That way we are sure it won't terminate before
        //  being attached.
        if (peer_identity [0] == 0) {
            session = new (std::nothrow) transient_session_t (io_thread,
                socket, options);
            alloc_assert (session);
            session->inc_seqnum ();
            launch_sibling (session);
            send_attach (session, ephemeral_engine, peer_identity, false);
            terminate ();
            return;
        }
        
        //  Try to find the session corresponding to the peer's identity.
        //  If found, send the engine to that session and destroy this object.
        //  Note that session's seqnum is incremented by find_session rather
        //  than by send_attach.
        session = socket->find_session (peer_identity);
        if (session) {
            send_attach (session, ephemeral_engine, peer_identity, false);
            terminate ();
            return;
        }

        //  There's no such named session. We have to create one. Note that
        //  seqnum is incremented to account for attach command before the
        //  session is launched. That way we are sure it won't terminate before
        //  being attached.
        session = new (std::nothrow) named_session_t (io_thread, socket,
            options, peer_identity);
        alloc_assert (session);
        session->inc_seqnum ();
        launch_sibling (session);
        send_attach (session, ephemeral_engine, peer_identity, false);
        terminate ();
        return;
    }
}
