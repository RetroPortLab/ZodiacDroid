typedef unsigned u32; typedef int s32; typedef unsigned long long u64;
static u32 udiv(u32 n, u32 d, u32 *r){ u32 q=0; for(int i=31;i>=0;i--){ if((n>>i)>=d){ n-=d<<i; q|=1u<<i; } } *r=n; return q; }
u32 __aeabi_uidiv(u32 n,u32 d){ u32 r; return udiv(n,d,&r); }
u64 __aeabi_uidivmod(u32 n,u32 d){ u32 r; u32 q=udiv(n,d,&r); return ((u64)r<<32)|q; }
s32 __aeabi_idiv(s32 n,s32 d){ u32 r; u32 q=udiv(n<0?-(u32)n:n, d<0?-(u32)d:d,&r); return ((n<0)^(d<0))?-(s32)q:(s32)q; }
u64 __aeabi_idivmod(s32 n,s32 d){ u32 r; u32 q=udiv(n<0?-(u32)n:n, d<0?-(u32)d:d,&r); s32 sq=((n<0)^(d<0))?-(s32)q:(s32)q; s32 sr=n<0?-(s32)r:(s32)r; return ((u64)(u32)sr<<32)|(u32)sq; }
u64 __aeabi_lmul(u64 a,u64 b){ u32 al=a,ah=a>>32,bl=b,bh=b>>32;
  u32 a0=al&0xFFFF,a1=al>>16,b0=bl&0xFFFF,b1=bl>>16;
  u32 p00=a0*b0,p01=a0*b1,p10=a1*b0,p11=a1*b1;
  u32 mid=(p00>>16)+(p01&0xFFFF)+(p10&0xFFFF);
  u32 lo=(p00&0xFFFF)|(mid<<16); u32 hi=p11+(p01>>16)+(p10>>16)+(mid>>16)+al*bh+ah*bl;
  union{u64 v; u32 w[2];} r; r.w[0]=lo; r.w[1]=hi; return r.v; }
