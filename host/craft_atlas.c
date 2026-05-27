/*
 * Headless texture-atlas dump — writes the raw 16×16 block textures as
 * flat tiles (NOT an in-game render), one BMP per (biome, column) cell,
 * with the biome tint applied to grass/leaves/tufts exactly like the
 * renderer does. A montage script then assembles the labelled grid.
 *
 *   ./build_host/craft_atlas <out_dir>
 *
 * Columns: grass-top, grass-side, leaves, tall-grass, flowerR, flowerY,
 * surface (biome's own), log. Rows: the 8 biomes. Cutout texels
 * (magenta) are shown over neutral grey so the plant shape reads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "craft_blocks.h"

uint32_t craft_platform_rand32(void) { return (uint32_t)rand(); }
int craft_save_slot_used(int s) { (void)s; return 0; }
const uint16_t *craft_save_slot_thumb(int s) { (void)s; return NULL; }

#define KEY 0xF81Fu
#define S   CRAFT_TEX_SIZE

/* Biome tint targets + strength — copied from craft_render.c. */
#define RGB565C(r,g,b) (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
static const uint16_t biome_tgt[8] = {
    RGB565C(140,200, 70), RGB565C( 50,120, 48), RGB565C(190,180,120),
    RGB565C( 88,140,120), RGB565C( 52, 74, 40), RGB565C(100,130,100),
    RGB565C( 35,140, 38), RGB565C(195,175, 70),
};
#define TINT_T 165

static uint16_t tint(uint16_t c, uint16_t tgt) {
    int r1=(c>>11)&0x1F, g1=(c>>5)&0x3F, b1=c&0x1F;
    int r2=(tgt>>11)&0x1F, g2=(tgt>>5)&0x3F, b2=tgt&0x1F;
    int rr=r1+((r2-r1)*TINT_T>>8), gg=g1+((g2-g1)*TINT_T>>8), bb=b1+((b2-b1)*TINT_T>>8);
    return (uint16_t)((rr<<11)|(gg<<5)|bb);
}

static void write_tile_bmp(const char *path, const uint16_t *px) {
    int W=S, H=S, rowsz=(W*3+3)&~3, off=54, fsz=off+rowsz*H;
    uint8_t hdr[54]={0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=fsz; hdr[3]=fsz>>8; hdr[4]=fsz>>16; hdr[5]=fsz>>24;
    hdr[10]=off; hdr[14]=40; hdr[18]=W; hdr[22]=H; hdr[26]=1; hdr[28]=24;
    FILE *f=fopen(path,"wb"); fwrite(hdr,1,54,f);
    uint8_t row[64];
    for (int y=H-1;y>=0;y--){
        memset(row,0,rowsz);
        for (int x=0;x<W;x++){
            uint16_t c=px[y*W+x];
            row[x*3+0]=(c&0x1F)*255/31;
            row[x*3+1]=((c>>5)&0x3F)*255/63;
            row[x*3+2]=((c>>11)&0x1F)*255/31;
        }
        fwrite(row,1,rowsz,f);
    }
    fclose(f);
}

/* Build one tile into `out`: sample block/face texture, optionally tint
 * for the biome, and key magenta to grey for cutout sprites. */
static void build_tile(uint16_t *out, BlockId blk, Face face,
                       int biome, int do_tint, int cutout) {
    const uint16_t *t = craft_block_texture(blk, face);
    for (int i=0;i<S*S;i++){
        uint16_t c=t[i];
        if (cutout && c==KEY) { out[i]=RGB565C(96,96,96); continue; }
        if (do_tint) c=tint(c, biome_tgt[biome]);
        out[i]=c;
    }
}

int main(int argc, char **argv) {
    const char *dir = argc>1 ? argv[1] : "/tmp/atlas";
    uint16_t tile[S*S];
    char path[256];

    for (int b=0;b<8;b++){
        /* col 0 grass-top (tinted) */
        build_tile(tile, BLK_GRASS, FACE_PY, b, 1, 0);
        snprintf(path,sizeof path,"%s/r%d_c0.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 1 grass-side (untinted) */
        build_tile(tile, BLK_GRASS, FACE_PZ, b, 0, 0);
        snprintf(path,sizeof path,"%s/r%d_c1.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 2 leaves (tinted) */
        build_tile(tile, BLK_LEAVES, FACE_PZ, b, 1, 0);
        snprintf(path,sizeof path,"%s/r%d_c2.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 3 tall grass (tinted, cutout) */
        build_tile(tile, BLK_TALL_GRASS, FACE_PZ, b, 1, 1);
        snprintf(path,sizeof path,"%s/r%d_c3.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 4 flower red (cutout) */
        build_tile(tile, BLK_FLOWER_RED, FACE_PZ, b, 0, 1);
        snprintf(path,sizeof path,"%s/r%d_c4.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 5 flower yellow (cutout) */
        build_tile(tile, BLK_FLOWER_YELLOW, FACE_PZ, b, 0, 1);
        snprintf(path,sizeof path,"%s/r%d_c5.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 6 surface block (per biome) */
        BlockId surf; Face sf=FACE_PY; int st=0;
        if (b==2)      surf=BLK_SAND;
        else if (b==3) surf=BLK_SNOW;
        else if (b==5) surf=BLK_SNOWY_ROCK;
        else { surf=BLK_GRASS; st=1; }   /* tinted grass top */
        build_tile(tile, surf, sf, b, st, 0);
        snprintf(path,sizeof path,"%s/r%d_c6.bmp",dir,b); write_tile_bmp(path,tile);
        /* col 7 log */
        build_tile(tile, BLK_WOOD, FACE_PZ, b, 0, 0);
        snprintf(path,sizeof path,"%s/r%d_c7.bmp",dir,b); write_tile_bmp(path,tile);
    }
    printf("wrote 8x8 tiles to %s\n", dir);
    return 0;
}
