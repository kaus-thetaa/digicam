#include "digicam/image_processor.h"
#include <esp_heap_caps.h>
#include <stdlib.h>
#include <string.h>
#include <esp_system.h>
#include <stdio.h>
namespace dc {
static inline uint8_t clamp8(int v){ return (uint8_t)(v<0?0:(v>255?255:v)); }
uint8_t ImageProcessor::luma(const uint8_t* s,uint16_t w,uint16_t x,uint16_t y){
  size_t row=(size_t)y*w*2U; size_t g=row+(size_t)(x/2U)*4U; return s[g+((x&1U)?2U:0U)];
}
uint8_t ImageProcessor::chromaU(const uint8_t* s,uint16_t w,uint16_t x,uint16_t y){return s[(size_t)y*w*2U+(size_t)(x/2U)*4U+1U];}
uint8_t ImageProcessor::chromaV(const uint8_t* s,uint16_t w,uint16_t x,uint16_t y){return s[(size_t)y*w*2U+(size_t)(x/2U)*4U+3U];}
static inline void inverseMap(uint16_t outX,uint16_t outY,uint16_t srcW,uint16_t srcH,Rotation r,uint16_t& sx,uint16_t& sy){
  switch(r){
    case Rotation::Deg0:   sx=outX; sy=outY; break;
    case Rotation::Deg90:  sx=outY; sy=(uint16_t)(srcH-1U-outX); break;
    case Rotation::Deg180: sx=(uint16_t)(srcW-1U-outX); sy=(uint16_t)(srcH-1U-outY); break;
    case Rotation::Deg270: sx=(uint16_t)(srcW-1U-outY); sy=outX; break;
  }
}

// Compact 5x7 glyphs for a burned-in ISO-style date/time. The stamp is drawn
// after rotation/effects so it is always upright in both landscape and portrait output.
static const uint8_t* glyph5x7(char c) {
  static const uint8_t digits[10][7] = {
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31},
    {30,1,1,14,1,1,30}, {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14},
    {14,17,17,15,1,2,12}
  };
  static const uint8_t hyphen[7] = {0,0,0,31,0,0,0};
  static const uint8_t colon[7] = {0,4,4,0,4,4,0};
  static const uint8_t space[7] = {0,0,0,0,0,0,0};
  if (c >= '0' && c <= '9') return digits[c-'0'];
  if (c == '-') return hyphen;
  if (c == ':') return colon;
  return space;
}
static inline void putLuma(uint8_t* yuv,uint16_t w,uint16_t h,int x,int y,uint8_t value) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  size_t pair = ((size_t)y*w + (size_t)(x & ~1))*2U;
  yuv[pair + ((x & 1) ? 2U : 0U)] = value;
}
static void burnTimestamp(uint8_t* yuv,uint16_t w,uint16_t h,const char* text) {
  if (!yuv || !text || !*text || w < 220 || h < 24) return;
  const int scale=2, charAdvance=6*scale, textW=(int)strlen(text)*charAdvance;
  const int textH=7*scale, x0=(int)w-textW-7, y0=(int)h-textH-6;
  // Opaque low-luma panel improves legibility without changing chroma data.
  for(int y=y0-3;y<y0+textH+3;y++) for(int x=x0-3;x<x0+textW+3;x++) putLuma(yuv,w,h,x,y,20);
  for(size_t ci=0; text[ci]; ++ci) {
    const uint8_t* rows=glyph5x7(text[ci]);
    for(int gy=0;gy<7;gy++) for(int gx=0;gx<5;gx++) {
      if(rows[gy] & (1U << (4-gx)))
        for(int sy=0;sy<scale;sy++) for(int sx=0;sx<scale;sx++)
          putLuma(yuv,w,h,x0+(int)ci*charAdvance+gx*scale+sx,y0+gy*scale+sy,235);
    }
  }
}
uint8_t* ImageProcessor::transform(const camera_fb_t* fb,const uint8_t* previous,size_t previousLen,Rotation rot,Style style,bool grain,ImageDimensions& dims,size_t& outLen,const char* timestampText){
  if(!fb || !fb->buf || fb->format!=PIXFORMAT_YUV422 || fb->width<2 || fb->height<2 || (fb->width&1U)) return nullptr;
  const uint16_t sw=fb->width, sh=fb->height;
  const size_t required=(size_t)sw*sh*2U;
  if(fb->len<required) return nullptr;
  if(previous && previousLen<required) previous=nullptr;
  // Avoid aggregate initialization here: the ESP32 GCC 8.4 toolchain can
  // reject brace construction for this type when it has default member values.
  if(rot==Rotation::Deg90 || rot==Rotation::Deg270){
    dims.width=sh;
    dims.height=sw;
  } else {
    dims.width=sw;
    dims.height=sh;
  }
  if((dims.width&1U) || dims.height<2) return nullptr;
  outLen=(size_t)dims.width*dims.height*2U;
  uint8_t* out=(uint8_t*)heap_caps_malloc(outLen,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!out) out=(uint8_t*)heap_caps_malloc(outLen,MALLOC_CAP_8BIT);
  if(!out) return nullptr;
  uint32_t seed=esp_random()|1U;
  for(uint16_t oy=0;oy<dims.height;oy++){
    const size_t row=(size_t)oy*dims.width*2U;
    for(uint16_t ox=0;ox<dims.width;ox+=2){
      uint16_t x0,y0,x1,y1; inverseMap(ox,oy,sw,sh,rot,x0,y0); inverseMap((uint16_t)(ox+1),oy,sw,sh,rot,x1,y1);
      int y0v=luma(fb->buf,sw,x0,y0), y1v=luma(fb->buf,sw,x1,y1);
      int u0=chromaU(fb->buf,sw,x0,y0), u1=chromaU(fb->buf,sw,x1,y1);
      int v0=chromaV(fb->buf,sw,x0,y0), v1=chromaV(fb->buf,sw,x1,y1);
      if(previous){
        y0v=(y0v+luma(previous,sw,x0,y0))/2; y1v=(y1v+luma(previous,sw,x1,y1))/2;
        u0=(u0+chromaU(previous,sw,x0,y0))/2; u1=(u1+chromaU(previous,sw,x1,y1))/2;
        v0=(v0+chromaV(previous,sw,x0,y0))/2; v1=(v1+chromaV(previous,sw,x1,y1))/2;
      }
      int u=(u0+u1)/2, v=(v0+v1)/2;
      if(style==Style::Vintage){y0v=16+(y0v-16)*90/100+7;y1v=16+(y1v-16)*90/100+7;u=128+(u-128)*68/100-4;v=128+(v-128)*72/100+8;}
      else if(style==Style::Warm){y0v+=2;y1v+=2;u=128+(u-128)*90/100-5;v=128+(v-128)*95/100+7;}
      else if(style==Style::Noir){y0v=16+(y0v-16)*105/100;y1v=16+(y1v-16)*105/100;u=128;v=128;}
      else if(style==Style::Cinematic){y0v=16+(y0v-16)*88/100;y1v=16+(y1v-16)*88/100;u=128+(u-128)*62/100+3;v=128+(v-128)*72/100+2;}
      if(grain){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;int n=(int)(seed%7U)-3;y0v+=n;y1v+=n;}
      const size_t d=row+(size_t)(ox/2U)*4U;
      out[d]=clamp8(y0v);out[d+1]=clamp8(u);out[d+2]=clamp8(y1v);out[d+3]=clamp8(v);
    }
  }
  if(timestampText && *timestampText) burnTimestamp(out,dims.width,dims.height,timestampText);
  return out;
}
bool ImageProcessor::makePreview(const camera_fb_t* fb,Rotation rot,uint8_t* bitmap,size_t bitmapLen,uint8_t& ix,uint8_t& iy,uint8_t& iw,uint8_t& ih){
  if(!fb||!fb->buf||!bitmap||bitmapLen<128U*64U/8U||fb->format!=PIXFORMAT_YUV422||(fb->width&1U)||fb->len<(size_t)fb->width*fb->height*2U) return false;
  memset(bitmap,0,128U*64U/8U);
  const uint16_t sw=fb->width,sh=fb->height;
  const uint16_t ow=(rot==Rotation::Deg90||rot==Rotation::Deg270)?sh:sw;
  const uint16_t oh=(rot==Rotation::Deg90||rot==Rotation::Deg270)?sw:sh;
  // Fit the rotated image in 128x64 while preserving aspect ratio.
  uint16_t w=128,h=64;
  if((uint32_t)ow*64U > (uint32_t)oh*128U) h=(uint16_t)((uint32_t)128U*oh/ow);
  else w=(uint16_t)((uint32_t)64U*ow/oh);
  if(w==0||h==0)return false;
  ix=(uint8_t)((128U-w)/2U);iy=(uint8_t)((64U-h)/2U);iw=(uint8_t)w;ih=(uint8_t)h;
  static const uint8_t dither[4]={0,2,3,1};
  for(uint16_t y=0;y<h;y++) for(uint16_t x=0;x<w;x++){
    uint16_t ox=(uint16_t)((uint32_t)x*ow/w), oy=(uint16_t)((uint32_t)y*oh/h), sx,sy;
    inverseMap(ox,oy,sw,sh,rot,sx,sy);
    uint8_t lum=luma(fb->buf,sw,sx,sy); int threshold=112+(int)dither[((y+iy)&1U)*2U+((x+ix)&1U)]*8;
    if(lum>threshold){size_t bit=(size_t)(y+iy)*128U+(x+ix);bitmap[bit>>3]|=(uint8_t)(1U<<(bit&7U));}
  }
  return true;
}
}
