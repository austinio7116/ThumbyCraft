/*
 * Headless chooser for flowering jungle vines + blossom leaves.
 * Row 1: flowering-vine cutout candidates (green vine + coloured
 *        blossoms), shown over grey (transparent).
 * Row 2: blossom-leaf cutout candidates (green leaves + blossom flecks),
 *        shown over sky-blue (transparent).
 * Green is jungle-tinted to read in context; blossoms stay full colour.
 *
 *   ./build_host/craft_blossom <out_dir>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "craft_blocks.h"

uint32_t craft_platform_rand32(void){return (uint32_t)rand();}
int craft_save_slot_used(int s){(void)s;return 0;}
const uint16_t *craft_save_slot_thumb(int s){(void)s;return NULL;}

#define S CRAFT_TEX_SIZE
#define KEY 0xF81Fu
#define RGB565C(r,g,b) (uint16_t)((((r)>>3)<<11)|(((g)>>2)<<5)|((b)>>3))
#define TINT_T 165
static const uint16_t JUNGLE=RGB565C(45,160,45);
static const uint16_t GREYBG=RGB565C(80,80,90);
static const uint16_t SKYBG =RGB565C(120,170,235);

static uint16_t tintg(uint16_t c,uint16_t tgt){
    int r1=(c>>11)&0x1F,g1=(c>>5)&0x3F,b1=c&0x1F,r2=(tgt>>11)&0x1F,g2=(tgt>>5)&0x3F,b2=tgt&0x1F;
    return (uint16_t)(((r1+((r2-r1)*TINT_T>>8))<<11)|((g1+((g2-g1)*TINT_T>>8))<<5)|(b1+((b2-b1)*TINT_T>>8)));
}
static int is_green(uint16_t c){int r=(c>>11)&0x1F,g=(c>>5)&0x3F,b=c&0x1F;return (g>>1)>r&&(g>>1)>=b;}
static uint32_t hsh(int a,int b){uint32_t n=(uint32_t)(a*73856093)^(uint32_t)(b*19349663);n^=n>>13;n*=0x9E3779B1u;n^=n>>16;return n;}
static void put(uint16_t*t,int x,int y,uint16_t c){int yy=((y%S)+S)%S; if(x>=0&&x<S)t[yy*S+x]=c;}

/* flower blob 2-3px in colour col with a lighter centre. */
static void blob(uint16_t*t,int x,int y,uint16_t col,uint16_t ctr){
    put(t,x,y,col);put(t,x+1,y,col);put(t,x,y+1,col);put(t,x+1,y+1,ctr);put(t,x-1,y,col);put(t,x,y-1,col);
}

/* Flowering vine: two winding green stems (period S, tiles vertically)
 * + leaves, with flower blobs hung off the stems. */
static void vine(uint16_t*t,uint16_t fcol,uint16_t fctr){
    for(int i=0;i<S*S;i++)t[i]=KEY;
    const float cx[2]={4,11},ph[2]={0,2};
    for(int s=0;s<2;s++){
        for(int y=0;y<S;y++){int x=(int)(cx[s]+1.5f*sinf(6.2831853f*y/S+ph[s])+0.5f);put(t,x,y,rgb565(40,110,40));put(t,x,y+1,rgb565(40,110,40));}
        for(int y=1;y<S;y+=4){int x=(int)(cx[s]+1.5f*sinf(6.2831853f*y/S+ph[s])+0.5f);
            int side=((y/4)&1)?1:-1; put(t,x+side,y,rgb565(60,135,55)); put(t,x+side*2,y,rgb565(60,135,55));}
    }
    for(int s=0;s<2;s++) for(int y=2;y<S;y+=5){int x=(int)(cx[s]+1.5f*sinf(6.2831853f*y/S+ph[s])+0.5f);
        blob(t,x+((y&1)?2:-2),y,fcol,fctr);}
}

/* Blossom leaf: leafy green cutout (~30% holes) + blossom clusters. */
static void bleaf(uint16_t*t,uint16_t bcol,uint16_t bctr){
    for(int i=0;i<S*S;i++)t[i]=KEY;
    for(int gy=0;gy<S;gy+=2)for(int gx=0;gx<S;gx+=2){uint32_t h=hsh(gx+1,gy+1);
        if((int)(h%100)>=88)continue; uint16_t base=rgb565(40,140,50); uint16_t tip=rgb565(30,120,44);
        int ox=(h>>9)&1,oy=(h>>10)&1; put(t,gx+ox,gy+oy,base);put(t,gx+1+ox,gy+oy,base);put(t,gx+ox,gy+1+oy,tip);
        if(h&0x10000)put(t,gx+1+ox,gy+1+oy,base);}
    for(int k=0;k<4;k++){uint32_t h=hsh(k+5,17);int x=(int)(h%S),y=(int)((h>>8)%S);
        blob(t,x,y,bcol,bctr);}
}

static void write_bmp(const char*path,const uint16_t*t,uint16_t bg,uint16_t gtint){
    int W=S,H=S,rowsz=(W*3+3)&~3,off=54,fsz=off+rowsz*H;uint8_t hdr[54]={0};
    hdr[0]='B';hdr[1]='M';hdr[2]=fsz;hdr[3]=fsz>>8;hdr[4]=fsz>>16;hdr[5]=fsz>>24;hdr[10]=off;hdr[14]=40;hdr[18]=W;hdr[22]=H;hdr[26]=1;hdr[28]=24;
    FILE*f=fopen(path,"wb");fwrite(hdr,1,54,f);uint8_t row[64];
    for(int y=H-1;y>=0;y--){memset(row,0,rowsz);for(int x=0;x<W;x++){uint16_t c=t[y*S+x];
        if(c==KEY)c=bg; else if(is_green(c))c=tintg(c,gtint);
        row[x*3+0]=(c&0x1F)*255/31;row[x*3+1]=((c>>5)&0x3F)*255/63;row[x*3+2]=((c>>11)&0x1F)*255/31;}
        fwrite(row,1,rowsz,f);}fclose(f);
}

int main(int argc,char**argv){
    const char*dir=argc>1?argv[1]:"/tmp/blossom";uint16_t t[S*S];char p[256];
    /* vine flower colours: pink, red, purple, orange */
    uint16_t vc[4]={rgb565(235,120,180),rgb565(225,60,60),rgb565(170,90,210),rgb565(245,150,60)};
    uint16_t vk[4]={rgb565(255,200,225),rgb565(255,180,90),rgb565(220,170,245),rgb565(255,220,140)};
    const char*vn[4]={"vine-pink","vine-red","vine-purple","vine-orange"};
    for(int i=0;i<4;i++){vine(t,vc[i],vk[i]);snprintf(p,sizeof p,"%s/v%d.bmp",dir,i);write_bmp(p,t,GREYBG,JUNGLE);printf("%s\n",vn[i]);}
    /* blossom-leaf colours: pink, white, yellow, magenta */
    uint16_t bc[4]={rgb565(240,150,190),rgb565(245,240,250),rgb565(245,220,90),rgb565(230,110,210)};
    uint16_t bk[4]={rgb565(255,210,230),rgb565(255,255,255),rgb565(255,245,160),rgb565(255,180,245)};
    const char*bn[4]={"leaf-pink","leaf-white","leaf-yellow","leaf-magenta"};
    for(int i=0;i<4;i++){bleaf(t,bc[i],bk[i]);snprintf(p,sizeof p,"%s/b%d.bmp",dir,i);write_bmp(p,t,SKYBG,JUNGLE);printf("%s\n",bn[i]);}
    return 0;
}
