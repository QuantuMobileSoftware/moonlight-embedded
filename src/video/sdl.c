/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "video.h"
#include "ffmpeg.h"

#include "../sdl.h"

#include <SDL.h>
#include <SDL_thread.h>

#include <unistd.h>
#include <stdbool.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>

#define DECODER_BUFFER_SIZE 92*1024

static char* ffmpeg_buffer;

static int h264streamfd = 0;

static int sdl_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  char * h264stream = "/tmp/stream.h264";
  h264streamfd = open(h264stream, O_WRONLY);
  if (h264streamfd == -1) {
    fprintf(stderr, "Couldn't open FIFO\n");
    return -1;
  }

  int avc_flags = SLICE_THREADING;

  if (ffmpeg_init(videoFormat, width, height, avc_flags, SDL_BUFFER_FRAMES, sysconf(_SC_NPROCESSORS_ONLN)) < 0) {
    fprintf(stderr, "Couldn't initialize video decoding\n");
    return -1;
  }
  
  ffmpeg_buffer = malloc(DECODER_BUFFER_SIZE + FF_INPUT_BUFFER_PADDING_SIZE);
  if (ffmpeg_buffer == NULL) {
    fprintf(stderr, "Not enough memory\n");
    ffmpeg_destroy();
    return -1;
  }

  return 0;
}

static void sdl_cleanup() {
  ffmpeg_destroy();
  if (h264streamfd != 0) {
    close(h264streamfd);
    h264streamfd = 0;
  }
}

static int sdl_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  if (decodeUnit->fullLength < DECODER_BUFFER_SIZE) {
    PLENTRY entry = decodeUnit->bufferList;
    int length = 0;
    while (entry != NULL) {
      memcpy(ffmpeg_buffer+length, entry->data, entry->length);
      length += entry->length;
      entry = entry->next;
    }

    if (h264streamfd != 0) {
        ssize_t left = length;
        while (left > 0) {
          ssize_t written = write(h264streamfd, ffmpeg_buffer + (length - left), left);
          if (written == -1) {
            fprintf(stderr, "Couldn't write to FIFO\n");
            break;
          }
          left -= written;
        }
    }
    
    ffmpeg_decode(ffmpeg_buffer, length);

    if (SDL_LockMutex(mutex) == 0) {
      AVFrame* frame = ffmpeg_get_frame(false);
      if (frame != NULL) {
        sdlNextFrame++;

        SDL_Event event;
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME;
        event.user.data1 = &frame->data;
        event.user.data2 = &frame->linesize;
        SDL_PushEvent(&event);
      }

      SDL_UnlockMutex(mutex);
    } else
      fprintf(stderr, "Couldn't lock mutex\n");
  } else {
    fprintf(stderr, "Video decode buffer too small");
    exit(1);
  }

  return DR_OK;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_sdl = {
  .setup = sdl_setup,
  .cleanup = sdl_cleanup,
  .submitDecodeUnit = sdl_submit_decode_unit,
  .capabilities = CAPABILITY_SLICES_PER_FRAME(4) | CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC | CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC | CAPABILITY_DIRECT_SUBMIT,
};
