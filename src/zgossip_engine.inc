/*  =========================================================================
    zgossip_engine - zgossip engine

    ** WARNING *************************************************************
    THIS SOURCE FILE IS 100% GENERATED. If you edit this file, you will lose
    your changes at the next build cycle. This is great for temporary printf
    statements. DO NOT MAKE ANY CHANGES YOU WISH TO KEEP. The correct places
    for commits are:

     * The XML model used for this code generation: zgossip.xml, or
     * The code generation script that built this file: zproto_server_c
    ************************************************************************
    Copyright (c) the Contributors as noted in the AUTHORS file.       
    This file is part of CZMQ, the high-level C binding for 0MQ:       
    http://czmq.zeromq.org.                                            
                                                                       
    This Source Code Form is subject to the terms of the Mozilla Public
    License, v. 2.0. If a copy of the MPL was not distributed with this
    file, You can obtain one at http://mozilla.org/MPL/2.0/.           
    =========================================================================
*/


//  ---------------------------------------------------------------------------
//  State machine constants

typedef enum {
    start_state = 1,
    have_tuple_state = 2,
    connected_state = 3,
    external_state = 4
} state_t;

typedef enum {
    NULL_event = 0,
    terminate_event = 1,
    hello_event = 2,
    ok_event = 3,
    finished_event = 4,
    publish_event = 5,
    forward_event = 6,
    ping_event = 7,
    expired_event = 8
} event_t;

//  Names for state machine logging and error reporting
static char *
s_state_name [] = {
    "(NONE)",
    "start",
    "have tuple",
    "connected",
    "external"
};

static char *
s_event_name [] = {
    "(NONE)",
    "terminate",
    "HELLO",
    "ok",
    "finished",
    "PUBLISH",
    "forward",
    "PING",
    "expired"
};

//  ---------------------------------------------------------------------------
//  Context for the whole server task. This embeds the application-level
//  server context at its start (the entire structure, not a reference),
//  so we can cast a pointer between server_t and s_server_t arbitrarily.

typedef struct {
    server_t server;            //  Application-level server context
    zsock_t *pipe;              //  Socket to back to caller API
    zsock_t *router;            //  Socket to talk to clients
    int port;                   //  Server port bound to
    zloop_t *loop;              //  Reactor for server sockets
    zgossip_msg_t *message;     //  Message received or sent
    zhash_t *clients;           //  Clients we're connected to
    zconfig_t *config;          //  Configuration tree
    uint client_id;             //  Client identifier counter
    size_t timeout;             //  Default client expiry timeout
    bool verbose;               //  Verbose logging enabled?
    char *log_prefix;           //  Default log prefix
} s_server_t;


//  ---------------------------------------------------------------------------
//  Context for each connected client. This embeds the application-level
//  client context at its start (the entire structure, not a reference),
//  so we can cast a pointer between client_t and s_client_t arbitrarily.

typedef struct {
    client_t client;            //  Application-level client context
    s_server_t *server;         //  Parent server context
    char *hashkey;              //  Key into server->clients hash
    zframe_t *routing_id;       //  Routing_id back to client
    uint unique_id;             //  Client identifier in server
    state_t state;              //  Current state
    event_t event;              //  Current event
    event_t next_event;         //  The next event
    event_t exception;          //  Exception event, if any
    int wakeup;                 //  zloop timer for client alarms
    void *ticket;               //  zloop ticket for client timeouts
    event_t wakeup_event;       //  Wake up with this event
    char log_prefix [41];       //  Log prefix string
} s_client_t;

static int
    server_initialize (server_t *self);
static void
    server_terminate (server_t *self);
static zmsg_t *
    server_method (server_t *self, const char *method, zmsg_t *msg);
static int
    client_initialize (client_t *self);
static void
    client_terminate (client_t *self);
static void
    s_client_execute (s_client_t *client, event_t event);
static int
    s_client_handle_wakeup (zloop_t *loop, int timer_id, void *argument);
static int
    s_client_handle_ticket (zloop_t *loop, int timer_id, void *argument);
static void
    get_first_tuple (client_t *self);
static void
    get_next_tuple (client_t *self);
static void
    store_tuple_if_new (client_t *self);
static void
    get_tuple_to_forward (client_t *self);

//  ---------------------------------------------------------------------------
//  These methods are an internal API for actions

//  Set the next event, needed in at least one action in an internal
//  state; otherwise the state machine will wait for a message on the
//  router socket and treat that as the event.

static void
engine_set_next_event (client_t *client, event_t event)
{
    if (client) {
        s_client_t *self = (s_client_t *) client;
        self->next_event = event;
    }
}

//  Raise an exception with 'event', halting any actions in progress.
//  Continues execution of actions defined for the exception event.

static void
engine_set_exception (client_t *client, event_t event)
{
    if (client) {
        s_client_t *self = (s_client_t *) client;
        self->exception = event;
    }
}

//  Set wakeup alarm after 'delay' msecs. The next state should
//  handle the wakeup event. The alarm is cancelled on any other
//  event.

static void
engine_set_wakeup_event (client_t *client, size_t delay, event_t event)
{
    if (client) {
        s_client_t *self = (s_client_t *) client;
        if (self->wakeup) {
            zloop_timer_end (self->server->loop, self->wakeup);
            self->wakeup = 0;
        }
        self->wakeup = zloop_timer (
            self->server->loop, delay, 1, s_client_handle_wakeup, self);
        self->wakeup_event = event;
    }
}

//  Execute 'event' on specified client. Use this to send events to
//  other clients. Cancels any wakeup alarm on that client.

static void
engine_send_event (client_t *client, event_t event)
{
    if (client) {
        s_client_t *self = (s_client_t *) client;
        s_client_execute (self, event);
    }
}

//  Execute 'event' on all clients known to the server. If you pass a
//  client argument, that client will not receive the broadcast. If you
//  want to pass any arguments, store them in the server context.

static void
engine_broadcast_event (server_t *server, client_t *client, event_t event)
{
    if (server) {
        s_server_t *self = (s_server_t *) server;
        zlist_t *keys = zhash_keys (self->clients);
        char *key = (char *) zlist_first (keys);
        while (key) {
            s_client_t *target = (s_client_t *) zhash_lookup (self->clients, key);
            if (target != (s_client_t *) client)
                s_client_execute (target, event);
            key = (char *) zlist_next (keys);
        }
        zlist_destroy (&keys);
    }
}

//  Poll actor or zsock for activity, invoke handler on any received
//  message. Handler must be a CZMQ zloop_fn function; receives server
//  as arg.

static void
engine_handle_socket (server_t *server, void *sock, zloop_reader_fn handler)
{
    if (server) {
        s_server_t *self = (s_server_t *) server;
        //  Resolve zactor_t -> zsock_t
        if (zactor_is (sock))
            sock = zactor_sock ((zactor_t *) sock);
        else
            assert (zsock_is (sock));
        if (handler != NULL) {
            int rc = zloop_reader (self->loop, (zsock_t *) sock, handler, self);
            assert (rc == 0);
            zloop_reader_set_tolerant (self->loop, (zsock_t *) sock);
        }
        else
            zloop_reader_end (self->loop, (zsock_t *) sock);
    }
}

//  Register monitor function that will be called at regular intervals
//  by the server engine

static void
engine_set_monitor (server_t *server, size_t interval, zloop_timer_fn monitor)
{
    if (server) {
        s_server_t *self = (s_server_t *) server;
        int rc = zloop_timer (self->loop, interval, 0, monitor, self);
        assert (rc >= 0);
    }
}

//  Set log file prefix; this string will be added to log data, to make
//  log data more searchable. The string is truncated to ~20 chars.

static void
engine_set_log_prefix (client_t *client, const char *string)
{
    if (client) {
        s_client_t *self = (s_client_t *) client;
        snprintf (self->log_prefix, sizeof (self->log_prefix) - 1,
            "%6d:%-33s", self->unique_id, string);
    }
}

//  Set a configuration value in the server's configuration tree. The
//  properties this engine uses are: server/verbose, server/timeout, and
//  server/background. You can also configure other abitrary properties.

static void
engine_configure (server_t *server, const char *path, const char *value)
{
    if (server) {
        s_server_t *self = (s_server_t *) server;
        zconfig_put (self->config, path, value);
    }
}

//  Return true if server is running in verbose mode, else return false.

static bool
engine_verbose (server_t *server)
{
    if (server) {
        s_server_t *self = (s_server_t *) server;
        return self->verbose;
    }
    return false;
}

//  Pedantic compilers don't like unused functions, so we call the whole
//  API, passing null references. It's nasty and horrid and sufficient.

static void
s_satisfy_pedantic_compilers (void)
{
    engine_set_next_event (NULL, NULL_event);
    engine_set_exception (NULL, NULL_event);
    engine_set_wakeup_event (NULL, 0, NULL_event);
    engine_send_event (NULL, NULL_event);
    engine_broadcast_event (NULL, NULL, NULL_event);
    engine_handle_socket (NULL, 0, NULL);
    engine_set_monitor (NULL, 0, NULL);
    engine_set_log_prefix (NULL, NULL);
    engine_configure (NULL, NULL, NULL);
    engine_verbose (NULL);
}


//  ---------------------------------------------------------------------------
//  Generic methods on protocol messages
//  TODO: replace with lookup table, since ID is one byte

static event_t
s_protocol_event (zgossip_msg_t *message)
{
    assert (message);
    switch (zgossip_msg_id (message)) {
        case ZGOSSIP_MSG_HELLO:
            return hello_event;
            break;
        case ZGOSSIP_MSG_PUBLISH:
            return publish_event;
            break;
        case ZGOSSIP_MSG_PING:
            return ping_event;
            break;
        default:
            //  Invalid zgossip_msg_t
            return terminate_event;
    }
}


//  ---------------------------------------------------------------------------
//  Client methods

static s_client_t *
s_client_new (s_server_t *server, zframe_t *routing_id)
{
    s_client_t *self = (s_client_t *) zmalloc (sizeof (s_client_t));
    assert (self);
    assert ((s_client_t *) &self->client == self);

    self->server = server;
    self->hashkey = zframe_strhex (routing_id);
    self->routing_id = zframe_dup (routing_id);
    self->unique_id = server->client_id++;
    engine_set_log_prefix (&self->client, server->log_prefix);

    self->client.server = (server_t *) server;
    self->client.message = server->message;

    //  If expiry timers are being used, create client ticket
    if (server->timeout)
        self->ticket = zloop_ticket (server->loop, s_client_handle_ticket, self);
    //  Give application chance to initialize and set next event
    self->state = start_state;
    self->event = NULL_event;
    client_initialize (&self->client);
    return self;
}

static void
s_client_destroy (s_client_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        s_client_t *self = *self_p;
        if (self->wakeup)
            zloop_timer_end (self->server->loop, self->wakeup);
        if (self->ticket)
            zloop_ticket_delete (self->server->loop, self->ticket);
        zframe_destroy (&self->routing_id);
        //  Provide visual clue if application misuses client reference
        engine_set_log_prefix (&self->client, "*** TERMINATED ***");
        client_terminate (&self->client);
        free (self->hashkey);
        free (self);
        *self_p = NULL;
    }
}

//  Callback when we remove client from 'clients' hash table
static void
s_client_free (void *argument)
{
    s_client_t *client = (s_client_t *) argument;
    s_client_destroy (&client);
}


//  Execute state machine as long as we have events

static void
s_client_execute (s_client_t *self, event_t event)
{
    self->next_event = event;
    //  Cancel wakeup timer, if any was pending
    if (self->wakeup) {
        zloop_timer_end (self->server->loop, self->wakeup);
        self->wakeup = 0;
    }
    while (self->next_event > 0) {
        self->event = self->next_event;
        self->next_event = NULL_event;
        self->exception = NULL_event;
        if (self->server->verbose) {
            zsys_debug ("%s: %s:",
                self->log_prefix, s_state_name [self->state]);
            zsys_debug ("%s:     %s",
                self->log_prefix, s_event_name [self->event]);
        }
        switch (self->state) {
            case start_state:
                if (self->event == hello_event) {
                    if (!self->exception) {
                        //  get first tuple
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ get first tuple", self->log_prefix);
                        get_first_tuple (&self->client);
                    }
                    if (!self->exception)
                        self->state = have_tuple_state;
                }
                else
                if (self->event == ping_event) {
                    if (!self->exception) {
                        //  send PONG
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send PONG",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_PONG);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                }
                else
                if (self->event == expired_event) {
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                else {
                    //  Handle unexpected protocol events
                    if (!self->exception) {
                        //  send INVALID
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send INVALID",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_INVALID);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                break;

            case have_tuple_state:
                if (self->event == ok_event) {
                    if (!self->exception) {
                        //  send PUBLISH
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send PUBLISH",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_PUBLISH);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                    if (!self->exception) {
                        //  get next tuple
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ get next tuple", self->log_prefix);
                        get_next_tuple (&self->client);
                    }
                }
                else
                if (self->event == finished_event) {
                    if (!self->exception)
                        self->state = connected_state;
                }
                else {
                    //  Handle unexpected internal events
                    zsys_warning ("%s: unhandled event %s in %s",
                        self->log_prefix,
                        s_event_name [self->event],
                        s_state_name [self->state]);
                    assert (false);
                }
                break;

            case connected_state:
                if (self->event == publish_event) {
                    if (!self->exception) {
                        //  store tuple if new
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ store tuple if new", self->log_prefix);
                        store_tuple_if_new (&self->client);
                    }
                }
                else
                if (self->event == forward_event) {
                    if (!self->exception) {
                        //  get tuple to forward
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ get tuple to forward", self->log_prefix);
                        get_tuple_to_forward (&self->client);
                    }
                    if (!self->exception) {
                        //  send PUBLISH
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send PUBLISH",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_PUBLISH);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                }
                else
                if (self->event == ping_event) {
                    if (!self->exception) {
                        //  send PONG
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send PONG",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_PONG);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                }
                else
                if (self->event == expired_event) {
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                else {
                    //  Handle unexpected protocol events
                    if (!self->exception) {
                        //  send INVALID
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send INVALID",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_INVALID);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                break;

            case external_state:
                if (self->event == ping_event) {
                    if (!self->exception) {
                        //  send PONG
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send PONG",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_PONG);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                }
                else
                if (self->event == expired_event) {
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                else {
                    //  Handle unexpected protocol events
                    if (!self->exception) {
                        //  send INVALID
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ send INVALID",
                                self->log_prefix);
                        zgossip_msg_set_id (self->server->message, ZGOSSIP_MSG_INVALID);
                        zgossip_msg_set_routing_id (self->server->message, self->routing_id);
                        zgossip_msg_send (self->server->message, self->server->router);
                    }
                    if (!self->exception) {
                        //  terminate
                        if (self->server->verbose)
                            zsys_debug ("%s:         $ terminate", self->log_prefix);
                        self->next_event = terminate_event;
                    }
                }
                break;
        }
        //  If we had an exception event, interrupt normal programming
        if (self->exception) {
            if (self->server->verbose)
                zsys_debug ("%s:         ! %s",
                    self->log_prefix, s_event_name [self->exception]);

            self->next_event = self->exception;
        }
        if (self->next_event == terminate_event) {
            //  Automatically calls s_client_destroy
            zhash_delete (self->server->clients, self->hashkey);
            break;
        }
        else
        if (self->server->verbose)
            zsys_debug ("%s:         > %s",
                self->log_prefix, s_state_name [self->state]);
    }
}

//  zloop callback when client ticket expires

static int
s_client_handle_ticket (zloop_t *loop, int timer_id, void *argument)
{
    s_client_t *self = (s_client_t *) argument;
    self->ticket = NULL;        //  Ticket is now dead
    s_client_execute (self, expired_event);
    return 0;
}

//  zloop callback when client wakeup timer expires

static int
s_client_handle_wakeup (zloop_t *loop, int timer_id, void *argument)
{
    s_client_t *self = (s_client_t *) argument;
    s_client_execute (self, self->wakeup_event);
    return 0;
}


//  Server methods

static void
s_server_config_global (s_server_t *self)
{
    //  Built-in server configuration options
    //
    //  If we didn't already set verbose, check if the config tree wants it
    if (!self->verbose
    && atoi (zconfig_resolve (self->config, "server/verbose", "0")))
        self->verbose = true;

    //  Default client timeout is 60 seconds
    self->timeout = atoi (
        zconfig_resolve (self->config, "server/timeout", "60000"));
    zloop_set_ticket_delay (self->loop, self->timeout);

    //  Do we want to run server in the background?
    int background = atoi (
        zconfig_resolve (self->config, "server/background", "0"));
    if (!background)
        zsys_set_logstream (stdout);
}

static s_server_t *
s_server_new (zsock_t *pipe)
{
    s_server_t *self = (s_server_t *) zmalloc (sizeof (s_server_t));
    assert (self);
    assert ((s_server_t *) &self->server == self);

    self->pipe = pipe;
    self->router = zsock_new (ZMQ_ROUTER);
    //  By default the socket will discard outgoing messages above the
    //  HWM of 1,000. This isn't helpful for high-volume streaming. We
    //  will use a unbounded queue here. If applications need to guard
    //  against queue overflow, they should use a credit-based flow
    //  control scheme.
    zsock_set_unbounded (self->router);
    self->message = zgossip_msg_new ();
    self->clients = zhash_new ();
    self->config = zconfig_new ("root", NULL);
    self->loop = zloop_new ();
    srandom ((unsigned int) zclock_time ());
    self->client_id = randof (1000);
    s_server_config_global (self);

    //  Initialize application server context
    self->server.pipe = self->pipe;
    self->server.config = self->config;
    server_initialize (&self->server);

    s_satisfy_pedantic_compilers ();
    return self;
}

static void
s_server_destroy (s_server_t **self_p)
{
    assert (self_p);
    if (*self_p) {
        s_server_t *self = *self_p;
        zgossip_msg_destroy (&self->message);
        //  Destroy clients before destroying the server
        zhash_destroy (&self->clients);
        server_terminate (&self->server);
        zsock_destroy (&self->router);
        zconfig_destroy (&self->config);
        zloop_destroy (&self->loop);
        free (self);
        *self_p = NULL;
    }
}

//  Apply service-specific configuration tree:
//   * apply server configuration
//   * print any echo items in top-level sections
//   * apply sections that match methods

static void
s_server_config_service (s_server_t *self)
{
    //  Apply echo commands and class methods
    zconfig_t *section = zconfig_locate (self->config, "zgossip");
    if (section)
        section = zconfig_child (section);

    while (section) {
        if (streq (zconfig_name (section), "echo"))
            zsys_notice ("%s", zconfig_value (section));
        else
        if (streq (zconfig_name (section), "bind")) {
            char *endpoint = zconfig_resolve (section, "endpoint", "?");
            if (zsock_bind (self->router, "%s", endpoint) == -1)
                zsys_warning ("could not bind to %s (%s)", endpoint, zmq_strerror (zmq_errno ()));
        }
#if (ZMQ_VERSION_MAJOR >= 4)
        else
        if (streq (zconfig_name (section), "security")) {
            char *mechanism = zconfig_resolve (section, "mechanism", "null");
            char *domain = zconfig_resolve (section, "domain", NULL);
            if (streq (mechanism, "null")) {
                zsys_notice ("server is using NULL security");
                if (domain)
                    zsock_set_zap_domain (self->router, NULL);
            }
            else
            if (streq (mechanism, "plain")) {
                zsys_notice ("server is using PLAIN security");
                zsock_set_plain_server (self->router, 1);
            }
            else
                zsys_warning ("mechanism=%s is not supported", mechanism);
        }
#endif
        section = zconfig_next (section);
    }
    s_server_config_global (self);
}

//  Process message from pipe

static int
s_server_handle_pipe (zloop_t *loop, zsock_t *reader, void *argument)
{
    s_server_t *self = (s_server_t *) argument;
    zmsg_t *msg = zmsg_recv (self->pipe);
    if (!msg)
        return -1;              //  Interrupted; exit zloop
    char *method = zmsg_popstr (msg);
    if (self->verbose)
        zsys_debug ("%s:     API command=%s", self->log_prefix, method);

    if (streq (method, "VERBOSE"))
        self->verbose = true;
    else
    if (streq (method, "$TERM")) {
        //  Shutdown the engine
        free (method);
        zmsg_destroy (&msg);
        return -1;
    }
    else
    if (streq (method, "BIND")) {
        //  Bind to a specified endpoint, which may use an ephemeral port
        char *endpoint = zmsg_popstr (msg);
        self->port = zsock_bind (self->router, "%s", endpoint);
        if (self->port == -1)
            zsys_warning ("could not bind to %s", endpoint);
        free (endpoint);
    }
    else
    if (streq (method, "PORT")) {
        //  Return PORT + port number from the last bind, if any
        zstr_sendm (self->pipe, "PORT");
        zstr_sendf (self->pipe, "%d", self->port);
    }
    else                       //  Deprecated method name
    if (streq (method, "LOAD") || streq (method, "CONFIGURE")) {
        char *filename = zmsg_popstr (msg);
        zconfig_destroy (&self->config);
        self->config = zconfig_load (filename);
        if (self->config) {
            s_server_config_service (self);
            self->server.config = self->config;
        }
        else {
            zsys_warning ("cannot load config file '%s'", filename);
            self->config = zconfig_new ("root", NULL);
        }
        free (filename);
    }
    else
    if (streq (method, "SET")) {
        char *path = zmsg_popstr (msg);
        char *value = zmsg_popstr (msg);
        zconfig_put (self->config, path, value);
        if (streq (path, "server/animate")) {
            zsys_warning ("'%s' is deprecated, use VERBOSE command instead", path);
            self->verbose = atoi (value);
        }
        s_server_config_global (self);
        free (path);
        free (value);
    }
    else
    if (streq (method, "SAVE")) {
        char *filename = zmsg_popstr (msg);
        if (zconfig_save (self->config, filename))
            zsys_warning ("cannot save config file '%s'", filename);
        free (filename);
    }
    else {
        //  Execute custom method
        zmsg_t *reply = server_method (&self->server, method, msg);
        //  If reply isn't null, send it to caller
        zmsg_send (&reply, self->pipe);
    }
    free (method);
    zmsg_destroy (&msg);
    return 0;
}

//  Handle a protocol message from the client

static int
s_server_handle_protocol (zloop_t *loop, zsock_t *reader, void *argument)
{
    s_server_t *self = (s_server_t *) argument;
    //  We process as many messages as we can, to reduce the overhead
    //  of polling and the reactor:
    while (zsock_events (self->router) & ZMQ_POLLIN) {
        if (zgossip_msg_recv (self->message, self->router))
            return -1;              //  Interrupted; exit zloop

        //  TODO: use binary hashing on routing_id
        char *hashkey = zframe_strhex (zgossip_msg_routing_id (self->message));
        s_client_t *client = (s_client_t *) zhash_lookup (self->clients, hashkey);
        if (client == NULL) {
            client = s_client_new (self, zgossip_msg_routing_id (self->message));
            zhash_insert (self->clients, hashkey, client);
            zhash_freefn (self->clients, hashkey, s_client_free);
        }
        free (hashkey);
        //  Any input from client counts as activity
        if (client->ticket)
            zloop_ticket_reset (self->loop, client->ticket);

        //  Pass to client state machine
        s_client_execute (client, s_protocol_event (self->message));
    }
    return 0;
}

//  Watch server config file and reload if changed

static int
s_watch_server_config (zloop_t *loop, int timer_id, void *argument)
{
    s_server_t *self = (s_server_t *) argument;
    if (zconfig_has_changed (self->config)
    &&  zconfig_reload (&self->config) == 0) {
        s_server_config_service (self);
        self->server.config = self->config;
        zsys_notice ("reloaded configuration from %s",
            zconfig_filename (self->config));
    }
    return 0;
}


//  ---------------------------------------------------------------------------
//  This is the server actor, which polls its two sockets and processes
//  incoming messages

void
zgossip (zsock_t *pipe, void *args)
{
    //  Initialize
    s_server_t *self = s_server_new (pipe);
    assert (self);
    zsock_signal (pipe, 0);
    //  Actor argument may be a string used for logging
    self->log_prefix = args? (char *) args: "";

    //  Set-up server monitor to watch for config file changes
    engine_set_monitor ((server_t *) self, 1000, s_watch_server_config);
    //  Set up handler for the two main sockets the server uses
    engine_handle_socket ((server_t *) self, self->pipe, s_server_handle_pipe);
    engine_handle_socket ((server_t *) self, self->router, s_server_handle_protocol);

    //  Run reactor until there's a termination signal
    zloop_start (self->loop);

    //  Reactor has ended
    s_server_destroy (&self);
}
