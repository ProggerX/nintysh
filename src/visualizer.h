#ifndef NINTYSH_VISUALIZER_H
#define NINTYSH_VISUALIZER_H

#include <glib.h>

typedef struct Visualizer Visualizer;
typedef void (*VisualizerChangedFunc)(Visualizer *visualizer,
                                      gpointer user_data);

Visualizer *visualizer_new(VisualizerChangedFunc changed,
                           gpointer user_data);
void visualizer_free(Visualizer *visualizer);

void visualizer_start(Visualizer *visualizer);
void visualizer_stop(Visualizer *visualizer);

guint visualizer_level_count(Visualizer *visualizer);
double visualizer_level(Visualizer *visualizer, guint index);

#endif
