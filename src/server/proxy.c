/*
Copyright (C) 2024 notscared contributors

Proxy mode server handling.

When sv_proxy_mode is enabled, the server only accepts connections from
the Elixir proxy on localhost. The proxy handles client authentication
and challenge/response, then sends PROXY_* commands to manage clients.

Protocol commands:
  PROXY_CONNECT     - Create a client slot for a new player
  PROXY_DISCONNECT  - Remove a client from their slot
  PROXY_GAMEDATA    - Wrapped game packet to/from client
  PROXY_TRANSFER    - Transfer player to another backend (TODO)
  PROXY_SLOT_ASSIGNED - Response confirming slot allocation
*/

#include "server.h"

static void SV_ProxyConnect(void);
static void SV_ProxyTransfer(void);
static void SV_ProxyDisconnect(int slot);
static void SV_ProxyGameData(int slot);
static void send_slot_assigned(int slot);
static void proxy_netchan_send(void *opaque, const void *data, size_t len, const netadr_t *to);

/*
==================
SV_HandleProxyPacket

Dispatch incoming proxy protocol packets.
Packet format: PROXY_MAGIC (4) | cmd (1) | slot (1) | payload
==================
*/
void SV_HandleProxyPacket(void)
{
    int cmd, slot;

    msg_read.readcount = 4;  // Skip PROXY_MAGIC
    cmd = MSG_ReadByte();
    slot = MSG_ReadByte();

    switch (cmd) {
        case PROXY_CONNECT:
            SV_ProxyConnect();
            break;
        case PROXY_DISCONNECT:
            SV_ProxyDisconnect(slot);
            break;
        case PROXY_GAMEDATA:
            SV_ProxyGameData(slot);
            break;
        case PROXY_TRANSFER:
            SV_ProxyTransfer();
            break;
        default:
            Com_DPrintf("Proxy: unknown command %d\n", cmd);
            break;
    }
}

/*
==================
SV_ProxyConnect

Create a client slot for a proxied player.

Payload: userinfo_len (2) | userinfo | real_ip (str) | real_port (2) | qport (2)
==================
*/
static void SV_ProxyConnect(void)
{
    char userinfo[MAX_INFO_STRING];
    char real_ip[64];
    uint16_t real_port, qport, userinfo_len;
    client_t *newcl;
    int i;
    qboolean allow;

    if (!svs.initialized) {
        Com_DPrintf("Proxy: CONNECT but server not initialized\n");
        return;
    }

    userinfo_len = MSG_ReadShort();
    if (userinfo_len >= sizeof(userinfo)) {
        Com_DPrintf("Proxy: userinfo too long (%d)\n", userinfo_len);
        return;
    }

    byte *data_ptr = MSG_ReadData(userinfo_len);
    if (!data_ptr) {
        Com_DPrintf("Proxy: failed to read userinfo\n");
        return;
    }
    memcpy(userinfo, data_ptr, userinfo_len);
    userinfo[userinfo_len] = '\0';

    MSG_ReadString(real_ip, sizeof(real_ip));
    real_port = MSG_ReadShort();
    qport = MSG_ReadShort();

    // Find free slot
    newcl = NULL;
    for (i = 0; i < svs.maxclients; i++) {
        if (svs.client_pool[i].state == cs_free) {
            newcl = &svs.client_pool[i];
            break;
        }
    }

    if (!newcl) {
        Com_DPrintf("Proxy: no free client slots\n");
        return;
    }

    int number = newcl - svs.client_pool;

    // Initialize client
    memset(newcl, 0, sizeof(*newcl));
    newcl->number = newcl->infonum = number;
    newcl->protocol = PROTOCOL_VERSION_RERELEASE;
    newcl->edict = EDICT_NUM(number + 1);
    newcl->gamedir = fs_game->string;
    newcl->mapname = sv.name;
    newcl->configstrings = sv.configstrings;
    newcl->csr = &svs.csr;
    newcl->ge = ge;
    newcl->cm = &sv.cm;
    newcl->spawncount = sv.spawncount;
    newcl->maxclients = svs.maxclients;
    newcl->state = cs_assigned;  // SV_New_f will advance to cs_connected
    newcl->lastmessage = svs.realtime;

    // Mark as proxy client
    newcl->proxy_client = true;
    NET_StringToAdr(real_ip, &newcl->proxy_real_addr, 0);
    newcl->proxy_real_port = real_port;

    // Set up netchan pointing to proxy, with client's qport
    Netchan_Setup(&newcl->netchan, NS_SERVER, NETCHAN_OLD, &net_from,
                  qport, MAX_PACKETLEN_DEFAULT, newcl->protocol);
    newcl->numpackets = 1;

    // Hook send function to wrap outgoing packets
    newcl->netchan.send_fn = proxy_netchan_send;
    newcl->netchan.send_opaque = newcl;

    newcl->io_data.sz_read = &msg_read;
    newcl->io_data.sz_write = &msg_write;
    newcl->io_data.max_msg_len = newcl->netchan.maxpacketlen;

#if USE_FPS
    newcl->framediv = 1;
    newcl->settings[CLS_FPS] = sv.framerate;
#endif

    // Initialize protocol context
    q2proto_connect_t dummy_connect = {0};
    dummy_connect.protocol = Q2P_PROTOCOL_Q2REPRO;
    dummy_connect.version = PROTOCOL_VERSION_RERELEASE;
    q2proto_init_servercontext(&newcl->q2proto_ctx, &svs.server_info, &dummy_connect);

    Q_strlcpy(newcl->userinfo, userinfo, sizeof(newcl->userinfo));
    SV_UserinfoChanged(newcl);

    // Let game accept/reject
    sv_client = newcl;
    sv_player = newcl->edict;
    allow = ge->ClientConnect(newcl->edict, userinfo, "", false);
    sv_client = NULL;
    sv_player = NULL;

    if (!allow) {
        Com_DPrintf("Proxy: game rejected connection\n");
        memset(newcl, 0, sizeof(*newcl));
        return;
    }

    SV_RateInit(&newcl->ratelimit_namechange, sv_namechange_limit->string);
    SV_InitClientSend(newcl);
    List_Append(&sv_clientlist, &newcl->entry);

    Com_Printf("Proxy client %d connected: %s\n", number, newcl->name);
    send_slot_assigned(number);
}

/*
==================
SV_ProxyTransfer

Accept a player being transferred from another backend.
Same as PROXY_CONNECT but also sends map data immediately.

The client's connection to the proxy stays alive - we just send
them the new server's map data through the existing netchan.
==================
*/
static void SV_ProxyTransfer(void)
{
    char userinfo[MAX_INFO_STRING];
    char real_ip[64];
    uint16_t real_port, qport, userinfo_len;
    client_t *newcl;
    int i;
    qboolean allow;

    if (!svs.initialized) {
        Com_DPrintf("Proxy: TRANSFER but server not initialized\n");
        return;
    }

    userinfo_len = MSG_ReadShort();
    if (userinfo_len >= sizeof(userinfo)) {
        Com_DPrintf("Proxy: userinfo too long (%d)\n", userinfo_len);
        return;
    }

    byte *data_ptr = MSG_ReadData(userinfo_len);
    if (!data_ptr) {
        Com_DPrintf("Proxy: failed to read userinfo\n");
        return;
    }
    memcpy(userinfo, data_ptr, userinfo_len);
    userinfo[userinfo_len] = '\0';

    MSG_ReadString(real_ip, sizeof(real_ip));
    real_port = MSG_ReadShort();
    qport = MSG_ReadShort();
    uint32_t incoming_seq = MSG_ReadLong();  // Netchan sequence from old backend

    // Find free slot
    newcl = NULL;
    for (i = 0; i < svs.maxclients; i++) {
        if (svs.client_pool[i].state == cs_free) {
            newcl = &svs.client_pool[i];
            break;
        }
    }

    if (!newcl) {
        Com_DPrintf("Proxy: no free client slots for transfer\n");
        return;
    }

    int number = newcl - svs.client_pool;

    // Initialize client
    memset(newcl, 0, sizeof(*newcl));
    newcl->number = newcl->infonum = number;
    newcl->protocol = PROTOCOL_VERSION_RERELEASE;
    newcl->edict = EDICT_NUM(number + 1);
    newcl->gamedir = fs_game->string;
    newcl->mapname = sv.name;
    newcl->configstrings = sv.configstrings;
    newcl->csr = &svs.csr;
    newcl->ge = ge;
    newcl->cm = &sv.cm;
    newcl->spawncount = sv.spawncount;
    newcl->maxclients = svs.maxclients;
    newcl->state = cs_connected;  // Skip cs_assigned, go straight to connected
    newcl->lastmessage = svs.realtime;

    // Mark as proxy client
    newcl->proxy_client = true;
    NET_StringToAdr(real_ip, &newcl->proxy_real_addr, 0);
    newcl->proxy_real_port = real_port;

    // Set up netchan pointing to proxy, with client's qport
    Netchan_Setup(&newcl->netchan, NS_SERVER, NETCHAN_OLD, &net_from,
                  qport, MAX_PACKETLEN_DEFAULT, newcl->protocol);
    newcl->numpackets = 1;

    // Use the sequence number from the old backend + buffer so the client
    // accepts our packets. The client's incoming_sequence is from the old
    // backend, so we need to be higher than that.
    newcl->netchan.outgoing_sequence = incoming_seq + 100;

    // Hook send function to wrap outgoing packets
    newcl->netchan.send_fn = proxy_netchan_send;
    newcl->netchan.send_opaque = newcl;

    newcl->io_data.sz_read = &msg_read;
    newcl->io_data.sz_write = &msg_write;
    newcl->io_data.max_msg_len = newcl->netchan.maxpacketlen;

#if USE_FPS
    newcl->framediv = 1;
    newcl->settings[CLS_FPS] = sv.framerate;
#endif

    // Initialize protocol context
    q2proto_connect_t dummy_connect = {0};
    dummy_connect.protocol = Q2P_PROTOCOL_Q2REPRO;
    dummy_connect.version = PROTOCOL_VERSION_RERELEASE;
    q2proto_init_servercontext(&newcl->q2proto_ctx, &svs.server_info, &dummy_connect);

    Q_strlcpy(newcl->userinfo, userinfo, sizeof(newcl->userinfo));
    SV_UserinfoChanged(newcl);

    // Let game accept/reject
    sv_client = newcl;
    sv_player = newcl->edict;
    allow = ge->ClientConnect(newcl->edict, userinfo, "", false);

    if (!allow) {
        sv_client = NULL;
        sv_player = NULL;
        Com_DPrintf("Proxy: game rejected transfer\n");
        memset(newcl, 0, sizeof(*newcl));
        return;
    }

    SV_RateInit(&newcl->ratelimit_namechange, sv_namechange_limit->string);
    SV_InitClientSend(newcl);
    List_Append(&sv_clientlist, &newcl->entry);

    // Set a version string for transferred clients so they pass the version probe check.
    // Without this, SV_Begin_f would drop them with "failed version probe".
    newcl->version_string = SV_CopyString("proxy-transfer");

    Com_Printf("Proxy client %d transferred: %s\n", number, newcl->name);
    send_slot_assigned(number);

    // Directly send serverdata/configstrings to client, bypassing "reconnect"
    // This avoids the client going to ca_challenging state and sending a fresh
    // getchallenge (which would be routed to the wrong backend by the proxy).
    // sv_client/sv_player are already set from above, so SV_New_f will use them.
    SV_New_f();

    sv_client = NULL;
    sv_player = NULL;
}

/*
==================
SV_ProxyDisconnect

Silently remove a proxied client from this backend.
Unlike SV_DropClient, this does NOT send disconnect to the client
because the client is being transferred to another backend.
==================
*/
static void SV_ProxyDisconnect(int slot)
{
    if (slot < 0 || slot >= svs.maxclients)
        return;

    client_t *cl = &svs.client_pool[slot];
    if (cl->state == cs_free || !cl->proxy_client)
        return;

    Com_Printf("Proxy client %d disconnected: %s\n", slot, cl->name);

    // Silent cleanup - don't send anything to client
    clstate_t oldstate = cl->state;
    cl->state = cs_zombie;
    cl->lastmessage = svs.realtime;

    // Let game know the client disconnected
    if (oldstate == cs_spawned || (g_features->integer & GMF_WANT_ALL_DISCONNECTS)) {
        ge->ClientDisconnect(cl->edict);
    }

    // Clean up client resources
    SV_CleanClient(cl);

    // Remove from client list immediately (no zombie timeout needed)
    SV_RemoveClient(cl);
}

/*
==================
SV_ProxyGameData

Process a wrapped game packet from a proxied client.
==================
*/
static void SV_ProxyGameData(int slot)
{
    if (slot < 0 || slot >= svs.maxclients)
        return;

    client_t *cl = &svs.client_pool[slot];
    if (cl->state == cs_free || cl->state == cs_zombie || !cl->proxy_client)
        return;

    // Strip 6-byte proxy header and reinitialize buffer
    int header_size = 6;
    size_t game_data_size = msg_read.cursize - header_size;
    memmove(msg_read.data, msg_read.data + header_size, game_data_size);
    SZ_InitRead(&msg_read, msg_read.data, game_data_size);

    netchan_t *netchan = &cl->netchan;
    if (!Netchan_Process(netchan))
        return;

    if (cl->state == cs_zombie)
        return;

    cl->lastmessage = svs.realtime;
#if USE_ICMP
    cl->unreachable = false;
#endif
    if (netchan->dropped > 0)
        cl->frameflags |= FF_CLIENTDROP;

    sv_client = cl;
    sv_player = cl->edict;
    SV_ExecuteClientMessage(cl);
    sv_client = NULL;
    sv_player = NULL;
}

/*
==================
send_slot_assigned

Confirm slot allocation to proxy.
==================
*/
static void send_slot_assigned(int slot)
{
    byte buffer[8];
    WL32(buffer, PROXY_MAGIC);
    buffer[4] = PROXY_SLOT_ASSIGNED;
    buffer[5] = (byte)slot;
    NET_SendPacket(NS_SERVER, buffer, 6, &net_from);
}

/*
==================
proxy_netchan_send

Netchan callback that wraps outgoing packets with proxy header.
==================
*/
static void proxy_netchan_send(void *opaque, const void *data, size_t len, const netadr_t *to)
{
    client_t *client = (client_t *)opaque;
    byte buffer[MAX_PACKETLEN_DEFAULT + 16];

    if (len > sizeof(buffer) - 6) {
        Com_WPrintf("proxy_netchan_send: packet too large (%zu)\n", len);
        return;
    }

    WL32(buffer, PROXY_MAGIC);
    buffer[4] = PROXY_GAMEDATA;
    buffer[5] = (byte)client->number;
    memcpy(buffer + 6, data, len);

    NET_SendPacket(NS_SERVER, buffer, len + 6, to);
}

/*
==================
SV_ProxySendToClient

Alternative send path for proxy clients (if not using netchan callback).
==================
*/
void SV_ProxySendToClient(client_t *client, const void *data, size_t len)
{
    if (!client->proxy_client) {
        NET_SendPacket(NS_SERVER, data, len, &client->netchan.remote_address);
        return;
    }

    byte buffer[MAX_PACKETLEN_DEFAULT + 16];
    if (len > sizeof(buffer) - 6) {
        Com_WPrintf("SV_ProxySendToClient: packet too large (%zu)\n", len);
        return;
    }

    WL32(buffer, PROXY_MAGIC);
    buffer[4] = PROXY_GAMEDATA;
    buffer[5] = (byte)client->number;
    memcpy(buffer + 6, data, len);

    NET_SendPacket(NS_SERVER, buffer, len + 6, &client->netchan.remote_address);
}

/*
==================
SV_RequestTransfer

Request the proxy to transfer a player to another backend server.
Called from game code (trigger_server_portal).

Packet format: PROXY_MAGIC | cmd=6 | slot | target_server (null-terminated)
==================
*/
void SV_RequestTransfer(const edict_t *player, const char *target_server)
{
    if (!sv_proxy_mode || !sv_proxy_mode->integer) {
        Com_Printf("SV_RequestTransfer: not in proxy mode\n");
        return;
    }

    if (!player || !player->client) {
        Com_Printf("SV_RequestTransfer: invalid player\n");
        return;
    }

    if (!target_server || !*target_server) {
        Com_Printf("SV_RequestTransfer: no target server specified\n");
        return;
    }

    // Find the client for this player
    int slot = -1;
    for (int i = 0; i < svs.maxclients; i++) {
        if (svs.client_pool[i].edict == player && svs.client_pool[i].state != cs_free) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        Com_Printf("SV_RequestTransfer: player not found in client pool\n");
        return;
    }

    client_t *client = &svs.client_pool[slot];
    if (!client->proxy_client) {
        Com_Printf("SV_RequestTransfer: player is not a proxy client\n");
        return;
    }

    size_t target_len = strlen(target_server) + 1;  // Include null terminator
    byte buffer[128];

    if (target_len > sizeof(buffer) - 10) {
        Com_Printf("SV_RequestTransfer: target server name too long\n");
        return;
    }

    // Packet format: PROXY_MAGIC(4) | cmd(1) | slot(1) | seq(4) | target_server\0
    uint32_t seq = client->netchan.outgoing_sequence;

    WL32(buffer, PROXY_MAGIC);
    buffer[4] = PROXY_TRANSFER_REQUEST;
    buffer[5] = (byte)slot;
    WL32(buffer + 6, seq);
    memcpy(buffer + 10, target_server, target_len);

    Com_Printf("Requesting transfer for %s to %s\n", client->name, target_server);
    NET_SendPacket(NS_SERVER, buffer, 10 + target_len, &net_from);
}
