/* Packed Playdate bitmap cells, based on the documented PDI/PDT layout:
 * https://github.com/cranksters/playdate-reverse-engineering/blob/main/formats/pdi.md
 * Included by card.c; pixel values are 0=white, 1=black, 2=transparent.
 */
#include <zlib.h>
#include <math.h>
#define IMAGE_MT "q2.image"
#define TABLE_MT "q2.imagetable"
enum { IMAGE_LIMIT = 1024, ASSET_LIMIT = 8 * 1024 * 1024 };
typedef struct { int w, h; uint8_t pixels[]; } Q2Image;
static char image_bundle[4096];

static unsigned image_u16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}
static uint32_t image_u32(const uint8_t *p)
{
    return image_u16(p) | (uint32_t)image_u16(p + 2) << 16;
}
static Q2Image *image_alloc(lua_State *L, int w, int h, int fill)
{
    Q2Image *im = lua_newuserdatauv(L, sizeof *im + (size_t)w * h, 0);
    im->w = w; im->h = h;
    memset(im->pixels, fill, (size_t)w * h);
    luaL_setmetatable(L, IMAGE_MT);
    return im;
}
static int image_bit(const uint8_t *p, unsigned stride, unsigned x, unsigned y)
{
    return (p[(size_t)y * stride + x / 8] >> (7 - x % 8)) & 1;
}
/* Validate dimensions and storage before allocating or touching pixels. */
static int image_cell_valid(const uint8_t *p, size_t n)
{
    if (n < 16) return 0;
    unsigned w = image_u16(p), h = image_u16(p+2), stride = image_u16(p+4);
    unsigned fullw = w + image_u16(p+6) + image_u16(p+8);
    unsigned fullh = h + image_u16(p+10) + image_u16(p+12);
    return fullw > 0 && fullh > 0 && fullw <= IMAGE_LIMIT && fullh <= IMAGE_LIMIT &&
           stride >= (w+7)/8 && (uint64_t)stride*h*((image_u16(p+14)&3)?2:1) <= n-16;
}
static void image_cell(lua_State *L, const uint8_t *p)
{
    unsigned w = image_u16(p), h = image_u16(p+2), stride = image_u16(p+4);
    unsigned left = image_u16(p+6), top = image_u16(p+10);
    int mask = (image_u16(p+14)&3) != 0;
    Q2Image *im = image_alloc(L, w+left+image_u16(p+8), h+top+image_u16(p+12), 2);
    const uint8_t *bits = p+16, *alpha = bits+(size_t)stride*h;
    for (unsigned y=0;y<h;y++)
        for (unsigned x=0;x<w;x++)
            if (!mask || image_bit(alpha,stride,x,y))
                im->pixels[(size_t)(y+top)*im->w+x+left] = !image_bit(bits,stride,x,y);
}
static uint8_t *image_read(lua_State *L, const char *name, const char *ext,
                           const char *magic, size_t *size)
{
    char path[8192];
    size_t len = strlen(name), elen = strlen(ext);
    if (!len || name[0]=='/' || strstr(name,"..") || strchr(name,'\\')) return NULL;
    int count = snprintf(path,sizeof path,"%s/%s%s",image_bundle,name,
        len>=elen && !strcmp(name+len-elen,ext) ? "" : ext);
    if (count<0 || (size_t)count>=sizeof path) return NULL;
    FILE *f = fopen(path,"rb");
    if (!f) return NULL;
    if (fseek(f,0,SEEK_END)) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n<16 || n>ASSET_LIMIT || fseek(f,0,SEEK_SET)) { fclose(f); return NULL; }
    /* Lua-owned temporary buffers remain reclaimable on Lua allocation errors. */
    uint8_t *raw = lua_newuserdatauv(L,(size_t)n,0);
    size_t got = fread(raw,1,(size_t)n,f);
    fclose(f);
    if (got!=(size_t)n || memcmp(raw,magic,12)) return NULL;
    uint32_t flags = image_u32(raw+12);
    if (flags & 0x40000000u) return NULL;
    if (!(flags & 0x80000000u)) { *size=got-16; return raw+16; }
    if (got<32) return NULL;
    unsigned long out = image_u32(raw+16), expected=out;
    if (!out || out>ASSET_LIMIT) return NULL;
    uint8_t *data = lua_newuserdatauv(L,out,0);
    if (uncompress(data,&out,raw+32,got-32)!=Z_OK || out!=expected) return NULL;
    *size=out;
    return data;
}
static int image_new(lua_State *L)
{
    if (lua_type(L,1)==LUA_TSTRING) {
        const char *name=luaL_checkstring(L,1);
        size_t size;
        uint8_t *p=image_read(L,name,".pdi","Playdate IMG",&size);
        if (!p || !image_cell_valid(p,size)) { lua_pushnil(L); return 1; }
        image_cell(L,p);
    } else {
        lua_Integer w=luaL_checkinteger(L,1), h=luaL_checkinteger(L,2);
        luaL_argcheck(L,w>0 && w<=IMAGE_LIMIT,1,"image width out of range");
        luaL_argcheck(L,h>0 && h<=IMAGE_LIMIT,2,"image height out of range");
        int c=luaL_optinteger(L,3,2);
        image_alloc(L,(int)w,(int)h,c==COLOR_BLACK?1:c==COLOR_WHITE?0:2);
    }
    return 1;
}
static Q2Image *image_get(lua_State *L)
{
    return luaL_checkudata(L,1,IMAGE_MT);
}
static int image_size(lua_State *L)
{
    Q2Image *im=image_get(L);
    lua_pushinteger(L,im->w); lua_pushinteger(L,im->h); return 2;
}
static int image_copy(lua_State *L)
{
    Q2Image *im=image_get(L), *copy=image_alloc(L,im->w,im->h,2);
    memcpy(copy->pixels,im->pixels,(size_t)im->w*im->h); return 1;
}
static int image_clear(lua_State *L)
{
    Q2Image *im=image_get(L);
    int c=luaL_optinteger(L,2,2);
    memset(im->pixels,c==COLOR_BLACK?1:c==COLOR_WHITE?0:2,(size_t)im->w*im->h);
    return 0;
}
static int image_sample(lua_State *L)
{
    Q2Image *im=image_get(L);
    lua_Integer x=luaL_checkinteger(L,2), y=luaL_checkinteger(L,3);
    int p=2;
    if (x>=0 && y>=0 && x<im->w && y<im->h) p=im->pixels[(size_t)y*im->w+x];
    lua_pushinteger(L,p==1?COLOR_BLACK:p==0?COLOR_WHITE:2); return 1;
}
static int image_blit(lua_State *L, int scaled)
{
    Q2Image *im=image_get(L);
    double xx=luaL_checknumber(L,2), yy=luaL_checknumber(L,3);
    if (!isfinite(xx)||!isfinite(yy)||xx<-32768||xx>32768||yy<-32768||yy>32768)
        return luaL_error(L,"image position out of range");
    double sx=scaled?luaL_checknumber(L,4):1;
    double sy=scaled?luaL_optnumber(L,5,sx):1;
    if (!(sx>0&&sx<=16&&sy>0&&sy<=16)) return luaL_error(L,"image scale out of range");
    int flip=scaled?0:luaL_optinteger(L,4,0);
    int x=(int)xx,y=(int)yy,w=(int)(im->w*sx),h=(int)(im->h*sy);
    int l=clamp(x,SOURCE_WIDTH),r=clamp(x+w,SOURCE_WIDTH);
    int t=clamp(y,SOURCE_HEIGHT),b=clamp(y+h,SOURCE_HEIGHT);
    for(int dy=t;dy<b;dy++) for(int dx=l;dx<r;dx++) {
        int ix=(int)((dx-x)/sx),iy=(int)((dy-y)/sy);
        if(flip&1) ix=im->w-1-ix;
        if(flip&2) iy=im->h-1-iy;
        if((unsigned)ix>=(unsigned)im->w||(unsigned)iy>=(unsigned)im->h) continue;
        int p=im->pixels[(size_t)iy*im->w+ix];
        if(p!=2) frame[dy][dx]=p;
    }
    return 0;
}
static int image_draw(lua_State *L) { return image_blit(L,0); }
static int image_scaled(lua_State *L) { return image_blit(L,1); }
static int table_new(lua_State *L)
{
    size_t n;
    uint8_t *p=image_read(L,luaL_checkstring(L,1),".pdt","Playdate IMT",&n);
    if(!p || n<4) { lua_pushnil(L); return 1; }
    unsigned count=image_u16(p);
    size_t start=4+(size_t)count*4;
    if(!count || count>256 || start>n) { lua_pushnil(L); return 1; }
    size_t prev=0, pixels=0;
    for(unsigned i=0;i<count;i++) {
        size_t end=image_u32(p+4+i*4);
        if(end<prev || end>n-start || !image_cell_valid(p+start+prev,end-prev)) {
            lua_pushnil(L); return 1;
        }
        const uint8_t *c=p+start+prev;
        pixels+=(size_t)(image_u16(c)+image_u16(c+6)+image_u16(c+8))*
            (image_u16(c+2)+image_u16(c+10)+image_u16(c+12));
        if(pixels>ASSET_LIMIT) { lua_pushnil(L); return 1; }
        prev=end;
    }
    lua_createtable(L,count,0);
    prev=0;
    for(unsigned i=0;i<count;i++) {
        image_cell(L,p+start+prev);
        lua_rawseti(L,-2,i+1);
        prev=image_u32(p+4+i*4);
    }
    luaL_setmetatable(L,TABLE_MT);
    return 1;
}
static int table_get(lua_State *L)
{
    lua_rawgeti(L,1,luaL_checkinteger(L,2)); return 1;
}
static int table_length(lua_State *L)
{
    lua_pushinteger(L,lua_rawlen(L,1)); return 1;
}
static int table_draw(lua_State *L)
{
    lua_rawgeti(L,1,luaL_checkinteger(L,2));
    if(lua_isnil(L,-1)) return 0;
    lua_replace(L,2);
    lua_remove(L,1);
    return image_draw(L);
}
static void image_install(lua_State *L)
{
    static const luaL_Reg methods[]={
        {"new",image_new},{"getSize",image_size},{"copy",image_copy},
        {"clear",image_clear},{"sample",image_sample},{"draw",image_draw},
        {"drawScaled",image_scaled},{NULL,NULL}};
    luaL_newmetatable(L,IMAGE_MT);
    luaL_setfuncs(L,methods,0);
    lua_pushvalue(L,-1); lua_setfield(L,-2,"__index");
    lua_setfield(L,-2,"image");
    static const luaL_Reg tables[]={
        {"new",table_new},{"getImage",table_get},{"getLength",table_length},
        {"drawImage",table_draw},{NULL,NULL}};
    luaL_newmetatable(L,TABLE_MT);
    luaL_setfuncs(L,tables,0);
    lua_pushvalue(L,-1); lua_setfield(L,-2,"__index");
    lua_setfield(L,-2,"imagetable");
    lua_pushinteger(L,2); lua_setfield(L,-2,"kColorClear");
    lua_pushinteger(L,0); lua_setfield(L,-2,"kImageUnflipped");
    lua_pushinteger(L,1); lua_setfield(L,-2,"kImageFlippedX");
    lua_pushinteger(L,2); lua_setfield(L,-2,"kImageFlippedY");
    lua_pushinteger(L,3); lua_setfield(L,-2,"kImageFlippedXY");
}
