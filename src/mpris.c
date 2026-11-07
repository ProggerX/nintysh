#include "mpris.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define MPRIS_PREFIX "org.mpris.MediaPlayer2."
#define MPRIS_PATH "/org/mpris/MediaPlayer2"
#define MPRIS_PLAYER "org.mpris.MediaPlayer2.Player"
#define MPRIS_ROOT "org.mpris.MediaPlayer2"

struct MprisClient {
    GDBusConnection *connection;
    MprisState state;
    GPtrArray *players;
    char *preferred;
    MprisChangedFunc changed;
    gpointer user_data;
};

static void player_info_free(MprisPlayerInfo *info) {
    if (info == NULL) {
        return;
    }
    g_free((char *)info->bus_name);
    g_free((char *)info->identity);
    g_free((char *)info->status);
    g_free(info);
}

static void state_clear(MprisState *state) {
    g_free(state->player);
    g_free(state->identity);
    g_free(state->status);
    g_free(state->title);
    g_free(state->artist);
    g_free(state->album);
    g_free(state->art_url);
    g_free(state->track_id);
    memset(state, 0, sizeof(*state));
}

static GVariant *call_sync(MprisClient *client, const char *bus_name,
                           const char *interface, const char *method,
                           GVariant *parameters,
                           const GVariantType *reply_type) {
    if (client->connection == NULL) {
        return NULL;
    }
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        client->connection, bus_name, MPRIS_PATH, interface, method,
        parameters, reply_type, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (reply == NULL) {
        g_clear_error(&error);
    }
    return reply;
}

static GVariant *get_all(MprisClient *client, const char *bus_name,
                         const char *interface) {
    GVariant *reply =
        call_sync(client, bus_name, "org.freedesktop.DBus.Properties",
                  "GetAll", g_variant_new("(s)", interface),
                  G_VARIANT_TYPE("(a{sv})"));
    if (reply == NULL) {
        return NULL;
    }
    GVariant *props = NULL;
    g_variant_get(reply, "(@a{sv})", &props);
    g_variant_unref(reply);
    return props;
}

static char *dict_dup_string(GVariant *dict, const char *key) {
    if (dict == NULL) {
        return NULL;
    }
    GVariant *value =
        g_variant_lookup_value(dict, key, G_VARIANT_TYPE_STRING);
    if (value == NULL) {
        return NULL;
    }
    char *text = g_variant_dup_string(value, NULL);
    g_variant_unref(value);
    return text;
}

static gint64 dict_int64(GVariant *dict, const char *key, gint64 fallback) {
    gint64 value = fallback;
    if (dict == NULL || !g_variant_lookup(dict, key, "x", &value)) {
        return fallback;
    }
    return value;
}

static double dict_double(GVariant *dict, const char *key,
                          double fallback) {
    double value = fallback;
    if (dict == NULL || !g_variant_lookup(dict, key, "d", &value)) {
        return fallback;
    }
    return value;
}

static gboolean dict_bool(GVariant *dict, const char *key,
                          gboolean fallback) {
    gboolean value = fallback;
    if (dict == NULL || !g_variant_lookup(dict, key, "b", &value)) {
        return fallback;
    }
    return value;
}

static char *metadata_artists(GVariant *metadata) {
    if (metadata == NULL) {
        return NULL;
    }
    GVariant *value = g_variant_lookup_value(metadata, "xesam:artist", NULL);
    if (value == NULL) {
        return NULL;
    }

    char *result = NULL;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING_ARRAY)) {
        gchar **artists = g_variant_dup_strv(value, NULL);
        GString *joined = g_string_new(NULL);
        for (int i = 0; artists != NULL && artists[i] != NULL; i++) {
            if (artists[i][0] == '\0') {
                continue;
            }
            if (joined->len > 0) {
                g_string_append(joined, ", ");
            }
            g_string_append(joined, artists[i]);
        }
        g_strfreev(artists);
        if (joined->len > 0) {
            result = g_string_free(joined, FALSE);
        } else {
            g_string_free(joined, TRUE);
        }
    } else if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        result = g_variant_dup_string(value, NULL);
    }

    g_variant_unref(value);
    return result;
}

static char *metadata_track_id(GVariant *metadata) {
    if (metadata == NULL) {
        return NULL;
    }
    GVariant *value =
        g_variant_lookup_value(metadata, "mpris:trackid", NULL);
    if (value == NULL) {
        return NULL;
    }
    char *track_id = NULL;
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH) ||
        g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        track_id = g_variant_dup_string(value, NULL);
    }
    g_variant_unref(value);
    return track_id;
}

static gboolean query_player(MprisClient *client, const char *bus_name,
                             MprisState *state) {
    GVariant *props = get_all(client, bus_name, MPRIS_PLAYER);
    if (props == NULL) {
        return FALSE;
    }
    GVariant *root = get_all(client, bus_name, MPRIS_ROOT);

    state->available = TRUE;
    state->player = g_strdup(bus_name);
    state->identity = dict_dup_string(root, "Identity");
    if (state->identity == NULL) {
        state->identity = g_strdup(bus_name + strlen(MPRIS_PREFIX));
    }
    state->status = dict_dup_string(props, "PlaybackStatus");
    state->position_us = dict_int64(props, "Position", 0);
    state->volume = dict_double(props, "Volume", 1.0);
    state->can_play = dict_bool(props, "CanPlay", FALSE);
    state->can_pause = dict_bool(props, "CanPause", FALSE);
    state->can_go_next = dict_bool(props, "CanGoNext", FALSE);
    state->can_go_previous = dict_bool(props, "CanGoPrevious", FALSE);
    state->can_seek = dict_bool(props, "CanSeek", FALSE);
    state->can_control = dict_bool(props, "CanControl", FALSE);

    GVariant *metadata = g_variant_lookup_value(
        props, "Metadata", G_VARIANT_TYPE_VARDICT);
    if (metadata != NULL) {
        state->title = dict_dup_string(metadata, "xesam:title");
        state->artist = metadata_artists(metadata);
        state->album = dict_dup_string(metadata, "xesam:album");
        state->art_url = dict_dup_string(metadata, "mpris:artUrl");
        state->track_id = metadata_track_id(metadata);
        state->length_us = dict_int64(metadata, "mpris:length", 0);
        g_variant_unref(metadata);
    }

    g_variant_unref(props);
    if (root != NULL) {
        g_variant_unref(root);
    }
    return TRUE;
}

static GPtrArray *list_players(MprisClient *client) {
    GPtrArray *players =
        g_ptr_array_new_with_free_func((GDestroyNotify)g_free);
    if (client->connection == NULL) {
        return players;
    }
    GVariant *reply = g_dbus_connection_call_sync(
        client->connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "ListNames", NULL,
        G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, NULL);
    if (reply == NULL) {
        return players;
    }

    gchar **names = NULL;
    g_variant_get(reply, "(^as)", &names);
    for (int i = 0; names != NULL && names[i] != NULL; i++) {
        if (g_str_has_prefix(names[i], MPRIS_PREFIX)) {
            g_ptr_array_add(players, g_strdup(names[i]));
        }
    }
    g_strfreev(names);
    g_variant_unref(reply);
    return players;
}

static char *player_status(MprisClient *client, const char *bus_name) {
    GVariant *reply =
        call_sync(client, bus_name, "org.freedesktop.DBus.Properties",
                  "Get",
                  g_variant_new("(ss)", MPRIS_PLAYER, "PlaybackStatus"),
                  G_VARIANT_TYPE("(v)"));
    if (reply == NULL) {
        return NULL;
    }

    GVariant *value = NULL;
    g_variant_get(reply, "(v)", &value);
    char *status = NULL;
    if (value != NULL &&
        g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
        status = g_variant_dup_string(value, NULL);
    }
    if (value != NULL) {
        g_variant_unref(value);
    }
    g_variant_unref(reply);
    return status;
}

static char *player_identity(MprisClient *client, const char *bus_name) {
    GVariant *root = get_all(client, bus_name, MPRIS_ROOT);
    char *identity = dict_dup_string(root, "Identity");
    if (root != NULL) {
        g_variant_unref(root);
    }
    if (identity == NULL) {
        identity = g_strdup(bus_name + strlen(MPRIS_PREFIX));
    }
    return identity;
}

static gboolean state_equal(const MprisState *a, const MprisState *b) {
    return a->available == b->available &&
           g_strcmp0(a->player, b->player) == 0 &&
           g_strcmp0(a->identity, b->identity) == 0 &&
           g_strcmp0(a->status, b->status) == 0 &&
           g_strcmp0(a->title, b->title) == 0 &&
           g_strcmp0(a->artist, b->artist) == 0 &&
           g_strcmp0(a->album, b->album) == 0 &&
           g_strcmp0(a->art_url, b->art_url) == 0 &&
           g_strcmp0(a->track_id, b->track_id) == 0 &&
           a->length_us == b->length_us &&
           a->position_us == b->position_us &&
           a->can_play == b->can_play &&
           a->can_pause == b->can_pause &&
           a->can_go_next == b->can_go_next &&
           a->can_go_previous == b->can_go_previous &&
           a->can_seek == b->can_seek &&
           a->can_control == b->can_control &&
           fabs(a->volume - b->volume) < 0.001;
}

MprisClient *mpris_client_new(MprisChangedFunc changed,
                              gpointer user_data) {
    MprisClient *client = g_new0(MprisClient, 1);
    client->changed = changed;
    client->user_data = user_data;
    client->players =
        g_ptr_array_new_with_free_func((GDestroyNotify)player_info_free);

    GError *error = NULL;
    client->connection =
        g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (client->connection == NULL) {
        g_clear_error(&error);
    }
    return client;
}

void mpris_client_free(MprisClient *client) {
    if (client == NULL) {
        return;
    }
    state_clear(&client->state);
    if (client->players != NULL) {
        g_ptr_array_unref(client->players);
    }
    g_free(client->preferred);
    if (client->connection != NULL) {
        g_object_unref(client->connection);
    }
    g_free(client);
}

static gboolean players_equal(GPtrArray *a, GPtrArray *b) {
    if (a->len != b->len) {
        return FALSE;
    }
    for (guint i = 0; i < a->len; i++) {
        MprisPlayerInfo *pa = g_ptr_array_index(a, i);
        MprisPlayerInfo *pb = g_ptr_array_index(b, i);
        if (g_strcmp0(pa->bus_name, pb->bus_name) != 0 ||
            g_strcmp0(pa->identity, pb->identity) != 0 ||
            g_strcmp0(pa->status, pb->status) != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

gboolean mpris_client_poll(MprisClient *client) {
    if (client == NULL) {
        return FALSE;
    }

    GPtrArray *names = list_players(client);
    GPtrArray *discovered = g_ptr_array_new_with_free_func(
        (GDestroyNotify)player_info_free);
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        MprisPlayerInfo *info = g_new0(MprisPlayerInfo, 1);
        info->bus_name = g_strdup(name);
        info->identity = player_identity(client, name);
        info->status = player_status(client, name);
        g_ptr_array_add(discovered, info);
    }
    g_ptr_array_unref(names);

    const char *chosen = NULL;
    if (client->preferred != NULL) {
        for (guint i = 0; i < discovered->len; i++) {
            MprisPlayerInfo *info = g_ptr_array_index(discovered, i);
            if (g_strcmp0(info->bus_name, client->preferred) == 0) {
                chosen = info->bus_name;
                break;
            }
        }
    }
    if (chosen == NULL) {
        for (guint i = 0; i < discovered->len; i++) {
            MprisPlayerInfo *info = g_ptr_array_index(discovered, i);
            if (g_strcmp0(info->status, "Playing") == 0) {
                chosen = info->bus_name;
                break;
            }
        }
    }
    if (chosen == NULL && client->state.player != NULL) {
        for (guint i = 0; i < discovered->len; i++) {
            MprisPlayerInfo *info = g_ptr_array_index(discovered, i);
            if (g_strcmp0(info->bus_name, client->state.player) == 0) {
                chosen = info->bus_name;
                break;
            }
        }
    }
    if (chosen == NULL && discovered->len > 0) {
        chosen = ((MprisPlayerInfo *)g_ptr_array_index(discovered, 0))
                     ->bus_name;
    }

    MprisState next = { 0 };
    gboolean state_changed = FALSE;
    if (chosen == NULL || !query_player(client, chosen, &next)) {
        state_changed = client->state.available;
        state_clear(&client->state);
    } else {
        state_changed = !state_equal(&client->state, &next);
        if (state_changed) {
            state_clear(&client->state);
            client->state = next;
        } else {
            state_clear(&next);
        }
    }

    gboolean players_changed = !players_equal(client->players, discovered);
    if (players_changed) {
        g_ptr_array_set_size(client->players, 0);
        for (guint i = 0; i < discovered->len; i++) {
            g_ptr_array_add(client->players,
                            g_ptr_array_index(discovered, i));
        }
        g_ptr_array_set_free_func(discovered, NULL);
    }
    g_ptr_array_unref(discovered);

    gboolean changed = state_changed || players_changed;
    if (changed && client->changed != NULL) {
        client->changed(client, client->user_data);
    }
    return changed;
}

const MprisState *mpris_client_state(MprisClient *client) {
    return client != NULL ? &client->state : NULL;
}

guint mpris_client_player_count(MprisClient *client) {
    return client != NULL ? client->players->len : 0;
}

const MprisPlayerInfo *mpris_client_player(MprisClient *client,
                                           guint index) {
    if (client == NULL || index >= client->players->len) {
        return NULL;
    }
    return g_ptr_array_index(client->players, index);
}

void mpris_client_select_player(MprisClient *client,
                                const char *bus_name) {
    if (client == NULL) {
        return;
    }
    g_free(client->preferred);
    client->preferred = g_strdup(bus_name);
}

static void call_player_method(MprisClient *client, const char *method) {
    if (client->connection == NULL || client->state.player == NULL) {
        return;
    }
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        client->connection, client->state.player, MPRIS_PATH, MPRIS_PLAYER,
        method, NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
    }
    g_clear_error(&error);
}

void mpris_client_play_pause(MprisClient *client) {
    if (client->state.status != NULL &&
        g_strcmp0(client->state.status, "Playing") == 0) {
        mpris_client_pause(client);
    } else {
        mpris_client_play(client);
    }
}

void mpris_client_play(MprisClient *client) {
    call_player_method(client, "Play");
}

void mpris_client_pause(MprisClient *client) {
    call_player_method(client, "Pause");
}

void mpris_client_next(MprisClient *client) {
    call_player_method(client, "Next");
}

void mpris_client_previous(MprisClient *client) {
    call_player_method(client, "Previous");
}

void mpris_client_seek(MprisClient *client, gint64 offset_us) {
    if (client->connection == NULL || client->state.player == NULL ||
        !client->state.can_seek) {
        return;
    }
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        client->connection, client->state.player, MPRIS_PATH, MPRIS_PLAYER,
        "Seek", g_variant_new("(x)", offset_us), NULL,
        G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
    }
    g_clear_error(&error);
}

void mpris_client_set_position(MprisClient *client, gint64 position_us) {
    if (client->connection == NULL || client->state.player == NULL) {
        return;
    }
    if (client->state.track_id == NULL) {
        if (client->state.can_seek) {
            mpris_client_seek(client,
                              position_us - client->state.position_us);
        }
        return;
    }

    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        client->connection, client->state.player, MPRIS_PATH, MPRIS_PLAYER,
        "SetPosition",
        g_variant_new("(ox)", client->state.track_id, position_us), NULL,
        G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (reply != NULL) {
        g_variant_unref(reply);
    }
    if (reply == NULL && client->state.can_seek) {
        mpris_client_seek(client,
                          position_us - client->state.position_us);
    }
    g_clear_error(&error);
}
