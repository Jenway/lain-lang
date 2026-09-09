#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
static uintptr_t base, idx, scale, offset;
#define L1_load8(p) (*(uint8_t*)(uintptr_t)(p))
#define L1_load32(p) (*(uint32_t*)(uintptr_t)(p))
#define L1_load64(p) (*(uint64_t*)(uintptr_t)(p))
#define L1_store8(p,v) (*(uint8_t*)(uintptr_t)(p)=(uint8_t)(v))
#define L1_store32(p,v) (*(uint32_t*)(uintptr_t)(p)=(uint32_t)(v))
#define L1_store64(p,v) (*(uint64_t*)(uintptr_t)(p)=(uint64_t)(v))
#define L1_lea(base,idx,scale,offset) ((uintptr_t)(base)+(uintptr_t)(idx)*(uintptr_t)(scale)+(intptr_t)(offset))
extern uintptr_t bootstrap_allocate_pages(uint64_t);
extern void bootstrap_artifact_begin(void);
extern void bootstrap_artifact_write_byte(uint32_t);
extern void bootstrap_artifact_write_span(uintptr_t, uint64_t);
extern void bootstrap_artifact_finish(void);
extern uint64_t bootstrap_source_count(void);
extern uint64_t bootstrap_source_length(uint64_t);
extern uintptr_t bootstrap_source_data(uint64_t);
extern uintptr_t bootstrap_source_path_data(uint64_t);
extern uint64_t bootstrap_source_path_length(uint64_t);
extern void bootstrap_copy_bytes(uintptr_t, uintptr_t, uint64_t);
#define L1_add(a,b) ((a)+(b))
#define L1_sub(a,b) ((a)-(b))
#define L1_mul(a,b) ((a)*(b))
#define L1_eq(a,b) ((a)==(b))
#define L1_lt(a,b) ((a)<(b))
#define L1_le(a,b) ((a)<=(b))
#define L1_and(a,b) ((a)&&(b))
#define L1_or(a,b) ((a)||(b))
#define L1_ne(a,b) ((a)!=(b))
#define L1_gt(a,b) ((a)>(b))
#define L1_ge(a,b) ((a)>=(b))
#define L1_udiv(a,b) ((a)/(b))
#define L1_urem(a,b) ((a)%(b))
uint32_t helper();
int lainc_entry();
uint32_t helper(void) {
  return  42;
}
int lainc_entry(void) {
  return   helper();
}
