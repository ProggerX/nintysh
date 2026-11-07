#ifndef NINTYSH_GAMMA_H
#define NINTYSH_GAMMA_H

#include <glib.h>

typedef struct GammaClient GammaClient;

GammaClient *gamma_client_new(void);
void gamma_client_free(GammaClient *client);

guint gamma_client_output_count(GammaClient *client);
const char *gamma_client_output_name(GammaClient *client, guint index);
int gamma_client_brightness(GammaClient *client, guint index);
int gamma_client_temperature(GammaClient *client, guint index);

void gamma_client_set_brightness(GammaClient *client, guint index,
                                 int percent);
void gamma_client_set_temperature(GammaClient *client, guint index,
                                  int kelvin);

#endif
