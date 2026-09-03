// SPDX-License-Identifier: GPL-2.0
/*
 * cluster_fixture.c - clean-room specimen loader (FC-P0.6).
 *
 * See cluster_fixture.h for the file format and the provenance rules this
 * enforces. Host test support only; never linked into the executive.
 */

#include "cluster_fixture.h"
#include "cluster_sha256.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "%OVMX-CLUSTER-SPECIMEN-1"

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
	va_list ap;

	if (err && errlen) {
		va_start(ap, fmt);
		vsnprintf(err, errlen, fmt, ap);
		va_end(ap);
	}
	return -1;
}

static char *trim(char *s)
{
	char *end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
			   end[-1] == ' ' || end[-1] == '\t'))
		end--;
	*end = '\0';
	return s;
}

/* Strip a '#' or ';' comment; the format has no escaping and needs none. */
static void strip_comment(char *s)
{
	char *h = strpbrk(s, "#;");

	if (h)
		*h = '\0';
}

static void copy_field(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);

	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static const char *basename_of(const char *p)
{
	const char *slash = strrchr(p, '/');

	return slash ? slash + 1 : p;
}

const char *vms_fixture_origin_name(int origin)
{
	switch (origin) {
	case VMS_FIXTURE_ORIGIN_CAPTURE:   return "capture";
	case VMS_FIXTURE_ORIGIN_SPEC:      return "spec-composed";
	case VMS_FIXTURE_ORIGIN_SYNTHETIC: return "synthetic";
	}
	return "?";
}

static int parse_origin(const char *v, int *out)
{
	if (!strcmp(v, "capture"))
		*out = VMS_FIXTURE_ORIGIN_CAPTURE;
	else if (!strcmp(v, "spec-composed"))
		*out = VMS_FIXTURE_ORIGIN_SPEC;
	else if (!strcmp(v, "synthetic"))
		*out = VMS_FIXTURE_ORIGIN_SYNTHETIC;
	else
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ *
 * The clean-room manifest gate
 * ------------------------------------------------------------------ */

int vms_fixture_capture_in_manifest(const char *manifest_path,
				    const char *capture,
				    char *err, size_t errlen)
{
	char line[512];
	FILE *f;
	int hit = 0;

	if (!capture || !*capture)
		return fail(err, errlen, "empty capture name");
	f = fopen(manifest_path, "r");
	if (!f)
		return fail(err, errlen, "cannot open clean-room manifest %s",
			    manifest_path);
	while (fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');
		char *path;

		if (!sp)
			continue;
		path = trim(sp);
		if (!strcmp(basename_of(path), capture)) {
			hit = 1;
			break;
		}
	}
	fclose(f);
	if (!hit)
		return fail(err, errlen,
			    "capture '%s' is not in the clean-room manifest %s",
			    capture, basename_of(manifest_path));
	return 0;
}

/* ------------------------------------------------------------------ *
 * Header parsing
 * ------------------------------------------------------------------ */

struct parse_state {
	struct vms_fixture *fx;
	int  saw_magic;
	int  saw_bytes;
	int  saw_origin;
	int  saw_wire_len;
	int  saw_sha;
	uint32_t cursor;   /* current @offset for the bytes section */
	int  cursor_set;
};

static int apply_header_kv(struct parse_state *st, const char *key,
			   const char *val, char *err, size_t errlen)
{
	struct vms_fixture *fx = st->fx;

	if (!strcmp(key, "name")) {
		copy_field(fx->name, sizeof(fx->name), val);
	} else if (!strcmp(key, "class")) {
		copy_field(fx->class_name, sizeof(fx->class_name), val);
	} else if (!strcmp(key, "origin")) {
		if (parse_origin(val, &fx->origin) != 0)
			return fail(err, errlen, "unknown origin '%s'", val);
		st->saw_origin = 1;
	} else if (!strcmp(key, "spec")) {
		copy_field(fx->spec, sizeof(fx->spec), val);
	} else if (!strcmp(key, "capture")) {
		copy_field(fx->capture, sizeof(fx->capture), val);
	} else if (!strcmp(key, "frame")) {
		fx->frame_index = strtol(val, NULL, 10);
	} else if (!strcmp(key, "wire-len")) {
		long n = strtol(val, NULL, 10);

		if (n <= 0 || (unsigned long)n > VMS_FIXTURE_MAX_WIRE)
			return fail(err, errlen, "wire-len %ld out of range", n);
		fx->wire_len = (uint32_t)n;
		st->saw_wire_len = 1;
	} else if (!strcmp(key, "sha256")) {
		if (strlen(val) != 64)
			return fail(err, errlen, "sha256 must be 64 hex chars");
		copy_field(fx->sha256, sizeof(fx->sha256), val);
		st->saw_sha = 1;
	} else {
		return fail(err, errlen, "unknown header key '%s'", key);
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Byte-section parsing
 * ------------------------------------------------------------------ */

static int span_add(struct vms_fixture *fx, uint32_t off, uint32_t len,
		    char *err, size_t errlen)
{
	unsigned i;

	if (len == 0)
		return 0;
	if (fx->n_cited >= VMS_FIXTURE_MAX_SPANS)
		return fail(err, errlen, "too many cited spans");
	for (i = 0; i < fx->n_cited; i++) {
		uint32_t a0 = fx->cited[i].off, a1 = a0 + fx->cited[i].len;

		if (off < a1 && a0 < off + len)
			return fail(err, errlen,
				    "cited span @%u+%u overlaps @%u+%u",
				    off, len, a0, fx->cited[i].len);
	}
	fx->cited[fx->n_cited].off = off;
	fx->cited[fx->n_cited].len = len;
	fx->n_cited++;
	return 0;
}

static int parse_hex_byte(const char *tok, unsigned *out)
{
	char *end;
	unsigned long v;

	if (strlen(tok) != 2)
		return -1;
	v = strtoul(tok, &end, 16);
	if (*end != '\0')
		return -1;
	*out = (unsigned)v;
	return 0;
}

/* One line of the %bytes section: an optional @offset then hex byte tokens. */
static int apply_bytes_line(struct parse_state *st, char *line,
			    char *err, size_t errlen)
{
	struct vms_fixture *fx = st->fx;
	uint32_t start;
	uint32_t n = 0;
	char *tok;

	tok = strtok(line, " \t");
	if (tok && tok[0] == '@') {
		char *end;
		unsigned long off = strtoul(tok + 1, &end, 10);

		if (*end != '\0')
			return fail(err, errlen, "bad @offset '%s'", tok);
		st->cursor = (uint32_t)off;
		st->cursor_set = 1;
		tok = strtok(NULL, " \t");
	}
	if (!st->cursor_set)
		return fail(err, errlen, "byte line before any @offset");
	start = st->cursor;
	for (; tok; tok = strtok(NULL, " \t")) {
		unsigned byte;

		if (parse_hex_byte(tok, &byte) != 0)
			return fail(err, errlen, "bad hex byte '%s'", tok);
		if (st->cursor >= fx->wire_len)
			return fail(err, errlen,
				    "byte at offset %u exceeds wire-len %u",
				    st->cursor, fx->wire_len);
		fx->bytes[st->cursor] = (uint8_t)byte;
		st->cursor++;
		n++;
	}
	return span_add(fx, start, n, err, errlen);
}

/* ------------------------------------------------------------------ *
 * Post-parse validation
 * ------------------------------------------------------------------ */

static int validate_required(const struct parse_state *st,
			     char *err, size_t errlen)
{
	const struct vms_fixture *fx = st->fx;

	if (!st->saw_magic)
		return fail(err, errlen, "missing " MAGIC " magic line");
	if (!fx->name[0])
		return fail(err, errlen, "missing name:");
	if (!fx->class_name[0])
		return fail(err, errlen, "missing class:");
	if (!st->saw_origin)
		return fail(err, errlen, "missing origin:");
	if (!st->saw_wire_len)
		return fail(err, errlen, "missing wire-len:");
	if (!st->saw_sha)
		return fail(err, errlen, "missing sha256:");
	if (!st->saw_bytes)
		return fail(err, errlen, "missing %%bytes section");
	return 0;
}

/*
 * Provenance rules, per origin. These are the honesty teeth: a specimen
 * cannot claim to be capture-derived without naming a manifest-hashed
 * capture, cannot be spec-composed without a spec cite, and a synthetic
 * negative control cannot masquerade as a real frame class.
 */
static int validate_origin(const struct vms_fixture *fx,
			   const char *manifest_path, char *err, size_t errlen)
{
	switch (fx->origin) {
	case VMS_FIXTURE_ORIGIN_CAPTURE:
		if (!fx->capture[0])
			return fail(err, errlen,
				    "origin capture requires capture:");
		return vms_fixture_capture_in_manifest(manifest_path,
						       fx->capture, err, errlen);
	case VMS_FIXTURE_ORIGIN_SPEC:
		if (!fx->spec[0])
			return fail(err, errlen,
				    "origin spec-composed requires spec:");
		if (!fx->capture[0])
			return fail(err, errlen,
				    "origin spec-composed requires the capture: the spec cites");
		return vms_fixture_capture_in_manifest(manifest_path,
						       fx->capture, err, errlen);
	case VMS_FIXTURE_ORIGIN_SYNTHETIC:
		if (strcmp(fx->class_name, "unknown") != 0)
			return fail(err, errlen,
				    "synthetic specimens may only carry class 'unknown', not '%s'",
				    fx->class_name);
		if (fx->capture[0])
			return fail(err, errlen,
				    "synthetic specimen must not name a capture");
		return 0;
	}
	return fail(err, errlen, "bad origin");
}

static int validate_digest(const struct vms_fixture *fx,
			   char *err, size_t errlen)
{
	char hex[65];

	cluster_sha256_hex(fx->bytes, fx->wire_len, hex);
	if (strcmp(hex, fx->sha256) != 0)
		return fail(err, errlen,
			    "sha256 mismatch: declared %s, assembled %s",
			    fx->sha256, hex);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Entry point
 * ------------------------------------------------------------------ */

static int load_lines(FILE *f, struct parse_state *st, char *err, size_t errlen)
{
	char raw[1024];

	while (fgets(raw, sizeof(raw), f)) {
		char *line;

		strip_comment(raw);
		line = trim(raw);
		if (!*line)
			continue;
		if (!st->saw_magic) {
			if (strcmp(line, MAGIC) != 0)
				return fail(err, errlen,
					    "first line is not " MAGIC);
			st->saw_magic = 1;
			continue;
		}
		if (!strcmp(line, "%bytes")) {
			if (!st->saw_wire_len)
				return fail(err, errlen,
					    "%%bytes before wire-len:");
			st->saw_bytes = 1;
			continue;
		}
		if (st->saw_bytes) {
			if (apply_bytes_line(st, line, err, errlen) != 0)
				return -1;
			continue;
		}
		{
			char *colon = strchr(line, ':');
			char *key, *val;

			if (!colon)
				return fail(err, errlen,
					    "header line without ':' -- '%s'",
					    line);
			*colon = '\0';
			key = trim(line);
			val = trim(colon + 1);
			if (apply_header_kv(st, key, val, err, errlen) != 0)
				return -1;
		}
	}
	return 0;
}

int vms_fixture_load(const char *path, const char *manifest_path,
		     struct vms_fixture *out, char *err, size_t errlen)
{
	struct parse_state st;
	FILE *f;
	int rc;

	memset(out, 0, sizeof(*out));
	out->frame_index = -1;
	copy_field(out->path, sizeof(out->path), path);

	memset(&st, 0, sizeof(st));
	st.fx = out;

	f = fopen(path, "r");
	if (!f)
		return fail(err, errlen, "cannot open %s", path);
	rc = load_lines(f, &st, err, errlen);
	fclose(f);
	if (rc != 0)
		return -1;
	if (validate_required(&st, err, errlen) != 0)
		return -1;
	if (validate_origin(out, manifest_path, err, errlen) != 0)
		return -1;
	if (validate_digest(out, err, errlen) != 0)
		return -1;
	return 0;
}

int vms_fixture_is_cited(const struct vms_fixture *f, uint32_t off, uint32_t len)
{
	uint32_t i;

	for (i = 0; i < len; i++) {
		uint32_t p = off + i;
		unsigned s;
		int hit = 0;

		for (s = 0; s < f->n_cited; s++) {
			if (p >= f->cited[s].off &&
			    p < f->cited[s].off + f->cited[s].len) {
				hit = 1;
				break;
			}
		}
		if (!hit)
			return 0;
	}
	return 1;
}

uint32_t vms_fixture_cited_bytes(const struct vms_fixture *f)
{
	uint32_t total = 0;
	unsigned i;

	for (i = 0; i < f->n_cited; i++)
		total += f->cited[i].len;
	return total;
}

int vms_fixture_load_all(const char *dir, const char *manifest_path,
			 struct vms_fixture *out, size_t max,
			 char *err, size_t errlen)
{
	static char paths[VMS_FIXTURE_MAX_FILES][512];
	int n, i;

	n = vms_fixture_list(dir, paths, VMS_FIXTURE_MAX_FILES, err, errlen);
	if (n < 0)
		return -1;
	if ((size_t)n > max)
		return fail(err, errlen, "%d fixtures exceeds caller's %zu",
			    n, max);
	for (i = 0; i < n; i++) {
		char why[VMS_FIXTURE_ERRLEN];

		if (vms_fixture_load(paths[i], manifest_path, &out[i],
				     why, sizeof(why)) != 0)
			return fail(err, errlen, "%s: %s",
				    basename_of(paths[i]), why);
	}
	return n;
}

static int path_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

int vms_fixture_list(const char *dir, char paths[][512], size_t max,
		     char *err, size_t errlen)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	size_t n = 0;

	if (!d)
		return fail(err, errlen, "cannot open fixture dir %s", dir);
	while ((de = readdir(d)) != NULL) {
		size_t nl = strlen(de->d_name);

		if (nl < 5 || strcmp(de->d_name + nl - 5, ".spec") != 0)
			continue;
		if (n >= max) {
			closedir(d);
			return fail(err, errlen, "more than %zu fixtures", max);
		}
		snprintf(paths[n], 512, "%s/%s", dir, de->d_name);
		n++;
	}
	closedir(d);
	qsort(paths, n, 512, path_cmp);
	return (int)n;
}
