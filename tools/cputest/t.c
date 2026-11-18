typedef unsigned u32; typedef int s32; typedef unsigned long long u64;
static u32 h=2166136261u; static void mix(u32 v){ h=(h^v)*16777619u; }
u32 seed=12345; static u32 rnd(void){ seed=seed*1103515245u+12345u; return seed ^ (seed>>15); }
struct S { u32 a; unsigned char b[7]; s32 c; } arr[16];
static int cmp(const void*a,const void*b){ return *(s32*)a - *(s32*)b; }
u32 run(void){
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
