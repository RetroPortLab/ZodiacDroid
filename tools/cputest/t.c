typedef unsigned u32; typedef int s32; typedef unsigned long long u64;
static u32 h=2166136261u; static void mix(u32 v){ h=(h^v)*16777619u; }
u32 seed=12345; static u32 rnd(void){ seed=seed*1103515245u+12345u; return seed ^ (seed>>15); }
struct S { u32 a; unsigned char b[7]; s32 c; } arr[16];
static int cmp(const void*a,const void*b){ return *(s32*)a - *(s32*)b; }
static u32 rol(u32 x,int n){ return (x<<n)|(x>>(32-n)); }
static void sha1blk(u32 *h, const unsigned char *p){
  u32 w[80]; for(int i=0;i<16;i++) w[i]=(p[4*i]<<24)|(p[4*i+1]<<16)|(p[4*i+2]<<8)|p[4*i+3];
  for(int i=16;i<80;i++) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
  u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
  for(int i=0;i<80;i++){ u32 f,k; if(i<20){f=(b&c)|(~b&d);k=0x5a827999;} else if(i<40){f=b^c^d;k=0x6ed9eba1;} else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8f1bbcdc;} else {f=b^c^d;k=0xca62c1d6;}
    u32 t=rol(a,5)+f+e+k+w[i]; e=d; d=c; c=rol(b,30); b=a; a=t; }
  h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; }
static unsigned char buf[4096];
/* bignum multiply-accumulate: exercises UMULL/UMLAL/SMLAL */
static void bigmul(u32 *r, const u32 *a, const u32 *b, int n){
  for(int i=0;i<2*n;i++) r[i]=0;
  for(int i=0;i<n;i++){ u64 c=0; for(int j=0;j<n;j++){ c += (u64)a[i]*b[j] + r[i+j]; r[i+j]=(u32)c; c>>=32; } r[i+n]=(u32)c; }
}
static long long smac(const int *a,const int *b,int n){ long long s=0; for(int i=0;i<n;i++) s+=(long long)a[i]*b[i]; return s; }
u32 run(void){
  { u32 hh[5]={0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
    for(int i=0;i<4096;i++) buf[i]=(unsigned char)(rnd()>>7);
    for(int i=0;i<4096;i+=64) sha1blk(hh,buf+i);
    for(int i=0;i<5;i++) mix(hh[i]); }
  { u32 a[32],b[32],r[64]; for(int i=0;i<32;i++){a[i]=rnd();b[i]=rnd();}
    for(int k=0;k<20;k++){ bigmul(r,a,b,32); for(int i=0;i<32;i++){ a[i]=r[i]^r[63-i]; } }
    for(int i=0;i<64;i++) mix(r[i]);
    int sa[64],sb[64]; for(int i=0;i<64;i++){sa[i]=(int)rnd();sb[i]=(int)rnd();}
    long long q=smac(sa,sb,64); mix((u32)q); mix((u32)(q>>32)); }
  for(int i=0;i<2000;i++){
    u32 a=rnd(), b=rnd()|1; s32 sa=(s32)a, sb=(s32)b;
    mix(a+b); mix(a-b); mix(a*b); mix(a/b); mix(a%b); mix(sa/sb); mix(sa%sb);
    mix(a<<(b&31)); mix(a>>(b&31)); mix((u32)(sa>>(b&31))); mix(a<b); mix(sa<sb);
    u64 p=(u64)a*b; mix((u32)(p>>32)); mix((u32)p);
    long long q=(long long)sa*sb; mix((u32)(q>>32));
    unsigned char *c=(unsigned char*)&arr[i&15]; c[(a&15)] = (unsigned char)b; mix(c[(b&15)]);
    arr[i&15].c += sa>>3; mix(arr[(i*7)&15].c);
    signed char sc=(signed char)a; mix((u32)(s32)sc);
  }
  s32 v[64]; for(int i=0;i<64;i++) v[i]=(s32)rnd();
  for(int i=1;i<64;i++){ s32 k=v[i]; int j=i-1; while(j>=0&&v[j]>k){v[j+1]=v[j];j--;} v[j+1]=k; }
  for(int i=0;i<64;i++) mix(v[i]);
  return h;
}
#ifdef HOST
#include <stdio.h>
int main(){ printf("%08x\n", run()); }
#else
u32 run_entry(void){ return run(); }
#endif
