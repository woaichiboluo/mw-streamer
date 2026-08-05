#include <stdio.h>

#include "mw/c_api.h"

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <streaming-config.toml>\n", argv[0]);
    return 2;
  }

  MwResult result = mw_init(NULL);
  if (result != kMwResultSuccess) {
    fprintf(stderr, "initialization failed: %s\n", mw_last_error());
    return 1;
  }

  MwStreaming* streaming = NULL;
  result = mw_streaming_create(argv[1], &streaming);
  if (result != kMwResultSuccess) {
    fprintf(stderr, "pipeline creation failed: %s\n", mw_last_error());
    mw_shutdown();
    return 1;
  }

  printf("pipeline configuration loaded successfully\n");
  mw_streaming_destroy(streaming);
  mw_shutdown();
  return 0;
}
