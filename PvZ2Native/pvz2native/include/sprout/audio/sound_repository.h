#ifndef SPROUT_AUDIO_SOUND_REPOSITORY_H
#define SPROUT_AUDIO_SOUND_REPOSITORY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *name;
    uint8_t *buffer;
    size_t buffer_size;
    int freq;
    int num_channels;
    int bits_per_sample;
} sprout_sound_resource_t;

typedef struct {
    char *name;
    size_t resources_count;
    sprout_sound_resource_t resources[4];
} sprout_sound_t;

sprout_sound_resource_t *sprout_get_sound_buffer(char *name);

#endif