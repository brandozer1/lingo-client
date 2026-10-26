/* fficonfig.h - hand written in place of libffi's configure step (lingos/native, see README). Onsay vendors a small
   part of libffi (MIT) to call functions by their layout. We only ever call, never make closures, so the trampoline
   and executable-memory options are off. */
#define FFI_BUILDING 1
#define HAVE_AS_CFI_PSEUDO_OP 1
#define STDC_HEADERS 1
#define SIZEOF_DOUBLE 8
#define SIZEOF_LONG_DOUBLE 8
#define SIZEOF_SIZE_T 8
#define HAVE_MEMCPY 1
#define FFI_MMAP_EXEC_WRIT 0
#define EH_FRAME_FLAGS "aw"
#ifdef LIBFFI_ASM
#define FFI_HIDDEN(name)
#else
#define FFI_HIDDEN __attribute__ ((visibility ("hidden")))
#endif
