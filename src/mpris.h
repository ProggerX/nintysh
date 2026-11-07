#ifndef NINTYSH_MPRIS_H
#define NINTYSH_MPRIS_H

#include <glib.h>

typedef struct {
    gboolean available;
    char *player;
    char *identity;
    char *status;
    char *title;
    char *artist;
    char *album;
    char *art_url;
    char *track_id;
    gint64 length_us;
    gint64 position_us;
    gboolean can_play;
    gboolean can_pause;
    gboolean can_go_next;
    gboolean can_go_previous;
    gboolean can_seek;
    gboolean can_control;
    double volume;
} MprisState;

typedef struct {
    const char *bus_name;
    const char *identity;
    const char *status;
} MprisPlayerInfo;

typedef struct MprisClient MprisClient;
typedef void (*MprisChangedFunc)(MprisClient *client, gpointer user_data);

MprisClient *mpris_client_new(MprisChangedFunc changed, gpointer user_data);
void mpris_client_free(MprisClient *client);

gboolean mpris_client_poll(MprisClient *client);
const MprisState *mpris_client_state(MprisClient *client);

guint mpris_client_player_count(MprisClient *client);
const MprisPlayerInfo *mpris_client_player(MprisClient *client,
                                           guint index);
void mpris_client_select_player(MprisClient *client,
                                const char *bus_name);

void mpris_client_play_pause(MprisClient *client);
void mpris_client_play(MprisClient *client);
void mpris_client_pause(MprisClient *client);
void mpris_client_next(MprisClient *client);
void mpris_client_previous(MprisClient *client);
void mpris_client_seek(MprisClient *client, gint64 offset_us);
void mpris_client_set_position(MprisClient *client, gint64 position_us);

#endif
