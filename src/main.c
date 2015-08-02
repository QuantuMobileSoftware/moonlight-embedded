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

#include "loop.h"
#include "client.h"
#include "connection.h"
#include "video.h"
#include "audio.h"
#include "discover.h"
#include "config.h"
#include "platform.h"
#include "sdl.h"

#include "input/evdev.h"
#include "input/udev.h"
#include "input/cec.h"

#include "limelight-common/Limelight.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <openssl/rand.h>

static void applist(PSERVER_DATA server) {
  PAPP_LIST list;
  if (gs_applist(server, list) != GS_OK) {
    fprintf(stderr, "Can't get app list\n");
    return;
  }

  for (int i = 1;list != NULL;i++) {
    printf("%d. %s\n", i, list->name);
    list = list->next;
  }
}

static int get_app_id(PSERVER_DATA server, const char *name) {
  PAPP_LIST list;
  if (gs_applist(server, list) != GS_OK) {
    fprintf(stderr, "Can't get app list\n");
    return -1;
  }

  while (list != NULL) {
    if (strcmp(list->name, name) == 0)
      return list->id;

    list = list->next;
  }
  return -1;
}

static void stream(PSERVER_DATA server, PCONFIGURATION config, enum platform system) {
  int appId = get_app_id(server, config->app);
  if (appId<0) {
    fprintf(stderr, "Can't find app %s\n", config->app);
    exit(-1);
  }

  gs_start_app(server, &config->stream, appId, config->sops, config->localaudio);

  LiStartConnection(server->address, &config->stream, &connection_callbacks, platform_get_video(system), platform_get_audio(system), NULL, 0, server->serverMajorVersion);

  if (IS_EMBEDDED(system))
    loop_main();
  #ifdef HAVE_SDL
  else if (system == SDL)
    sdl_loop();
  #endif

  LiStopConnection();
}

static void help() {
  printf("Usage: moonlight action [options] host\n\n");
  printf(" Actions\n\n");
  printf("\tmap\t\t\tCreate mapping file for gamepad\n");
  printf("\tpair\t\t\tPair device with computer\n");
  printf("\tstream\t\t\tStream computer to device\n");
  printf("\tlist\t\t\tList available games and applications\n");
  printf("\tquit\t\t\tQuit the application or game being streamed\n");
  printf("\thelp\t\t\tShow this help\n\n");
  printf(" Streaming options\n\n");
  printf("\t-config <config>\tLoad configuration file\n");
  printf("\t-save <config>\t\tSave configuration file\n");
  printf("\t-720\t\t\tUse 1280x720 resolution [default]\n");
  printf("\t-1080\t\t\tUse 1920x1080 resolution\n");
  printf("\t-width <width>\t\tHorizontal resolution (default 1280)\n");
  printf("\t-height <height>\tVertical resolution (default 720)\n");
  printf("\t-30fps\t\t\tUse 30fps\n");
  printf("\t-60fps\t\t\tUse 60fps [default]\n");
  printf("\t-bitrate <bitrate>\tSpecify the bitrate in Kbps\n");
  printf("\t-packetsize <size>\tSpecify the maximum packetsize in bytes\n");
  printf("\t-app <app>\t\tName of app to stream\n");
  printf("\t-nosops\t\t\tDon't allow GFE to modify game settings\n");
  printf("\t-input <device>\t\tUse <device> as input. Can be used multiple times\n");
  printf("\t-mapping <file>\t\tUse <file> as gamepad mapping configuration file (use before -input)\n");
  printf("\t-audio <device>\t\tUse <device> as ALSA audio output device (default sysdefault)\n");
  printf("\t-localaudio\t\tPlay audio locally\n");
  printf("\t-keydir <directory>\tLoad encryption keys from directory\n\n");
  printf("Use Ctrl+Alt+Shift+Q to exit streaming session\n\n");
  exit(0);
}

static void pair_check(PSERVER_DATA server) {
  if (!server->paired) {
    fprintf(stderr, "You must pair with the PC first\n");
    exit(-1);
  }
}

int main(int argc, char* argv[]) {
  CONFIGURATION config;
  config_parse(argc, argv, &config);

  if (config.action == NULL || strcmp("help", config.action) == 0)
    help();
  
  enum platform system = platform_check(config.platform);
  if (system == 0) {
    fprintf(stderr, "Platform '%s' not found\n", config.platform);
    exit(-1);
  }
  
  if (strcmp("map", config.action) == 0) {
    if (config.address == NULL) {
      perror("No filename for mapping");
      exit(-1);
    }
    udev_init(!inputAdded, config.mapping);
    for (int i=0;i<config.inputsCount;i++)
      evdev_create(config.inputs[i].path, config.inputs[i].mapping);
    
    evdev_map(config.address);
    exit(0);
  }

  if (config.address == NULL) {
    config.address = malloc(MAX_ADDRESS_SIZE);
    if (config.address == NULL) {
      perror("Not enough memory");
      exit(-1);
    }
    config.address[0] = 0;
    gs_discover_server(config.address);
    if (config.address[0] == 0) {
      fprintf(stderr, "Autodiscovery failed. Specify an IP address next time.\n");
      exit(-1);
    }
  }

  PSERVER_DATA server;
  if (gs_init(server, config.address, config.key_dir) != GS_OK) {
      fprintf(stderr, "Can't connect to server %s\n", config.address);
      exit(-1);
  }

  if (strcmp("list", config.action) == 0) {
    pair_check(server);
    applist(server);
  } else if (strcmp("stream", config.action) == 0) {
    pair_check(server);
    if (IS_EMBEDDED(system)) {
      for (int i=0;i<config.inputsCount;i++)
        evdev_create(config.inputs[i].path, config.inputs[i].mapping);

      udev_init(!inputAdded, config.mapping);
      evdev_init();
      #ifdef HAVE_LIBCEC
      cec_init();
      #endif /* HAVE_LIBCEC */
    }

    stream(server, &config, system);
  } else if (strcmp("pair", config.action) == 0) {
    char pin[5];
    sprintf(pin, "%d%d%d%d", (int)random() % 10, (int)random() % 10, (int)random() % 10, (int)random() % 10);
    printf("Please enter the following PIN on the target PC: %s\n", pin);
    if (gs_pair(server, &pin[0]) != GS_OK) {
      fprintf(stderr, "Failed to pair to server: %s\n", gs_error);
    } else {
      printf("Succesfully paired\n");
    }
  } else if (strcmp("quit", config.action) == 0) {
    pair_check(server);
    gs_quit_app(server);
  } else
    fprintf(stderr, "%s is not a valid action\n", config.action);
}
