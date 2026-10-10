/*
 * test_sysgen_factory.c - rd vms-025's R1 for the one thing that actually
 * stopped a real-VAX mixed cluster from working on 2026-10-09:
 *
 *     SYSBOOT> SET LOCKDIRWT 1
 *     %SYSGEN-E-NOSUCHP, no such parameter "LOCKDIRWT"
 *
 * ==========================================================================
 * THE HOLE THIS FILE EXISTS TO MAKE UNREPEATABLE
 * ==========================================================================
 * The whole interim mixed-cluster DLM configuration -- the one
 * vms_ldwv_sole_directory() reads and every arm of the mixed-cluster lock
 * manager stands behind -- needs this node at a nonzero LOCKDIRWT and the real
 * VMS members at 0. On a booted OVMX node that was UNREACHABLE: SYSBOOT's SET
 * looked the name up ONLY in the parameter FILE it had just loaded, and the
 * shipped SYS$SYSTEM:OVMXVMSSYS.PAR was authored before LOCKDIRWT existed. So
 * the node joined at weight 0, p. 6-32's all-zero rule gave one vector entry
 * per system, the predicate read FALSE, and a real VAX's $ENQ went unanswered
 * for a reason nothing on the console named.
 *
 * LOCKDIRWT was not alone: QDSKVOTES, TIMVCFAIL, CLUSTER_CREDITS,
 * NISCS_MAX_PKTSZ, MSCP_LOAD, MSCP_SERVE_ALL and DISK_QUORUM are all read by
 * load_cluster_sysgen_params() at boot and all absent from that seed.
 *
 * ==========================================================================
 * WHAT IS REAL HERE
 * ==========================================================================
 *   the TABLE   src/libvms/include/sysgen_factory.h -- the SHIPPING factory
 *               parameter table and the SHIPPING merge, shared by SYSGEN.EXE
 *               and SYSBOOT. Not a copy.
 *   the SEED    distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR -- the
 *               actual binary the bootable image carries, read off disk.
 *   the READER  the list of parameters this test requires is DERIVED by
 *               scanning the shipping text of src/ovmx_init/ovmx_init.c for
 *               its sysgen_read_param()/sysgen_read_string() calls. Add a
 *               parameter to the boot loader and this test demands it; it
 *               cannot drift from the code, because it IS the code.
 *   the WIRING  src/ovmx_init/sysboot.c's call sites, read out of the shipped
 *               text (the glue is PID 1's and does not link into a host unit
 *               test, the same two-proof shape tests/cluster/host uses).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysgen_factory.h"

static unsigned g_checks, g_fails;

static void ck(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		printf("  ok   %s\n", what);
		return;
	}
	g_fails++;
	printf("  FAIL %s\n", what);
}

/* ==========================================================================
 * 0. The shipping sources this test reads
 * ========================================================================== */

static char g_src[400000];

static int read_src(const char *rel)
{
	char path[1024];
	FILE *f;
	size_t n;

	snprintf(path, sizeof(path), "%s/%s", OVMX_REPO_ROOT, rel);
	f = fopen(path, "rb");
	if (f == NULL)
		return -1;
	n = fread(g_src, 1u, sizeof(g_src) - 1u, f);
	fclose(f);
	g_src[n] = '\0';
	return 0;
}

/* ==========================================================================
 * 1. EVERY PARAMETER THE BOOT LOADER READS IS ONE THIS SYSTEM KNOWS
 *
 * Derived, not listed: the needles are the shipping call forms, so the set is
 * whatever ovmx_init.c really reads today.
 * ========================================================================== */

/* Copy the quoted name that follows `*p` (positioned just after the open
 * paren of a sysgen_read_* call) into `out`. Returns 0 on success. */
static int quoted_name_at(const char *p, char *out, size_t cap)
{
	size_t i = 0;

	if (*p != '"')
		return -1;
	p++;
	while (*p != '"' && *p != '\0' && i + 1 < cap)
		out[i++] = *p++;
	out[i] = '\0';
	return (*p == '"' && i > 0) ? 0 : -1;
}

/* Walk every `needle` occurrence in g_src and require the quoted parameter
 * name that follows it to exist in the factory table. Returns how many it
 * checked, so the caller can prove the scan actually found something. */
static unsigned require_all_reads(const char *needle)
{
	const char *p = g_src;
	unsigned seen = 0;
	size_t nl = strlen(needle);

	while ((p = strstr(p, needle)) != NULL) {
		char name[64];
		char what[160];

		p += nl;
		if (quoted_name_at(p, name, sizeof(name)) != 0)
			continue;
		seen++;
		snprintf(what, sizeof(what),
			 "  the boot loader reads %s -- and this system's "
			 "factory table knows it", name);
		ck(sysgen_factory_find(name) != NULL, what);
	}
	return seen;
}

static void every_parameter_the_boot_reads_is_known(void)
{
	unsigned n;

	printf("-- every SYSGEN parameter the cluster boot READS is in the "
	       "factory table --\n");
	if (read_src("src/ovmx_init/ovmx_init.c") != 0) {
		ck(0, "could not read src/ovmx_init/ovmx_init.c");
		return;
	}
	n  = require_all_reads("sysgen_read_param(");
	n += require_all_reads("sysgen_read_string(");
	ck(n >= 10u,
	   "the scan really found the boot loader's parameter reads (not a "
	   "vacuous pass)");
	/* The one that bit the lab, named explicitly so the regression has a
	 * label a human recognises. */
	ck(sysgen_factory_find("LOCKDIRWT") != NULL,
	   "*** LOCKDIRWT is a parameter this system knows -- the knob the "
	   "2026-10-09 lab could not set ***");
}

/* ==========================================================================
 * 2. THE MERGE: A STORE WRITTEN BY AN OLDER SYSTEM STILL EXPOSES THE KNOB
 * ========================================================================== */

/* The pre-FC-P0.10 store, built HERE so it cannot drift: the factory table
 * minus every row added after it was authored. This is the shape the shipped
 * seed had, and the shape every disk written by an older system has. */
static void old_store(struct sysgen_file *ws)
{
	static const char *late[] = {
		"LOCKDIRWT", "QDSKVOTES", "TIMVCFAIL", "CLUSTER_CREDITS",
		"NISCS_MAX_PKTSZ", "MSCP_LOAD", "MSCP_SERVE_ALL",
		"DISK_QUORUM", "OVMX_CLEAN_DEPART"
	};
	uint32_t i, j;

	memset(ws, 0, sizeof(*ws));
	ws->magic = SYSGEN_MAGIC;
	ws->version = SYSGEN_VERSION;
	for (i = 0; i < OVMX_SYSGEN_FACTORY_COUNT; i++) {
		int skip = 0;

		for (j = 0; j < sizeof(late) / sizeof(late[0]); j++) {
			if (strcasecmp(ovmx_sysgen_factory_params[i].name,
				       late[j]) == 0)
				skip = 1;
		}
		if (!skip)
			ws->params[ws->count++] = ovmx_sysgen_factory_params[i];
	}
}

static void the_merge_makes_an_old_store_complete(void)
{
	struct sysgen_file ws;
	uint32_t before, added;

	printf("-- the factory merge: a store written before a parameter "
	       "existed --\n");
	old_store(&ws);
	before = ws.count;

	/* THE REPRODUCER. This is the state SYSBOOT was in on the lab node. */
	ck(!sysgen_file_has(&ws, "LOCKDIRWT"),
	   "*** the stored set has NO LOCKDIRWT row: without the merge, "
	   "SYSBOOT> SET LOCKDIRWT answers %SYSGEN-E-NOSUCHP ***");
	ck(!sysgen_file_has(&ws, "TIMVCFAIL") &&
	   !sysgen_file_has(&ws, "MSCP_LOAD"),
	   "  and it is not the only one");

	added = sysgen_factory_merge(&ws);
	ck(added == 9u && ws.count == before + 9u,
	   "the merge appends exactly the rows the store lacked");
	ck(sysgen_file_has(&ws, "LOCKDIRWT"),
	   "*** and LOCKDIRWT is now in the working set: SHOWable, SETtable, "
	   "and persisted by WRITE ***");
	ck(sysgen_file_has(&ws, "TIMVCFAIL") &&
	   sysgen_file_has(&ws, "MSCP_LOAD") &&
	   sysgen_file_has(&ws, "DISK_QUORUM"),
	   "  along with every other row the loader reads");

	/* Idempotent: a second merge adds nothing. */
	ck(sysgen_factory_merge(&ws) == 0u,
	   "a second merge adds nothing (the union is idempotent)");
}

static void the_merge_never_overwrites_a_stored_value(void)
{
	struct sysgen_file ws;
	uint32_t i;
	int found = 0;

	printf("-- the merge never overwrites what the operator stored --\n");
	old_store(&ws);
	/* The operator set VOTES 7 and wrote it. The factory default is 1. */
	for (i = 0; i < ws.count; i++) {
		if (strcasecmp(ws.params[i].name, "VOTES") == 0) {
			ws.params[i].current = 7u;
			found = 1;
		}
	}
	ck(found, "the stored set carries VOTES");
	(void)sysgen_factory_merge(&ws);
	for (i = 0; i < ws.count; i++) {
		if (strcasecmp(ws.params[i].name, "VOTES") != 0)
			continue;
		ck(ws.params[i].current == 7u,
		   "*** the operator's stored VOTES 7 survives the merge -- the "
		   "file carries VALUES, the table only carries which knobs "
		   "exist ***");
	}
	/* ... and a merged row carries the FACTORY value, which is exactly what
	 * the boot-time reader falls back to for an absent record. So a merge
	 * never changes the running configuration. */
	for (i = 0; i < ws.count; i++) {
		const struct sysgen_param *f;

		if (strcasecmp(ws.params[i].name, "LOCKDIRWT") != 0)
			continue;
		f = sysgen_factory_find("LOCKDIRWT");
		ck(f != NULL && ws.params[i].current == f->default_val &&
		   ws.params[i].current == 0u,
		   "  and a merged LOCKDIRWT reads the factory 0 (design "
		   "D-DLM-1): the merge exposes the knob, it does not turn it");
	}
}

static void the_merge_respects_the_store_bound(void)
{
	struct sysgen_file ws;
	uint32_t i;

	printf("-- the merge respects SYSGEN_MAX_PARAMS --\n");
	memset(&ws, 0, sizeof(ws));
	ws.magic = SYSGEN_MAGIC;
	ws.version = SYSGEN_VERSION;
	ws.count = SYSGEN_MAX_PARAMS;
	for (i = 0; i < SYSGEN_MAX_PARAMS; i++)
		snprintf(ws.params[i].name, sizeof(ws.params[i].name),
			 "FILLER_%u", (unsigned)i);
	ck(sysgen_factory_merge(&ws) == 0u && ws.count == SYSGEN_MAX_PARAMS,
	   "a full store appends nothing rather than overflowing");

	ck(OVMX_SYSGEN_FACTORY_COUNT <= SYSGEN_MAX_PARAMS,
	   "and the factory table itself fits the store (a row that does not "
	   "fit is a row no operator can ever set)");

	/*
	 * THE TABLE SIZE IS A WIRE-VISIBLE NUMBER: SYSBOOT's WRITE prints it
	 * ("%SYSGEN-I-WRITTEN, N parameters written"), and the QEMU boot leg
	 * tests/qemu/test_sysboot_cluster_params_e2e.sh asserts that exact N.
	 * Pinned here so adding a parameter reddens BOTH places together instead
	 * of one of them silently -- that script names this assertion back.
	 */
	ck(OVMX_SYSGEN_FACTORY_COUNT == 40u,
	   "the factory table is 40 rows -- the N SYSBOOT's WRITE prints and "
	   "test_sysboot_cluster_params_e2e.sh asserts");
}

/* ==========================================================================
 * 3. THE REAL SHIPPED SEED, read off disk
 * ========================================================================== */

static void the_shipped_seed_is_complete_after_the_merge(void)
{
	char path[1024];
	struct sysgen_file ws;
	FILE *f;
	uint32_t before, i, missing_pre = 0;

	printf("-- the bootable image's own OVMXVMSSYS.PAR --\n");
	snprintf(path, sizeof(path),
		 "%s/distro/rootfs/vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR",
		 OVMX_REPO_ROOT);
	f = fopen(path, "rb");
	if (f == NULL) {
		ck(0, "could not open the shipped OVMXVMSSYS.PAR seed");
		return;
	}
	if (fread(&ws, sizeof(ws), 1, f) != 1) {
		fclose(f);
		ck(0, "could not read the shipped OVMXVMSSYS.PAR seed");
		return;
	}
	fclose(f);
	ck(ws.magic == SYSGEN_MAGIC && ws.version == SYSGEN_VERSION,
	   "the shipped seed is a valid v2 parameter store");
	if (ws.count > SYSGEN_MAX_PARAMS)
		ws.count = SYSGEN_MAX_PARAMS;
	before = ws.count;

	/* Informational, and the measured number: the lab's own boot printed
	 * "%SYSGEN-I-WRITTEN, 31 parameters written". */
	for (i = 0; i < OVMX_SYSGEN_FACTORY_COUNT; i++) {
		if (!sysgen_file_has(&ws, ovmx_sysgen_factory_params[i].name))
			missing_pre++;
	}
	printf("       seed carries %u rows; %u factory rows are absent from "
	       "it\n", (unsigned)before, (unsigned)missing_pre);

	(void)sysgen_factory_merge(&ws);
	for (i = 0; i < OVMX_SYSGEN_FACTORY_COUNT; i++) {
		char what[160];
		const char *nm = ovmx_sysgen_factory_params[i].name;

		snprintf(what, sizeof(what),
			 "  %s is in the booted working set", nm);
		ck(sysgen_file_has(&ws, nm), what);
	}
	ck(sysgen_file_has(&ws, "LOCKDIRWT"),
	   "*** so a node booted from the shipped image CAN be given a "
	   "LOCKDIRWT at SYSBOOT ***");
}

/* ==========================================================================
 * 4. THE WIRING, read out of the shipped text
 *
 * sysboot.c is PID 1's and does not link into a host unit test; its CALL SITES
 * are what makes the merge reach a booting node, so they are pinned the same
 * way tests/cluster/host pins a non-linkable glue TU.
 * ========================================================================== */

static void has_src(const char *needle, const char *what)
{
	ck(strstr(g_src, needle) != NULL, what);
}

static void sysboot_and_sysgen_use_the_shared_table(void)
{
	printf("-- the call sites, read out of the shipped text --\n");

	if (read_src("src/ovmx_init/sysboot.c") != 0) {
		ck(0, "could not read src/ovmx_init/sysboot.c");
		return;
	}
	has_src("#include \"sysgen_factory.h\"",
		"SYSBOOT builds on the SHARED factory table, not a private "
		"one-row stub");
	has_src("sysgen_factory_load(ws)",
		"... which is also what it loads when there is no parameter "
		"file at all");
	has_src("(void)sysgen_factory_merge(ws);",
		"*** and a LOADED file is MERGED with it, so a parameter the "
		"system knows is never NOSUCHP ***");

	if (read_src("tools/vms_sysgen.c") != 0) {
		ck(0, "could not read tools/vms_sysgen.c");
		return;
	}
	has_src("#include \"sysgen_factory.h\"",
		"SYSGEN.EXE builds on the SAME table (one source, INV-LEDGER)");
	has_src("(void)sysgen_factory_merge(&working_set);",
		"... and USE CURRENT merges it too, so SYSGEN and SYSBOOT "
		"cannot disagree about which parameters exist");
}

int main(void)
{
	printf("=== test_sysgen_factory (rd vms-025: the SYSGEN parameter "
	       "table is the SYSTEM's, not the file's) ===\n");

	every_parameter_the_boot_reads_is_known();
	the_merge_makes_an_old_store_complete();
	the_merge_never_overwrites_a_stored_value();
	the_merge_respects_the_store_bound();
	the_shipped_seed_is_complete_after_the_merge();
	sysboot_and_sysgen_use_the_shared_table();

	printf("test_sysgen_factory: %u checks, %u failures\n",
	       g_checks, g_fails);
	return g_fails == 0 ? 0 : 1;
}
