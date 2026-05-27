/*
 * Headless "greens chooser" — for each biome, renders the grass-top
 * texture under several candidate tint colours so we can pick the best
 * green per biome. Writes one BMP per (biome, option) and prints a
 * label token (R,G,B) per tile in grid order for the montage script.
 *
 *   ./build_host/craft_greens <out_dir>
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
#define RGB565C(r,g,b) (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define TINT_T 165

/* Candidate greens: [biome][option][r,g,b]. Option 0 is the current
 * shipped value; 1..3 are alternatives. Biome order matches CraftBiome:
 * plains, forest, desert, taiga, swamp, mountains, jungle, savanna. */
static const int cand[8][4][3] = {
    /* plains   */ {{140,200,70},{125,190,80},{150,205,85},{115,180,75}},
    /* forest   */ {{ 60,140,55},{ 70,150,62},{ 50,120,48},{ 80,155,65}},
    /* desert   */ {{190,180,120},{170,165,110},{200,190,130},{160,160,105}},
    /* taiga    */ {{140,175,165},{110,160,150},{ 88,140,120},{120,170,170}},
    /* swamp    */ {{ 95,100,40},{ 72, 88,42},{ 60, 90,50},{ 52, 74,40}},
    /* mountains*/ {{130,150,120},{112,140,112},{122,148,108},{100,130,100}},
    /* jungle   */ {{ 45,175,40},{ 40,155,42},{ 55,168,48},{ 35,140,38}},
    /* savanna  */ {{195,175,70},{185,168,82},{178,160,92},{170,155,75}},
};

static uint16_t tint(uint16_t c, uint16_t tgt) {
    int r1=(c>>11)&0x1F, g1=(c>>5)&0x3F, b1=c&0x1F;
    int r2=(tgt>>11)&0x1F, g2=(tgt>>5)&0x3F, b2=tgt&0x1F;
    int rr=r1+((r2-r1)*TINT_T>>8), gg=g1+((g2-g1)*TINT_T>>8), bb=b1+((b2-b1)*TINT_T>>8);
    return (uint16_t)((rr<<11)|(gg<<5)|bb);
}

static void write_bmp(const char *path, const uint16_t *px) {
    int W=S,H=S,rowsz=(W*3+3)&~3,off=54,fsz=off+rowsz*H;
    uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';hdr[2]=fsz;hdr[3]=fsz>>8;hdr[4]=fsz>>16;hdr[5]=fsz>>24;
    hdr[10]=off;hdr[14]=40;hdr[18]=W;hdr[22]=H;hdr[26]=1;hdr[28]=24;
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);
    uint8_t row[64];
    for(int y=H-1;y>=0;y--){memset(row,0,rowsz);
        for(int x=0;x<W;x++){uint16_t c=px[y*W+x];
            row[x*3+0]=(c&0x1F)*255/31;row[x*3+1]=((c>>5)&0x3F)*255/63;row[x*3+2]=((c>>11)&0x1F)*255/31;}
        fwrite(row,1,rowsz,f);}
    fclose(f);
}

int main(int argc, char **argv) {
    const char *dir = argc>1?argv[1]:"/tmp/greens";
    const uint16_t *gtop = craft_block_texture(BLK_GRASS, FACE_PY);
    uint16_t tile[S*S];
    char path[256];
    for (int b=0;b<8;b++)
        for (int o=0;o<4;o++){
            uint16_t tgt = RGB565C(cand[b][o][0],cand[b][o][1],cand[b][o][2]);
            for (int i=0;i<S*S;i++) tile[i]=tint(gtop[i],tgt);
            snprintf(path,sizeof path,"%s/b%d_o%d.bmp",dir,b,o);
            write_bmp(path,tile);
            printf("%d,%d,%d\n", cand[b][o][0],cand[b][o][1],cand[b][o][2]);
        }
    return 0;
}
