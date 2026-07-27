#!/bin/sh
# mk_decc_shr.sh — build recipe for DECC$SHR.EXE, the OVMX C run-time library
# shareable image (bead vms-61f.1, pillar vms-ade).
#
# DECC$SHR.EXE is the whole musl `libc.a` linked by LINK.EXE (the OVMX VMS-native
# linker — NO ld/ld.so) into a single OVMX shareable image (ELF ET_DYN) that
# carries a `.vms$sv` symbol vector exposing the C run-time universals
# (malloc/free/memcpy/strlen/printf/...) as PROCEDURE universals. Consumers bind
# to it through the symbol vector at activation, exactly as VMS images bind to
# DECC$SHR on OpenVMS.
#
# Composition (VMS-native, whole-archive, in-process — no `ld -r`):
#   libc.a   — musl C library (all 1345 members pulled)
#   libgcc.a — the compiler runtime: soft-float / long-double / complex builtins
#              (__addtf3, __trunctfdf2, __fixtfsi, __multc3, ...). musl's stdio /
#              printf reference these internally; they are resolved WITHIN the
#              image and kept INTERNAL — a C-RTL consumer never calls them, so
#              they are NOT exported as universals.
#
# Not exported / linker-defined (resolve to 0, the correct empty value): musl's
# libc.a + libgcc.a carry NO .init_array/.fini_array sections and no dynamic
# section, so __init_array_start/end, __fini_array_start/end and _DYNAMIC are
# empty/null — LINK.EXE resolves these WEAK-undefined symbols to 0 (standard ELF
# semantics). The image therefore links with ZERO deferred externals.
#
# THIS RECIPE PRODUCES THE SHAREABLE ONLY. Runtime init (musl bootstrap, thread-
# pointer/TCB setup, __libc_start_main, running constructors) is vms-61f.2.
#
# Usage:  mk_decc_shr.sh <LINK.EXE> <out-DECC$SHR.EXE> [libc.a] [libgcc.a]
# Env:    GSMATCH (default LEQUAL,1,0)
#
# Must run where an aarch64 musl `libc.a` + `libgcc.a` exist (the arm64 musl
# container — see CLAUDE.md test loop). aarch64-only for now.
set -e

LINK_EXE=${1:?usage: mk_decc_shr.sh <LINK.EXE> <out> [libc.a] [libgcc.a]}
OUT=${2:?usage: mk_decc_shr.sh <LINK.EXE> <out> [libc.a] [libgcc.a]}
LIBC=${3:-/usr/lib/libc.a}
LIBGCC=${4:-$(gcc -print-libgcc-file-name)}
GSMATCH=${GSMATCH:-LEQUAL,1,0}

[ -f "$LIBC" ]   || { echo "mk_decc_shr: libc.a not found: $LIBC (need the arm64 musl container)"; exit 1; }
[ -f "$LIBGCC" ] || { echo "mk_decc_shr: libgcc.a not found: $LIBGCC"; exit 1; }

# The C run-time universals DECC$SHR exports. Every name is defined by musl's
# libc.a; each becomes a PROCEDURE universal in .vms$sv. (The stdin/stdout/stderr
# DATA universals are appended at the END for vms-b65.2 — musl statically
# initializes these `FILE *const` objects at image build, so a consumer's GOT
# data-import binds to the real object address via vms-e65's =DATA path. `environ`
# and other runtime-populated DATA objects still wait on vms-61f.2 runtime init.)
#
# __init_libc is musl's C-RTL bootstrap (programs the thread pointer, builds the
# TCB/TLS, sets the stack guard, makes malloc usable). It is not a consumer-
# callable universal, but IMGACT's activation bootstrap (vms-61f.2) resolves it
# BY NAME from the .vms$sv name blob to drive runtime init before transferring
# control — so the production vector MUST export it. (vms-36a)
#
# The pthread_{mutex,cond}_* + raise/sigaction/sigemptyset + getpid universals at
# the END are APPENDED (never inserted): the symbol vector is append-only, so
# existing consumers' bound vector indices stay valid — a GSMATCH-compatible
# additive change (LEQUAL). They let OVMX libs that migrate onto DECC$SHR bind
# pthread / signal / getpid (the b65 lib-migration chain; the import-binding path
# itself is vms-e65). getpid was appended for vms-b65.1 (vmsprocess's
# vms_get_current_process needs it). (calloc/close are already exported above, so
# they are not re-listed here.) pthread_once/toupper/ttyname were appended for
# vms-b65.3 (vmslnm's lnm_get_manager singleton + lnm_setup_defaults terminal
# logicals). All appended at the END — indices for prior consumers are unchanged.
# __errno_location + the ctype (isalnum/tolower) + strncasecmp + the dirent
# (opendir/readdir/closedir) + stat/realpath/unlink universals at the very END
# were appended for vms-b65.4 (vmsfs's filespec translation, ODS-2 version scan,
# case-insensitive path resolution, and device-table ops need them). Appended at
# the END — indices for prior consumers are unchanged (GSMATCH LEQUAL-compatible).
#
# The FINAL block (the libm transcendentals + the process/time/glob/pwd libc
# universals, and the stdin/stdout/stderr DATA universals) was appended for
# vms-b65.2 — the LIBVMS$SHR migration, the largest OVMX runtime. libvms's
# system services + lib$/str$/mth$/ots$ RTL import them: mth_routines.c /
# sys_float.c pull the libm transcendentals (sin/cos/tan/exp/log*/pow/sqrt/
# floor/ceil/round/fmod/fabs* + f-variants), sys_process.c / lib_misc.c the
# fork/exec*/kill/pause/*priority/get{uid,gid,pwuid,rusage} process controls,
# lib_datetime.c the time/*time_r/timegm/timer_* clocks, sys_time.c
# clock_gettime, sys_uring.c syscall/mmap/munmap, lib_logical.c glob/globfree,
# lib_output.c fwrite(...,stdout)/fprintf(stderr,...) (the stdin/stdout/stderr
# DATA universals — musl statically initializes these `FILE *const` objects, so
# a consumer's GOT data-import binds to DECC$SHR's real object, vms-e65's
# =DATA path, link.c:400). The rest (dup/dup2/mkdir/rename/socketpair/statvfs/
# sysconf/uname/strtok_r/fscanf/freopen/gmtime_r/localtime*/__libc_current_sigrtmin/
# _exit) are scattered across the RTL. ALL appended at the END (GSMATCH LEQUAL-
# compatible): prior consumers' bound vector indices are unchanged. DATA
# universals sit at the very end; only stdin/stdout/stderr are cross-image DATA
# imports for libvms (every other import is a PROCEDURE) — no producer-pointer
# ABS64 case (vms-212).
VEC="\
__init_libc=PROCEDURE,\
malloc=PROCEDURE,free=PROCEDURE,calloc=PROCEDURE,realloc=PROCEDURE,\
aligned_alloc=PROCEDURE,posix_memalign=PROCEDURE,\
memcpy=PROCEDURE,memmove=PROCEDURE,memset=PROCEDURE,memcmp=PROCEDURE,memchr=PROCEDURE,\
strlen=PROCEDURE,strnlen=PROCEDURE,strcmp=PROCEDURE,strncmp=PROCEDURE,strcasecmp=PROCEDURE,\
strcpy=PROCEDURE,strncpy=PROCEDURE,strcat=PROCEDURE,strncat=PROCEDURE,strdup=PROCEDURE,\
strchr=PROCEDURE,strrchr=PROCEDURE,strstr=PROCEDURE,strtok=PROCEDURE,\
strtol=PROCEDURE,strtoul=PROCEDURE,strtod=PROCEDURE,atoi=PROCEDURE,atol=PROCEDURE,\
snprintf=PROCEDURE,vsnprintf=PROCEDURE,sprintf=PROCEDURE,vsprintf=PROCEDURE,\
printf=PROCEDURE,fprintf=PROCEDURE,vfprintf=PROCEDURE,sscanf=PROCEDURE,\
puts=PROCEDURE,putchar=PROCEDURE,fputs=PROCEDURE,fputc=PROCEDURE,\
fwrite=PROCEDURE,fread=PROCEDURE,fopen=PROCEDURE,fclose=PROCEDURE,fflush=PROCEDURE,\
fseek=PROCEDURE,ftell=PROCEDURE,fgets=PROCEDURE,fgetc=PROCEDURE,getchar=PROCEDURE,\
perror=PROCEDURE,\
qsort=PROCEDURE,bsearch=PROCEDURE,abs=PROCEDURE,labs=PROCEDURE,\
exit=PROCEDURE,abort=PROCEDURE,atexit=PROCEDURE,getenv=PROCEDURE,setenv=PROCEDURE,\
open=PROCEDURE,close=PROCEDURE,read=PROCEDURE,write=PROCEDURE,lseek=PROCEDURE,\
\
pthread_mutex_init=PROCEDURE,pthread_mutex_lock=PROCEDURE,\
pthread_mutex_unlock=PROCEDURE,pthread_mutex_destroy=PROCEDURE,\
pthread_cond_init=PROCEDURE,pthread_cond_wait=PROCEDURE,\
pthread_cond_broadcast=PROCEDURE,pthread_cond_destroy=PROCEDURE,\
raise=PROCEDURE,sigaction=PROCEDURE,sigemptyset=PROCEDURE,\
getpid=PROCEDURE,\
pthread_once=PROCEDURE,toupper=PROCEDURE,ttyname=PROCEDURE,\
__errno_location=PROCEDURE,isalnum=PROCEDURE,tolower=PROCEDURE,\
strncasecmp=PROCEDURE,opendir=PROCEDURE,readdir=PROCEDURE,closedir=PROCEDURE,\
stat=PROCEDURE,realpath=PROCEDURE,unlink=PROCEDURE,\
\
acos=PROCEDURE,acosf=PROCEDURE,asin=PROCEDURE,asinf=PROCEDURE,\
atan=PROCEDURE,atan2=PROCEDURE,atan2f=PROCEDURE,atanf=PROCEDURE,\
ceil=PROCEDURE,cos=PROCEDURE,cosf=PROCEDURE,cosh=PROCEDURE,\
exp=PROCEDURE,expf=PROCEDURE,fabs=PROCEDURE,fabsf=PROCEDURE,\
floor=PROCEDURE,fmod=PROCEDURE,log=PROCEDURE,log10=PROCEDURE,\
log10f=PROCEDURE,log2=PROCEDURE,logf=PROCEDURE,pow=PROCEDURE,\
round=PROCEDURE,sin=PROCEDURE,sinf=PROCEDURE,sinh=PROCEDURE,\
sqrt=PROCEDURE,sqrtf=PROCEDURE,tan=PROCEDURE,tanf=PROCEDURE,tanh=PROCEDURE,\
__libc_current_sigrtmin=PROCEDURE,_exit=PROCEDURE,clock_gettime=PROCEDURE,\
dup=PROCEDURE,dup2=PROCEDURE,execl=PROCEDURE,execv=PROCEDURE,fork=PROCEDURE,\
freopen=PROCEDURE,fscanf=PROCEDURE,getgid=PROCEDURE,getpriority=PROCEDURE,\
getpwuid=PROCEDURE,getrusage=PROCEDURE,getuid=PROCEDURE,glob=PROCEDURE,\
globfree=PROCEDURE,gmtime_r=PROCEDURE,kill=PROCEDURE,localtime=PROCEDURE,\
localtime_r=PROCEDURE,mkdir=PROCEDURE,mmap=PROCEDURE,munmap=PROCEDURE,\
pause=PROCEDURE,rename=PROCEDURE,setpriority=PROCEDURE,socketpair=PROCEDURE,\
statvfs=PROCEDURE,strtok_r=PROCEDURE,syscall=PROCEDURE,sysconf=PROCEDURE,\
time=PROCEDURE,timegm=PROCEDURE,timer_create=PROCEDURE,timer_delete=PROCEDURE,\
timer_settime=PROCEDURE,uname=PROCEDURE,waitpid=PROCEDURE,\
\
stdin=DATA,stdout=DATA,stderr=DATA"

echo "mk_decc_shr: LINK.EXE=$LINK_EXE"
echo "mk_decc_shr: libc.a=$LIBC  libgcc.a=$LIBGCC  GSMATCH=$GSMATCH"

# Whole-archive, strict (NO --allow-undefined): a complete C-RTL shareable must
# link with zero deferred externals. libc.a first so its strong defs win.
"$LINK_EXE" --shareable \
    --symbol-vector "$VEC" \
    --gsmatch "$GSMATCH" \
    -o "$OUT" "$LIBC" "$LIBGCC"

echo "mk_decc_shr: created $OUT"
