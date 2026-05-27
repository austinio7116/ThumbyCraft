/*
 * Headless tall-grass-TUFT candidate chooser. Generates candidate 16×16
 * cutout sprites (magenta = transparent) for BLK_TALL_GRASS, plains-
 * tinted, shown over a neutral backdrop (magenta → grey) and upscaled so
 * the blade shapes + transparency read.
 *
 *   ./build_host/craft_grass <out_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "craft_blocks.h"

uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int s) { (void)s; return 0; }
const uint16_t *craft_save_slot_thumb(int s) { (void)s; return NULL; }

#define S CRAFT_TEX_SIZE
#define KEY 0xF81Fu
#define RGB565C(r,g,b) (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define TINT_T 165
static const uint16_t PLAINS = RGB565C(140,200,70);
static const uint16_t BG     = RGB565C(80,80,90);   /* show transparency */

static uint16_t tint(uint16_t c, uint16_t tgt){
    int r1=(c>>11)&0x1F,g1=(c>>5)&0x3F,b1=c&0x1F;
    int r2=(tgt>>11)&0x1F,g2=(tgt>>5)&0x3F,b2=tgt&0x1F;
    return (uint16_t)(((r1+((r2-r1)*TINT_T>>8))<<11)|((g1+((g2-g1)*TINT_T>>8))<<5)|(b1+((b2-b1)*TINT_T>>8)));
}
static void plot(uint16_t *t,int x,int y,uint16_t c){ if(x>=0&&x<S&&y>=0&&y<S) t[y*S+x]=c; }

/* Base (untinted) blade green at brightness f; the renderer biome-tints
 * it, so keep it green here. Lighter f = sun-catching tip. */
static uint16_t blade(float f){
    int g=(int)(140*f), r=(int)(44*f), b=(int)(40*f);
    if(g>200)g=200; if(g<0)g=0; if(r<0)r=0; if(b<0)b=0;
    return rgb565(r,g,b);
}

/* One blade: base (bx,15) rising to top row `top`, leaning by `lean`
 * per 6 rows, optional 2px width, lighter toward the tip. */
static void draw_blade(uint16_t *t,int bx,int lean,int top,int wide,int seed_head){
    for(int y=S-1;y>=top;y--){
        int up=(S-1)-y; int x=bx+(lean*up)/6;
        float f=0.78f+0.025f*up;            /* lighter toward tip */
        if(f>1.25f)f=1.25f;
        plot(t,x,y,blade(f));
        if(wide && (up&1)) plot(t,x+1,y,blade(f-0.12f));
    }
    if(seed_head){                          /* warm: seed cluster at tip */
        int x=bx+(lean*(S-1-top))/6;
        plot(t,x,top-1,rgb565(150,165,60));
        plot(t,x-1,top,rgb565(135,150,52));
        plot(t,x+1,top,rgb565(135,150,52));
    }
}

/* Bright seed head at a blade tip — a clear golden cluster: a tall
 * 1px-wide bright spike with a couple of diagonally-offset side grains
 * (different rows, so no flat horizontal bar). */
static void seed_spike(uint16_t *t,int x,int top){
    uint16_t hi=rgb565(210,215,70), md=rgb565(180,190,60);
    plot(t,x,top-1,hi); plot(t,x,top-2,hi); plot(t,x,top-3,hi); plot(t,x,top-4,md);
    plot(t,x-1,top-2,md);          /* offset grains on different rows */
    plot(t,x+1,top-3,md);
}

static void cand(int id, uint16_t *t){
    for(int i=0;i<S*S;i++) t[i]=KEY;
    if(id==0){            /* A: light-tips — clean fanned blades, no seeds */
        const int bx[5]={3,6,8,10,13},ln[5]={-2,-1,0,1,2},tp[5]={6,3,1,3,6};
        for(int b=0;b<5;b++) draw_blade(t,bx[b],ln[b],tp[b],0,0);
    } else if(id==1){     /* B: light-tips + clear seed heads on most blades */
        const int bx[5]={4,6,8,10,12},ln[5]={-2,-1,0,1,2},tp[5]={7,4,3,4,7};
        for(int b=0;b<5;b++){ draw_blade(t,bx[b],ln[b],tp[b],0,0);
            if(b!=0 && b!=4){ int x=bx[b]+(ln[b]*(S-1-tp[b]))/6; seed_spike(t,x,tp[b]); } }
    } else {              /* C: half-height tuft — fans out like the others */
        const int bx[6]={3,5,7,9,11,13},ln[6]={-3,-2,-1,1,2,3},tp[6]={10,8,7,8,9,11};
        for(int b=0;b<6;b++) draw_blade(t,bx[b],ln[b],tp[b],0,0);
    }
    for(int i=0;i<S*S;i++) if(t[i]!=KEY) t[i]=tint(t[i],PLAINS);
}

static void write_bmp(const char *path,const uint16_t *t){
    int W=S,H=S,rowsz=(W*3+3)&~3,off=54,fsz=off+rowsz*H;uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';hdr[2]=fsz;hdr[3]=fsz>>8;hdr[4]=fsz>>16;hdr[5]=fsz>>24;
    hdr[10]=off;hdr[14]=40;hdr[18]=W;hdr[22]=H;hdr[26]=1;hdr[28]=24;
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);uint8_t row[64];
    for(int y=H-1;y>=0;y--){memset(row,0,rowsz);for(int x=0;x<W;x++){uint16_t c=t[y*S+x]; if(c==KEY)c=BG;
        row[x*3+0]=(c&0x1F)*255/31;row[x*3+1]=((c>>5)&0x3F)*255/63;row[x*3+2]=((c>>11)&0x1F)*255/31;}
        fwrite(row,1,rowsz,f);}fclose(f);
}

int main(int argc,char**argv){
    const char*dir=argc>1?argv[1]:"/tmp/grass"; uint16_t t[S*S]; char path[256];
    const char*name[3]={"A light-tips","B seed-heads","C half-height"};
    for(int i=0;i<3;i++){cand(i,t);snprintf(path,sizeof path,"%s/g%d.bmp",dir,i);write_bmp(path,t);printf("%s\n",name[i]);}
    return 0;
}
