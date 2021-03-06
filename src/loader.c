#include "loader.h"
#include "bitmap.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <SDL2/SDL.h>
#include <FreeImage.h>

static void block_usr1_signal(void);
static int is_thread_cancelled(void);
static void *bg_new_img(void *data);
static void *bg_next_frame(void *data);
static void error_occurred(struct imv_loader *ldr);

struct imv_loader {
  pthread_mutex_t lock;
  pthread_t bg_thread;
  char *path;
  BYTE *buffer;
  size_t buffer_size;
  FIMEMORY *fi_buffer;
  FIMULTIBITMAP *mbmp;
  FIBITMAP *bmp;
  int width;
  int height;
  int cur_frame;
  int next_frame;
  int num_frames;
  double frame_time;
  unsigned int new_image_event;
  unsigned int bad_image_event;
};

static void block_usr1_signal(void)
{
  sigset_t sigmask;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGUSR1);
  sigprocmask(SIG_SETMASK, &sigmask, NULL);
}

static int is_thread_cancelled(void)
{
  sigset_t sigmask;
  sigpending(&sigmask);
  return sigismember(&sigmask, SIGUSR1);
}

static struct imv_bitmap *to_imv_bitmap(FIBITMAP *in_bmp)
{
  struct imv_bitmap *bmp = malloc(sizeof(struct imv_bitmap));
  bmp->width = FreeImage_GetWidth(in_bmp);
  bmp->height = FreeImage_GetHeight(in_bmp);
  bmp->data = malloc(4 * bmp->width * bmp->height);
  FreeImage_ConvertToRawBits(bmp->data, in_bmp, 4 * bmp->width, 32,
      FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK, TRUE);
  return bmp;
}

struct imv_loader *imv_loader_create(void)
{
  struct imv_loader *ldr = malloc(sizeof(struct imv_loader));
  memset(ldr, 0, sizeof(struct imv_loader));
  pthread_mutex_init(&ldr->lock, NULL);
  /* ignore this signal in case we accidentally receive it */
  block_usr1_signal();
  return ldr;
}

void imv_loader_free(struct imv_loader *ldr)
{
  /* wait for any existing bg thread to finish */
  if(ldr->bg_thread) {
    pthread_join(ldr->bg_thread, NULL);
  }
  pthread_mutex_destroy(&ldr->lock);

  if(ldr->bmp) {
    FreeImage_Unload(ldr->bmp);
  }
  if(ldr->mbmp) {
    FreeImage_CloseMultiBitmap(ldr->mbmp, 0);
  }
  if(ldr->path) {
    free(ldr->path);
  }
  free(ldr);
}

void imv_loader_load(struct imv_loader *ldr, const char *path,
                     const void *buffer, const size_t buffer_size)
{
  pthread_mutex_lock(&ldr->lock);

  /* cancel existing thread if already running */
  if(ldr->bg_thread) {
    pthread_kill(ldr->bg_thread, SIGUSR1);
    pthread_detach(ldr->bg_thread);
  }

  /* kick off a new thread to load the image */
  if(ldr->path) {
    free(ldr->path);
  }
  ldr->path = strdup(path);
  if (strncmp(path, "-", 2) == 0) {
    ldr->buffer = (BYTE *)buffer;
    ldr->buffer_size = buffer_size;
  } else if (ldr->fi_buffer != NULL) {
    FreeImage_CloseMemory(ldr->fi_buffer);
    ldr->fi_buffer = NULL;
  }
  pthread_create(&ldr->bg_thread, NULL, &bg_new_img, ldr);
  pthread_mutex_unlock(&ldr->lock);
}

void imv_loader_set_event_types(struct imv_loader *ldr,
    unsigned int new_image,
    unsigned int bad_image)
{
  ldr->new_image_event = new_image;
  ldr->bad_image_event = bad_image;
}

void imv_loader_load_next_frame(struct imv_loader *ldr)
{
  /* wait for existing thread to finish if already running */
  if(ldr->bg_thread) {
    pthread_join(ldr->bg_thread, NULL);
  }

  /* kick off a new thread */
  pthread_create(&ldr->bg_thread, NULL, &bg_next_frame, ldr);
}

void imv_loader_time_passed(struct imv_loader *ldr, double dt)
{
  int get_frame = 0;
  pthread_mutex_lock(&ldr->lock);
  if(ldr->num_frames > 1) {
    ldr->frame_time -= dt;
    if(ldr->frame_time < 0.0) {
      get_frame = 1;
    }
  } else {
    ldr->frame_time = 0.0;
  }
  pthread_mutex_unlock(&ldr->lock);

  if(get_frame) {
    imv_loader_load_next_frame(ldr);
  }
}

double imv_loader_time_left(struct imv_loader *ldr)
{
  return ldr->frame_time;
}

static void *bg_new_img(void *data)
{
  /* so we can poll for it */
  block_usr1_signal();

  struct imv_loader *ldr = data;
  char path[PATH_MAX] = "-";

  pthread_mutex_lock(&ldr->lock);
  int from_stdin = !strncmp(path, ldr->path, 2);
  if(!from_stdin) {
    (void)snprintf(path, PATH_MAX, "%s", ldr->path);
  }
  pthread_mutex_unlock(&ldr->lock);

  FREE_IMAGE_FORMAT fmt;
  if (from_stdin) {
    pthread_mutex_lock(&ldr->lock);
    ldr->fi_buffer = FreeImage_OpenMemory(ldr->buffer, ldr->buffer_size);
    fmt = FreeImage_GetFileTypeFromMemory(ldr->fi_buffer, 0);
    pthread_mutex_unlock(&ldr->lock);
  } else {
    fmt = FreeImage_GetFileType(path, 0);
  }
  if(fmt == FIF_UNKNOWN) {
    if (from_stdin) {
      pthread_mutex_lock(&ldr->lock);
      FreeImage_CloseMemory(ldr->fi_buffer);
      ldr->fi_buffer = NULL;
      pthread_mutex_unlock(&ldr->lock);
    }
    error_occurred(ldr);
    return NULL;
  }

  int num_frames = 1;
  FIMULTIBITMAP *mbmp = NULL;
  FIBITMAP *bmp = NULL;
  int width, height;
  int raw_frame_time = 0;

  if(fmt == FIF_GIF) {
    if(from_stdin) {
      pthread_mutex_lock(&ldr->lock);
      mbmp = FreeImage_LoadMultiBitmapFromMemory(FIF_GIF, ldr->fi_buffer,
          GIF_LOAD256);
      pthread_mutex_unlock(&ldr->lock);
    } else {
      mbmp = FreeImage_OpenMultiBitmap(FIF_GIF, path,
      /* don't create file */ 0,
      /* read only */ 1,
      /* keep in memory */ 1,
      /* flags */ GIF_LOAD256);
    }
    if(!mbmp) {
      error_occurred(ldr);
      return NULL;
    }

    num_frames = FreeImage_GetPageCount(mbmp);

    FIBITMAP *frame = FreeImage_LockPage(mbmp, 0);
    width = FreeImage_GetWidth(frame);
    height = FreeImage_GetHeight(frame);
    bmp = FreeImage_ConvertTo32Bits(frame);

    /* get duration of first frame */
    FITAG *tag = NULL;
    FreeImage_GetMetadata(FIMD_ANIMATION, frame, "FrameTime", &tag);
    if(FreeImage_GetTagValue(tag)) {
      raw_frame_time = *(int*)FreeImage_GetTagValue(tag);
    } else {
      raw_frame_time = 100; /* default value for gifs */
    }
    FreeImage_UnlockPage(mbmp, frame, 0);

  } else {
    /* Future TODO: If we load image line-by-line we could stop loading large
     * ones before wasting much more time/memory on them. */

    int flags = (fmt == FIF_JPEG) ? JPEG_EXIFROTATE : 0;
    FIBITMAP *image;
    if(from_stdin) {
      pthread_mutex_lock(&ldr->lock);
      image = FreeImage_LoadFromMemory(fmt, ldr->fi_buffer, flags);
      pthread_mutex_unlock(&ldr->lock);
    } else {
      image = FreeImage_Load(fmt, path, flags);
    }
    if(!image) {
      error_occurred(ldr);
      pthread_mutex_lock(&ldr->lock);
      FreeImage_CloseMemory(ldr->fi_buffer);
      ldr->fi_buffer = NULL;
      pthread_mutex_unlock(&ldr->lock);
      return NULL;
    }

    /* Check for cancellation before we convert pixel format */
    if(is_thread_cancelled()) {
      FreeImage_Unload(image);
      return NULL;
    }

    width = FreeImage_GetWidth(bmp);
    height = FreeImage_GetHeight(bmp);
    bmp = FreeImage_ConvertTo32Bits(image);
    FreeImage_Unload(image);
  }

  /* now update the loader */
  pthread_mutex_lock(&ldr->lock);

  /* check for cancellation before finishing */
  if(is_thread_cancelled()) {
    if(mbmp) {
      FreeImage_CloseMultiBitmap(mbmp, 0);
    }
    if(bmp) {
      FreeImage_Unload(bmp);
    }
    pthread_mutex_unlock(&ldr->lock);
    return NULL;
  }

  if(ldr->mbmp) {
    FreeImage_CloseMultiBitmap(ldr->mbmp, 0);
  }

  if(ldr->bmp) {
    FreeImage_Unload(ldr->bmp);
  }

  ldr->mbmp = mbmp;
  ldr->bmp = bmp;

  ldr->width = width;
  ldr->height = height;
  ldr->cur_frame = 0;
  ldr->next_frame = 1;
  ldr->num_frames = num_frames;
  ldr->frame_time = (double)raw_frame_time * 0.0001;

  /* return the image via SDL event queue */
  SDL_Event event;
  SDL_zero(event);
  event.type = ldr->new_image_event;
  event.user.data1 = to_imv_bitmap(bmp);
  event.user.code = 1; /* is a new image */
  SDL_PushEvent(&event);

  pthread_mutex_unlock(&ldr->lock);
  return NULL;
}

static void *bg_next_frame(void *data)
{
  struct imv_loader *ldr = data;

  pthread_mutex_lock(&ldr->lock);
  int num_frames = ldr->num_frames;
  if(num_frames < 2) {
    pthread_mutex_unlock(&ldr->lock);
    return NULL;
  }

  FITAG *tag = NULL;
  char disposal_method = 0;
  int frame_time = 0;
  short top = 0;
  short left = 0;

  ldr->cur_frame = ldr->next_frame;
  ldr->next_frame = (ldr->cur_frame + 1) % ldr->num_frames;
  FIBITMAP *frame = FreeImage_LockPage(ldr->mbmp, ldr->cur_frame);
  FIBITMAP *frame32 = FreeImage_ConvertTo32Bits(frame);

  /* First frame is always going to use the raw frame */
  if(ldr->cur_frame > 0) {
    FreeImage_GetMetadata(FIMD_ANIMATION, frame, "DisposalMethod", &tag);
    if(FreeImage_GetTagValue(tag)) {
      disposal_method = *(char*)FreeImage_GetTagValue(tag);
    }
  }

  FreeImage_GetMetadata(FIMD_ANIMATION, frame, "FrameLeft", &tag);
  if(FreeImage_GetTagValue(tag)) {
    left = *(short*)FreeImage_GetTagValue(tag);
  }

  FreeImage_GetMetadata(FIMD_ANIMATION, frame, "FrameTop", &tag);
  if(FreeImage_GetTagValue(tag)) {
    top = *(short*)FreeImage_GetTagValue(tag);
  }

  FreeImage_GetMetadata(FIMD_ANIMATION, frame, "FrameTime", &tag);
  if(FreeImage_GetTagValue(tag)) {
    frame_time = *(int*)FreeImage_GetTagValue(tag);
  }

  /* some gifs don't provide a frame time at all */
  if(frame_time == 0) {
    frame_time = 100;
  }
  ldr->frame_time += frame_time * 0.001;

  FreeImage_UnlockPage(ldr->mbmp, frame, 0);

  /* If this frame is inset, we need to expand it for compositing */
  if(ldr->width != (int)FreeImage_GetWidth(frame32) ||
     ldr->height != (int)FreeImage_GetHeight(frame32)) {
    FIBITMAP *expanded = FreeImage_Allocate(ldr->width, ldr->height, 32, 0,0,0);
    FreeImage_Paste(expanded, frame32, left, top, 255);
    FreeImage_Unload(frame32);
    frame32 = expanded;
  }

  switch(disposal_method) {
    case 0: /* nothing specified, fall through to compositing */
    case 1: /* composite over previous frame */
      if(ldr->bmp && ldr->cur_frame > 0) {
        FIBITMAP *bg_frame = FreeImage_ConvertTo24Bits(ldr->bmp);
        FreeImage_Unload(ldr->bmp);
        FIBITMAP *comp = FreeImage_Composite(frame32, 1, NULL, bg_frame);
        FreeImage_Unload(bg_frame);
        FreeImage_Unload(frame32);
        ldr->bmp = comp;
      } else {
        /* No previous frame, just render directly */
        if(ldr->bmp) {
          FreeImage_Unload(ldr->bmp);
        }
        ldr->bmp = frame32;
      }
      break;
    case 2: /* TODO - set to background, composite over that */
      if(ldr->bmp) {
        FreeImage_Unload(ldr->bmp);
      }
      ldr->bmp = frame32;
      break;
    case 3: /* TODO - restore to previous content */
      if(ldr->bmp) {
        FreeImage_Unload(ldr->bmp);
      }
      ldr->bmp = frame32;
      break;
  }

  SDL_Event event;
  SDL_zero(event);
  event.type = ldr->new_image_event;
  event.user.data1 = to_imv_bitmap(ldr->bmp);
  event.user.code = 0; /* not a new image */
  SDL_PushEvent(&event);

  pthread_mutex_unlock(&ldr->lock);
  return NULL;
}

static void error_occurred(struct imv_loader *ldr)
{
  pthread_mutex_lock(&ldr->lock);
  char *err_path = strdup(ldr->path);
  pthread_mutex_unlock(&ldr->lock);

  SDL_Event event;
  SDL_zero(event);
  event.type = ldr->bad_image_event;
  event.user.data1 = err_path;
  SDL_PushEvent(&event);
}


/* vim:set ts=2 sts=2 sw=2 et: */
