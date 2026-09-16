#include "avatar_face.h"
#include <math.h>
#include <string.h>
static uint16_t u16(const uint8_t *p) { return p[0] | (p[1]<<8); }
bool avatar_package_valid(const uint8_t *p,size_t size)
{
    if(size!=AVATAR_PACKAGE_BYTES || memcmp(p,"RAV2",4) || u16(p+4)!=280 || u16(p+6)!=280) return false;
    for(int i=32;i<60;i++) if(p[i]) return false;
    for(int i=0;i<3;i++) {
        const uint8_t *r=p+8+i*8; int x=u16(r),y=u16(r+2),w=u16(r+4),h=u16(r+6);
        if(w<4 || w>120 || h<2 || h>80 || x<8 || y<8 || x+w>272 || y+h>272) return false;
    }
    uint32_t crc=0xffffffff;
    for(size_t i=64;i<size;i++) { crc^=p[i]; for(int bit=0;bit<8;bit++) crc=(crc>>1)^((crc&1)?0xedb88320:0); }
    uint32_t expected=(uint32_t)p[60] | ((uint32_t)p[61]<<8) | ((uint32_t)p[62]<<16) | ((uint32_t)p[63]<<24);
    return (crc^0xffffffff)==expected;
}
static int bound(int v) { return v<0?0:v>279?279:v; }
static void warp(const uint16_t *src,uint16_t *dst,avatar_region_t r,int mood,float a)
{
    float cx=r.x+r.w/2.0f,cy=r.y+r.h/2.0f,rx=r.w/2.0f+3,ry=r.h/2.0f+3;
    for(int y=bound((int)(cy-2*ry));y<=bound((int)(cy+2*ry));y++)
        for(int x=bound((int)(cx-1.6f*rx));x<=bound((int)(cx+1.6f*rx));x++) {
            float nx=(x-cx)/rx,ny=(y-cy)/ry;
            float edge=fmaxf(0,1-fabsf(nx)/1.6f)*fmaxf(0,1-fabsf(ny)/2);
            float sx=x,sy=y;
            if(mood==0) {
                float s=1-0.94f*a*fmaxf(0,1-powf(fabsf(nx)/1.6f,4));
                float z=fabsf(ny);
                float sourceY=z<s?z/s:1+(z-s)/(2-s);
                sy=cy+copysignf(sourceY*ry,ny);
            } else if(mood==1) {
                sx=cx+(x-cx)/(1+0.30f*a*edge);
                sy=y+6*a*edge*nx*nx;
            } else {
                sx=cx+(x-cx)/(1-0.45f*a*edge);
                sy=cy+(y-cy)/(1+0.45f*a*edge);
            }
            dst[y*280+x]=src[bound((int)roundf(sy))*280+bound((int)roundf(sx))];
        }
}
void avatar_face_render(const uint16_t *src,uint16_t *dst,const avatar_region_t r[3],int mood,float a)
{
    memcpy(dst,src,280*280*2);
    if(a<=0 || !r[0].w) return;
    if(a>1) a=1;
    if(mood==0 || mood==3) { warp(src,dst,r[0],0,a); if(mood==0) warp(src,dst,r[1],0,a); }
    else warp(src,dst,r[2],mood==1?1:2,a);
}
