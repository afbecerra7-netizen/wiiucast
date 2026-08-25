#include "renderer.h"
#include "display_shader.h"

#include <whb/gfx.h>
#include <whb/log.h>

#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <gx2r/draw.h>

#include <malloc.h>
#include <string.h>

// Alineación que pide H264DEC para el framebuffer de salida.
#define FRAME_ALIGNMENT 0x400

typedef struct {
   GX2Texture yTex;
   GX2Texture uvTex;
   BOOL valid;
} YuvFrame;

static WHBGfxShaderGroup s_shader;
static GX2Sampler s_sampler;
static YuvFrame s_frames[VIDEO_NUM_BUFFERS];
static int s_visible = -1;       // último frame enviado a mostrar
static int s_width, s_height;
static BOOL s_ready = FALSE;

// Quad a pantalla completa. Posiciones en [0,1]; el vertex shader las
// convierte a clip space con el uniform u_screenSize (ver display.vsh).
static const float s_positions[] = {
   0.0f, 0.0f,
   1.0f, 0.0f,
   1.0f, 1.0f,
   0.0f, 1.0f,
};
static const float s_texCoords[] = {
   0.0f, 0.0f,
   1.0f, 0.0f,
   1.0f, 1.0f,
   0.0f, 1.0f,
};
static float *s_posBuf, *s_texBuf;

BOOL video_renderer_init(void)
{
   if (!WHBGfxInit()) {
      WHBLogPrintf("[video] WHBGfxInit fallo");
      return FALSE;
   }

   if (!WHBGfxLoadGFDShaderGroup(&s_shader, 0, display_gsh)) {
      WHBLogPrintf("[video] no se pudo cargar el shader de presentacion");
      return FALSE;
   }
   WHBGfxInitShaderAttribute(&s_shader, "in_pos",      0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
   WHBGfxInitShaderAttribute(&s_shader, "in_texCoord", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
   if (!WHBGfxInitFetchShader(&s_shader)) {
      WHBLogPrintf("[video] WHBGfxInitFetchShader fallo");
      return FALSE;
   }

   // Los buffers de atributos los lee la GPU: memoria alineada + invalidate.
   s_posBuf = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, sizeof(s_positions));
   s_texBuf = memalign(GX2_VERTEX_BUFFER_ALIGNMENT, sizeof(s_texCoords));
   if (!s_posBuf || !s_texBuf) {
      WHBLogPrintf("[video] sin memoria para los buffers de vertices");
      return FALSE;
   }
   memcpy(s_posBuf, s_positions, sizeof(s_positions));
   memcpy(s_texBuf, s_texCoords, sizeof(s_texCoords));
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s_posBuf, sizeof(s_positions));
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, s_texBuf, sizeof(s_texCoords));

   // CLAMP evita que el filtrado bilineal sangre del borde derecho al
   // izquierdo cuando el pitch alineado es mayor que el ancho visible.
   GX2InitSampler(&s_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

   s_ready = TRUE;
   WHBLogPrintf("[video] GX2 listo");
   return TRUE;
}

static void free_frames(void)
{
   for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
      if (s_frames[i].yTex.surface.image) {
         free(s_frames[i].yTex.surface.image);
         s_frames[i].yTex.surface.image = NULL;
      }
      s_frames[i].valid = FALSE;
   }
   s_visible = -1;
}

// Crea el par de texturas de un frame. El plano Y y el UV viven en UNA sola
// asignación contigua, porque H264DEC escribe NV12 seguido: primero Y con
// pitch alineado a 256, y justo detrás el UV entrelazado a media resolución.
static BOOL create_frame(YuvFrame *f, int width, int height)
{
   memset(&f->yTex, 0, sizeof(GX2Texture));
   memset(&f->uvTex, 0, sizeof(GX2Texture));

   int alignedH = VIDEO_FRAME_HEIGHT(height);

   f->yTex.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
   f->yTex.surface.use       = GX2_SURFACE_USE_TEXTURE;
   f->yTex.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8;
   f->yTex.surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
   f->yTex.surface.width     = width;
   f->yTex.surface.height    = alignedH;
   f->yTex.surface.depth     = 1;
   f->yTex.surface.mipLevels = 1;
   f->yTex.viewNumSlices     = 1;
   f->yTex.viewNumMips       = 1;
   f->yTex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
   GX2CalcSurfaceSizeAndAlignment(&f->yTex.surface);
   GX2InitTextureRegs(&f->yTex);

   f->uvTex.surface.dim       = GX2_SURFACE_DIM_TEXTURE_2D;
   f->uvTex.surface.use       = GX2_SURFACE_USE_TEXTURE;
   f->uvTex.surface.format    = GX2_SURFACE_FORMAT_UNORM_R8_G8;
   f->uvTex.surface.tileMode  = GX2_TILE_MODE_LINEAR_ALIGNED;
   f->uvTex.surface.width     = width / 2;
   f->uvTex.surface.height    = alignedH / 2;
   f->uvTex.surface.depth     = 1;
   f->uvTex.surface.mipLevels = 1;
   f->uvTex.viewNumSlices     = 1;
   f->uvTex.viewNumMips       = 1;
   f->uvTex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_0, GX2_SQ_SEL_1);
   GX2CalcSurfaceSizeAndAlignment(&f->uvTex.surface);
   GX2InitTextureRegs(&f->uvTex);

   uint32_t total = VIDEO_FRAME_SIZE(width, height);
   f->yTex.surface.image = memalign(FRAME_ALIGNMENT, total);
   if (!f->yTex.surface.image) return FALSE;

   memset(f->yTex.surface.image, 0, total);
   // El plano UV empieza justo detrás del Y (imageSize de la superficie Y).
   f->uvTex.surface.image = (uint8_t *)f->yTex.surface.image + f->yTex.surface.imageSize;

   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, f->yTex.surface.image, total);
   f->valid = TRUE;
   return TRUE;
}

BOOL video_renderer_set_size(int width, int height)
{
   if (!s_ready) return FALSE;
   if (width <= 0 || height <= 0) return FALSE;

   free_frames();
   s_width = width;
   s_height = height;

   for (int i = 0; i < VIDEO_NUM_BUFFERS; i++) {
      if (!create_frame(&s_frames[i], width, height)) {
         WHBLogPrintf("[video] sin memoria para las texturas %dx%d", width, height);
         free_frames();
         return FALSE;
      }
   }

   WHBLogPrintf("[video] texturas %dx%d (pitch %d, %u KB/frame)",
                width, height, VIDEO_FRAME_PITCH(width),
                VIDEO_FRAME_SIZE(width, height) / 1024);
   return TRUE;
}

void *video_renderer_framebuffer(int index)
{
   if (index < 0 || index >= VIDEO_NUM_BUFFERS) return NULL;
   return s_frames[index].yTex.surface.image;
}

void video_renderer_submit(int index)
{
   if (index < 0 || index >= VIDEO_NUM_BUFFERS || !s_frames[index].valid) return;
   // El decoder escribió con la CPU: hay que invalidar para que la GPU vea
   // los datos nuevos y no una copia vieja en caché.
   GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE,
                 s_frames[index].yTex.surface.image,
                 VIDEO_FRAME_SIZE(s_width, s_height));
   s_visible = index;
}

// Dibuja el quad con las texturas del frame visible en el target actual.
static void draw_frame(void)
{
   YuvFrame *f = &s_frames[s_visible];

   GX2SetFetchShader(&s_shader.fetchShader);
   GX2SetVertexShader(s_shader.vertexShader);
   GX2SetPixelShader(s_shader.pixelShader);

   // u_screenSize: el vertex shader mapea posiciones [0,1] a clip space
   // como pos*2*u - 1, así que con u = 1.0 el quad cubre la pantalla entera.
   float screenSize[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
   GX2SetVertexUniformReg(s_shader.vertexShader->uniformVars[0].offset, 4, screenSize);

   GX2SetPixelTexture(&f->yTex, s_shader.pixelShader->samplerVars[0].location);
   GX2SetPixelSampler(&s_sampler, s_shader.pixelShader->samplerVars[0].location);
   GX2SetPixelTexture(&f->uvTex, s_shader.pixelShader->samplerVars[1].location);
   GX2SetPixelSampler(&s_sampler, s_shader.pixelShader->samplerVars[1].location);

   GX2SetAttribBuffer(0, sizeof(s_positions), 2 * sizeof(float), s_posBuf);
   GX2SetAttribBuffer(1, sizeof(s_texCoords), 2 * sizeof(float), s_texBuf);

   GX2DrawEx(GX2_PRIMITIVE_MODE_QUADS, 4, 0, 1);
}

void video_renderer_draw(BOOL haveVideo, float bgR, float bgG, float bgB)
{
   if (!s_ready) return;

   BOOL show = haveVideo && s_visible >= 0 && s_frames[s_visible].valid;

   WHBGfxBeginRender();

   WHBGfxBeginRenderTV();
   WHBGfxClearColor(bgR, bgG, bgB, 1.0f);
   if (show) draw_frame();
   WHBGfxFinishRenderTV();

   WHBGfxBeginRenderDRC();
   WHBGfxClearColor(bgR, bgG, bgB, 1.0f);
   if (show) draw_frame();
   WHBGfxFinishRenderDRC();

   WHBGfxFinishRender();
}

int video_renderer_width(void)  { return s_width; }
int video_renderer_height(void) { return s_height; }

void video_renderer_shutdown(void)
{
   if (!s_ready) return;
   free_frames();
   free(s_posBuf); s_posBuf = NULL;
   free(s_texBuf); s_texBuf = NULL;
   WHBGfxFreeShaderGroup(&s_shader);
   WHBGfxShutdown();
   s_ready = FALSE;
}
