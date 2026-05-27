/*
 * Headless leaf-cluster chooser — renders candidate leaf cutout
 * textures built from overlapping leaf-shaped blobs (NOT noise), with
 * the gaps between clumps as holes. Holes shown as sky-blue; prints the
 * actual hole % per tile. Toroidally wrapped so the tile tessellates.
 *
 *   ./build_host/craft_leaves <out_dir>
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
static const uint16_t FOREST = RGB565C(50,120,48);
static const uint16_t SKY    = RGB565C(120,170,235);

static uint16_t tint(uint16_t c, uint16_t tgt){
    int r1=(c>>11)&0x1F,g1=(c>>5)&0x3F,b1=c&0x1F;
    int r2=(tgt>>11)&0x1F,g2=(tgt>>5)&0x3F,b2=tgt&0x1F;
    return (uint16_t)(((r1+((r2-r1)*TINT_T>>8))<<11)|((g1+((g2-g1)*TINT_T>>8))<<5)|(b1+((b2-b1)*TINT_T>>8)));
}
static uint32_t h2(int a,int b){uint32_t n=(uint32_t)(a*73856093)^(uint32_t)(b*19349663);n^=n>>13;n*=0x9E3779B1u;n^=n>>16;return n;}

/* Build a leaf tile from many SMALL leaf stamps (each ~3px, a little
 * teardrop) on a 2px grid; some grid cells are skipped → gaps. Tiles
 * toroidally. fillpct = chance a cell gets a leaf. */
static void plot(uint16_t *dst,int x,int y,uint16_t c){ dst[((y%S+S)%S)*S + (x%S+S)%S]=c; }
static void leaf_tile(uint16_t *dst, int fillpct){
    for(int i=0;i<S*S;i++) dst[i]=KEY;
    const uint16_t gr[4]={ rgb565(40,150,52), rgb565(30,128,44), rgb565(52,165,64), rgb565(22,104,36) };
    for(int gy=0; gy<S; gy+=2)
        for(int gx=0; gx<S; gx+=2){
            uint32_t hh=h2(gx+1,gy+1);
            if((int)(hh%100) >= fillpct) continue;       /* skipped → hole */
            uint16_t base=gr[hh&3];
            uint16_t tip =gr[3];                          /* darker tip */
            int ox=(hh>>9)&1, oy=(hh>>10)&1;             /* sub-cell jitter */
            int x=gx+ox, y=gy+oy;
            /* tiny teardrop leaf: body 2px + a tip pixel */
            plot(dst,x,  y,  base);
            plot(dst,x+1,y,  base);
            plot(dst,x,  y+1,tip);
            if(hh&0x10000) plot(dst,x+1,y+1,base);        /* sometimes 4px */
        }
}

static void write_bmp(const char *path, const uint16_t *px){
    int W=S,H=S,rowsz=(W*3+3)&~3,off=54,fsz=off+rowsz*H;uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';hdr[2]=fsz;hdr[3]=fsz>>8;hdr[4]=fsz>>16;hdr[5]=fsz>>24;
    hdr[10]=off;hdr[14]=40;hdr[18]=W;hdr[22]=H;hdr[26]=1;hdr[28]=24;
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);uint8_t row[64];
    for(int y=H-1;y>=0;y--){memset(row,0,rowsz);for(int x=0;x<W;x++){uint16_t c=px[y*W+x];
        row[x*3+0]=(c&0x1F)*255/31;row[x*3+1]=((c>>5)&0x3F)*255/63;row[x*3+2]=((c>>11)&0x1F)*255/31;}
        fwrite(row,1,rowsz,f);}fclose(f);
}

int main(int argc, char **argv){
    const char *dir = argc>1?argv[1]:"/tmp/leaves";
    int nb[5] = {60, 70, 80, 90, 100};
    uint16_t t[S*S], o[S*S];
    char path[256];
    for(int i=0;i<5;i++){
        leaf_tile(t, nb[i]);
        int holes=0;
        for(int p=0;p<S*S;p++){ if(t[p]==KEY){o[p]=SKY;holes++;} else o[p]=tint(t[p],FOREST); }
        snprintf(path,sizeof path,"%s/d%d.bmp",dir,i);
        write_bmp(path,o);
        printf("N%d %d%%\n", nb[i], holes*100/(S*S));
    }
    return 0;
}
