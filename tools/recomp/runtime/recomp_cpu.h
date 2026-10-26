// Runtime support for statically recompiled AArch64 code.
//
// Every recompiled function has the signature `void f_<addr>(cpu_t *c)` and
// operates on the guest register file in `c`. Guest memory is a flat mapping:
// guest address `a` lives at host address `recomp_mem_base + a`.

#ifndef POUND_RECOMP_CPU_H
#define POUND_RECOMP_CPU_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union
{
    uint8_t  b[16];
    uint16_t h[8];
    uint32_t s[4];
    uint64_t d[2];
    float    f32[4];
    double   f64[2];
} vreg_t;

typedef struct cpu
{
    uint64_t x[31]; // x0..x30 (x30 = link register)
    uint64_t sp;
    uint64_t pc;    // only meaningful at traps/dispatch
    uint64_t tpidr; // TPIDR_EL0
    uint8_t  n, z, cf, v;
    uint32_t fpcr, fpsr;
    vreg_t   q[32];
    uint64_t tpidrro;   // TPIDRRO_EL0 (thread local region, read-only to EL0)
    uint64_t excl_addr; // exclusive monitor (LDXR/STXR emulation)
    uint64_t excl_val;
    uint32_t excl_size;
    uint32_t pad_;
    void    *host;      // owned by the host runtime (current thread object)
    void    *hle_user;  // owned by the host runtime (HLE call context)
} cpu_t;

typedef void (*recomp_fn_t)(cpu_t *c);

// Provided by the host runtime.
extern uint8_t *recomp_mem_base;
void            recomp_dispatch(cpu_t *c, uint64_t target);              // call to an arbitrary address
void            recomp_trap(cpu_t *c, uint64_t pc, const char *reason); // never returns
void            recomp_svc(cpu_t *c, uint32_t imm);
// Continues the current guest call at `pc` in the fallback interpreter until it
// returns to `lr` (the function's return address), then returns to the caller.
void            recomp_resume(cpu_t *c, uint64_t pc, uint64_t lr);
uint64_t        recomp_cntpct(void);
uint64_t        recomp_cntfrq(void);

#ifndef RECOMP_CHECK_ADDR
#define RECOMP_CHECK_ADDR(a, n) ((void)0)
#endif

#define R_HOST(a, n) (RECOMP_CHECK_ADDR((a), (n)), (void *)(recomp_mem_base + (uint64_t)(a)))

static inline uint8_t  rd8(uint64_t a)  { uint8_t v;  memcpy(&v, R_HOST(a, 1), 1); return v; }
static inline uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, R_HOST(a, 2), 2); return v; }
static inline uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, R_HOST(a, 4), 4); return v; }
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, R_HOST(a, 8), 8); return v; }
static inline void     wr8(uint64_t a, uint8_t v)   { memcpy(R_HOST(a, 1), &v, 1); }
static inline void     wr16(uint64_t a, uint16_t v) { memcpy(R_HOST(a, 2), &v, 2); }
static inline void     wr32(uint64_t a, uint32_t v) { memcpy(R_HOST(a, 4), &v, 4); }
static inline void     wr64(uint64_t a, uint64_t v) { memcpy(R_HOST(a, 8), &v, 8); }
static inline void     rdv(uint64_t a, vreg_t *r, int n) { memset(r, 0, 16); memcpy(r, R_HOST(a, n), n); }
static inline void     wrv(uint64_t a, const vreg_t *r, int n) { memcpy(R_HOST(a, n), r, n); }

// ---- flags ---------------------------------------------------------------

static inline uint64_t add64f(cpu_t *c, uint64_t a, uint64_t b, unsigned carry)
{
    unsigned __int128 u = (unsigned __int128)a + b + carry;
    uint64_t          r = (uint64_t)u;
    c->n  = (uint8_t)(r >> 63);
    c->z  = (r == 0);
    c->cf = (uint8_t)(u >> 64);
    c->v  = (uint8_t)((~(a ^ b) & (a ^ r)) >> 63);
    return r;
}

static inline uint32_t add32f(cpu_t *c, uint32_t a, uint32_t b, unsigned carry)
{
    uint64_t u = (uint64_t)a + b + carry;
    uint32_t r = (uint32_t)u;
    c->n  = (uint8_t)(r >> 31);
    c->z  = (r == 0);
    c->cf = (uint8_t)(u >> 32);
    c->v  = (uint8_t)((~(a ^ b) & (a ^ r)) >> 31);
    return r;
}

static inline uint64_t logic64f(cpu_t *c, uint64_t r)
{
    c->n = (uint8_t)(r >> 63); c->z = (r == 0); c->cf = 0; c->v = 0;
    return r;
}

static inline uint32_t logic32f(cpu_t *c, uint32_t r)
{
    c->n = (uint8_t)(r >> 31); c->z = (r == 0); c->cf = 0; c->v = 0;
    return r;
}

static inline void setnzcv(cpu_t *c, unsigned nzcv)
{
    c->n = (nzcv >> 3) & 1; c->z = (nzcv >> 2) & 1; c->cf = (nzcv >> 1) & 1; c->v = nzcv & 1;
}

#define C_EQ (c->z)
#define C_NE (!c->z)
#define C_HS (c->cf)
#define C_LO (!c->cf)
#define C_MI (c->n)
#define C_PL (!c->n)
#define C_VS (c->v)
#define C_VC (!c->v)
#define C_HI (c->cf && !c->z)
#define C_LS (!c->cf || c->z)
#define C_GE (c->n == c->v)
#define C_LT (c->n != c->v)
#define C_GT (!c->z && c->n == c->v)
#define C_LE (c->z || c->n != c->v)
#define C_AL (1)

// ---- bit helpers ---------------------------------------------------------

static inline uint64_t ror64(uint64_t x, unsigned s) { s &= 63; return s ? (x >> s) | (x << (64 - s)) : x; }
static inline uint32_t ror32(uint32_t x, unsigned s) { s &= 31; return s ? (x >> s) | (x << (32 - s)) : x; }
static inline uint64_t asr64(uint64_t x, unsigned s) { return (uint64_t)((int64_t)x >> (s & 63)); }
static inline uint32_t asr32(uint32_t x, unsigned s) { return (uint32_t)((int32_t)x >> (s & 31)); }
static inline uint64_t clz64(uint64_t x) { return x ? (uint64_t)__builtin_clzll(x) : 64; }
static inline uint32_t clz32(uint32_t x) { return x ? (uint32_t)__builtin_clz(x) : 32; }
// CLS(x) = number of bits below the sign bit that equal it.
static inline uint64_t cls64(uint64_t x) { return clz64((x ^ (x << 1)) | 1); }
static inline uint32_t cls32(uint32_t x) { return clz32((uint32_t)(x ^ (x << 1)) | 1); }
static inline uint64_t rbit64(uint64_t x)
{
    x = ((x >> 1) & 0x5555555555555555ull) | ((x & 0x5555555555555555ull) << 1);
    x = ((x >> 2) & 0x3333333333333333ull) | ((x & 0x3333333333333333ull) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0Full) | ((x & 0x0F0F0F0F0F0F0F0Full) << 4);
    return __builtin_bswap64(x);
}
static inline uint32_t rbit32(uint32_t x) { return (uint32_t)(rbit64(x) >> 32); }
static inline uint64_t rev16_64(uint64_t x) { return ((x >> 8) & 0x00FF00FF00FF00FFull) | ((x & 0x00FF00FF00FF00FFull) << 8); }
static inline uint64_t rev32_64(uint64_t x) { uint64_t r = __builtin_bswap64(x); return (r >> 32) | (r << 32); }

static inline int64_t sdiv64(int64_t a, int64_t b) { if (b == 0) return 0; if (a == INT64_MIN && b == -1) return a; return a / b; }
static inline int32_t sdiv32(int32_t a, int32_t b) { if (b == 0) return 0; if (a == INT32_MIN && b == -1) return a; return a / b; }
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }

static inline uint64_t umulh64(uint64_t a, uint64_t b) { return (uint64_t)(((unsigned __int128)a * b) >> 64); }
static inline uint64_t smulh64(uint64_t a, uint64_t b) { return (uint64_t)(((__int128)(int64_t)a * (int64_t)b) >> 64); }

// Bitfield move (UBFM/SBFM/BFM) on `bits`-wide registers.
static inline uint64_t ubfm(uint64_t src, unsigned immr, unsigned imms, unsigned bits)
{
    uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    src &= mask;
    if (imms >= immr) {
        unsigned w = imms - immr + 1;
        uint64_t f = (src >> immr) & (w == 64 ? ~0ull : ((1ull << w) - 1));
        return f;
    }
    unsigned w = imms + 1;
    uint64_t f = src & (w == 64 ? ~0ull : ((1ull << w) - 1));
    return (f << (bits - immr)) & mask;
}
static inline uint64_t sbfm(uint64_t src, unsigned immr, unsigned imms, unsigned bits)
{
    uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    uint64_t r    = ubfm(src, immr, imms, bits);
    unsigned top  = imms >= immr ? imms - immr : bits - immr + imms; // position of the copied sign bit
    if ((r >> top) & 1)
        r |= (~0ull << top);
    return r & mask;
}
static inline uint64_t bfm(uint64_t dst, uint64_t src, unsigned immr, unsigned imms, unsigned bits)
{
    uint64_t mask = bits == 64 ? ~0ull : ((1ull << bits) - 1);
    uint64_t fmask;
    uint64_t val;
    if (imms >= immr) {
        unsigned w = imms - immr + 1;
        fmask = (w == 64 ? ~0ull : ((1ull << w) - 1));
        val   = (src >> immr) & fmask;
    } else {
        unsigned w     = imms + 1;
        unsigned shift = bits - immr;
        fmask = (w == 64 ? ~0ull : ((1ull << w) - 1)) << shift;
        val   = (src << shift) & fmask;
    }
    return ((dst & ~fmask) | val) & mask;
}

// CRC32 (polynomial 0x04C11DB7 reflected) and CRC32C (0x1EDC6F41 reflected).
static inline uint32_t crc32_upd(uint32_t crc, uint64_t data, unsigned bytes, uint32_t poly)
{
    for (unsigned i = 0; i < bytes * 8; ++i) {
        uint32_t bit = (uint32_t)((data >> i) & 1) ^ (crc & 1);
        crc >>= 1;
        if (bit)
            crc ^= poly;
    }
    return crc;
}

// ---- floating point with AArch64 NaN semantics ---------------------------
//
// Host (x86) and guest differ on NaNs: AArch64 generates the positive default NaN
// (0x7FC00000) where x86 produces 0xFFC00000, and the two pick different operands
// when several are NaN. The helpers below follow the Arm ARM (FPProcessNaNs, FPCR.DN).

#define FPCR_DN (1u << 25)

static inline uint32_t f32_bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float    f32_from(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t f64_bits(double f) { uint64_t u; memcpy(&u, &f, 8); return u; }
static inline double   f64_from(uint64_t u) { double f; memcpy(&f, &u, 8); return f; }

static inline int f32_isnan(float f) { return (f32_bits(f) & 0x7FFFFFFFu) > 0x7F800000u; }
static inline int f64_isnan(double f) { return (f64_bits(f) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
static inline int f32_issnan(float f) { return f32_isnan(f) && !(f32_bits(f) & 0x00400000u); }
static inline int f64_issnan(double f) { return f64_isnan(f) && !(f64_bits(f) & 0x0008000000000000ull); }
static inline int f32_isinf(float f) { return (f32_bits(f) & 0x7FFFFFFFu) == 0x7F800000u; }
static inline int f64_isinf(double f) { return (f64_bits(f) & 0x7FFFFFFFFFFFFFFFull) == 0x7FF0000000000000ull; }

#define F32_DNAN f32_from(0x7FC00000u)
#define F64_DNAN f64_from(0x7FF8000000000000ull)

static inline float  f32_quiet(const cpu_t *c, float a)  { return (c->fpcr & FPCR_DN) ? F32_DNAN : f32_from(f32_bits(a) | 0x00400000u); }
static inline double f64_quiet(const cpu_t *c, double a) { return (c->fpcr & FPCR_DN) ? F64_DNAN : f64_from(f64_bits(a) | 0x0008000000000000ull); }

#define RECOMP_FP_OPS(T, S, DN, SQRT, FMA)                                                          \
    static inline T S##_nan2(const cpu_t *c, T a, T b)                                               \
    {                                                                                                \
        if (S##_issnan(a)) return S##_quiet(c, a);                                                   \
        if (S##_issnan(b)) return S##_quiet(c, b);                                                   \
        if (S##_isnan(a))  return S##_quiet(c, a);                                                   \
        return S##_quiet(c, b);                                                                      \
    }                                                                                                \
    static inline T S##_nan3(const cpu_t *c, T a, T b, T d)                                          \
    {                                                                                                \
        if (S##_issnan(a)) return S##_quiet(c, a);                                                   \
        if (S##_issnan(b)) return S##_quiet(c, b);                                                   \
        if (S##_issnan(d)) return S##_quiet(c, d);                                                   \
        if (S##_isnan(a))  return S##_quiet(c, a);                                                   \
        if (S##_isnan(b))  return S##_quiet(c, b);                                                   \
        return S##_quiet(c, d);                                                                      \
    }                                                                                                \
    static inline T S##_gen(T r) { return S##_isnan(r) ? DN : r; }                                  \
    static inline T S##_add(const cpu_t *c, T a, T b) { if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b); return S##_gen(a + b); } \
    static inline T S##_sub(const cpu_t *c, T a, T b) { if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b); return S##_gen(a - b); } \
    static inline T S##_mul(const cpu_t *c, T a, T b) { if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b); return S##_gen(a * b); } \
    static inline T S##_div(const cpu_t *c, T a, T b) { if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b); return S##_gen(a / b); } \
    static inline T S##_neg(T a) { return S##_from(S##_bits(a) ^ ((__typeof__(S##_bits(a)))1 << (sizeof(T) * 8 - 1))); } \
    static inline T S##_abs(T a) { return S##_from(S##_bits(a) & ~((__typeof__(S##_bits(a)))1 << (sizeof(T) * 8 - 1))); } \
    static inline T S##_sqrt(const cpu_t *c, T a) { if (S##_isnan(a)) return S##_quiet(c, a); return S##_gen(SQRT(a)); } \
    static inline T S##_max(const cpu_t *c, T a, T b)                                                \
    {                                                                                                \
        if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b);                                  \
        if (a == 0 && b == 0) return (S##_bits(a) & S##_bits(b) & ((__typeof__(S##_bits(a)))1 << (sizeof(T) * 8 - 1))) ? a : S##_abs(a); \
        return a > b ? a : b;                                                                        \
    }                                                                                                \
    static inline T S##_min(const cpu_t *c, T a, T b)                                                \
    {                                                                                                \
        if (S##_isnan(a) || S##_isnan(b)) return S##_nan2(c, a, b);                                  \
        if (a == 0 && b == 0) return ((S##_bits(a) | S##_bits(b)) & ((__typeof__(S##_bits(a)))1 << (sizeof(T) * 8 - 1))) ? S##_neg(S##_abs(a)) : a; \
        return a < b ? a : b;                                                                        \
    }                                                                                                \
    static inline T S##_maxnm(const cpu_t *c, T a, T b)                                              \
    {                                                                                                \
        if (S##_isnan(a) && !S##_issnan(a) && !S##_isnan(b)) return b;                               \
        if (S##_isnan(b) && !S##_issnan(b) && !S##_isnan(a)) return a;                               \
        return S##_max(c, a, b);                                                                     \
    }                                                                                                \
    static inline T S##_minnm(const cpu_t *c, T a, T b)                                              \
    {                                                                                                \
        if (S##_isnan(a) && !S##_issnan(a) && !S##_isnan(b)) return b;                               \
        if (S##_isnan(b) && !S##_issnan(b) && !S##_isnan(a)) return a;                               \
        return S##_min(c, a, b);                                                                     \
    }                                                                                                \
    /* addend + n * m, NaN priority (addend, n, m) as in FPMulAdd */                                 \
    static inline T S##_muladd(const cpu_t *c, T addend, T n, T m)                                   \
    {                                                                                                \
        int inf_zero = (S##_isinf(n) && m == 0) || (S##_isinf(m) && n == 0);                        \
        if (S##_isnan(addend) && !S##_issnan(addend) && inf_zero) return DN;                         \
        if (S##_isnan(addend) || S##_isnan(n) || S##_isnan(m)) return S##_nan3(c, addend, n, m);     \
        return S##_gen(FMA(n, m, addend));                                                           \
    }                                                                                                \
    static inline void S##_cmp(cpu_t *c, T a, T b)                                                   \
    {                                                                                                \
        if (S##_isnan(a) || S##_isnan(b)) setnzcv(c, 3);                                             \
        else if (a == b) setnzcv(c, 6);                                                              \
        else if (a < b) setnzcv(c, 8);                                                               \
        else setnzcv(c, 2);                                                                          \
    }

#ifdef __cplusplus
}
#endif

#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

RECOMP_FP_OPS(float, f32, F32_DNAN, sqrtf, fmaf)
RECOMP_FP_OPS(double, f64, F64_DNAN, sqrt, fma)

#ifdef __cplusplus
}
#endif

#endif // POUND_RECOMP_CPU_H
