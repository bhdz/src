/*	$OpenBSD: mft.c,v 1.38 2021/09/09 14:15:49 claudio Exp $ */
/*
 * Copyright (c) 2019 Kristaps Dzonsons <kristaps@bsd.lv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <assert.h>
#include <err.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/asn1.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include "extern.h"

/*
 * Parse results and data of the manifest file.
 */
struct	parse {
	const char	*fn; /* manifest file name */
	struct mft	*res; /* result object */
};

static ASN1_OBJECT    *mft_oid;

static const char *
gentime2str(const ASN1_GENERALIZEDTIME *time)
{
	static char	buf[64];
	BIO		*mem;

	if ((mem = BIO_new(BIO_s_mem())) == NULL)
		cryptoerrx("BIO_new");
	if (!ASN1_GENERALIZEDTIME_print(mem, time))
		cryptoerrx("ASN1_GENERALIZEDTIME_print");
	if (BIO_gets(mem, buf, sizeof(buf)) < 0)
		cryptoerrx("BIO_gets");

	BIO_free(mem);
	return buf;
}

/*
 * Convert an ASN1_GENERALIZEDTIME to a struct tm.
 * Returns 1 on success, 0 on failure.
 */
static int
generalizedtime_to_tm(const ASN1_GENERALIZEDTIME *gtime, struct tm *tm)
{
	const char *data;
	size_t len;

	data = ASN1_STRING_get0_data(gtime);
	len = ASN1_STRING_length(gtime);

	memset(tm, 0, sizeof(*tm));
	return ASN1_time_parse(data, len, tm, V_ASN1_GENERALIZEDTIME) ==
	    V_ASN1_GENERALIZEDTIME;
}

/*
 * Validate and verify the time validity of the mft.
 * Returns 1 if all is good, 0 if mft is stale, any other case -1.
 */
static int
check_validity(const ASN1_GENERALIZEDTIME *from,
    const ASN1_GENERALIZEDTIME *until, const char *fn)
{
	time_t now = time(NULL);
	struct tm tm_from, tm_until, tm_now;

	if (gmtime_r(&now, &tm_now) == NULL) {
		warnx("%s: could not get current time", fn);
		return -1;
	}

	if (!generalizedtime_to_tm(from, &tm_from)) {
		warnx("%s: embedded from time format invalid", fn);
		return -1;
	}
	if (!generalizedtime_to_tm(until, &tm_until)) {
		warnx("%s: embedded until time format invalid", fn);
		return -1;
	}

	/* check that until is not before from */
	if (ASN1_time_tm_cmp(&tm_until, &tm_from) < 0) {
		warnx("%s: bad update interval", fn);
		return -1;
	}
	/* check that now is not before from */
	if (ASN1_time_tm_cmp(&tm_from, &tm_now) > 0) {
		warnx("%s: mft not yet valid %s", fn, gentime2str(from));
		return -1;
	}
	/* check that now is not after until */
	if (ASN1_time_tm_cmp(&tm_until, &tm_now) < 0) {
		warnx("%s: mft expired on %s", fn, gentime2str(until));
		return 0;
	}

	return 1;
}

/*
 * Parse an individual "FileAndHash", RFC 6486, sec. 4.2.
 * Return zero on failure, non-zero on success.
 */
static int
mft_parse_filehash(struct parse *p, const ASN1_OCTET_STRING *os)
{
	ASN1_SEQUENCE_ANY	*seq;
	const ASN1_TYPE		*file, *hash;
	char			*fn = NULL;
	const unsigned char	*d = os->data;
	size_t			 dsz = os->length;
	int			 rc = 0;
	struct mftfile		*fent;

	if ((seq = d2i_ASN1_SEQUENCE_ANY(NULL, &d, dsz)) == NULL) {
		cryptowarnx("%s: RFC 6486 section 4.2.1: FileAndHash: "
		    "failed ASN.1 sequence parse", p->fn);
		goto out;
	} else if (sk_ASN1_TYPE_num(seq) != 2) {
		warnx("%s: RFC 6486 section 4.2.1: FileAndHash: "
		    "want 2 elements, have %d", p->fn,
		    sk_ASN1_TYPE_num(seq));
		goto out;
	}

	/* First is the filename itself. */

	file = sk_ASN1_TYPE_value(seq, 0);
	if (file->type != V_ASN1_IA5STRING) {
		warnx("%s: RFC 6486 section 4.2.1: FileAndHash: "
		    "want ASN.1 IA5 string, have %s (NID %d)",
		    p->fn, ASN1_tag2str(file->type), file->type);
		goto out;
	}
	fn = strndup((const char *)file->value.ia5string->data,
	    file->value.ia5string->length);
	if (fn == NULL)
		err(1, NULL);

	/*
	 * Make sure we're just a pathname and either an ROA or CER.
	 * I don't think that the RFC specifically mentions this, but
	 * it's in practical use and would really screw things up
	 * (arbitrary filenames) otherwise.
	 */

	if (strchr(fn, '/') != NULL) {
		warnx("%s: path components disallowed in filename: %s",
		    p->fn, fn);
		goto out;
	} else if (strlen(fn) <= 4) {
		warnx("%s: filename must be large enough for suffix part: %s",
		    p->fn, fn);
		goto out;
	}

	/* Now hash value. */

	hash = sk_ASN1_TYPE_value(seq, 1);
	if (hash->type != V_ASN1_BIT_STRING) {
		warnx("%s: RFC 6486 section 4.2.1: FileAndHash: "
		    "want ASN.1 bit string, have %s (NID %d)",
		    p->fn, ASN1_tag2str(hash->type), hash->type);
		goto out;
	}

	if (hash->value.bit_string->length != SHA256_DIGEST_LENGTH) {
		warnx("%s: RFC 6486 section 4.2.1: hash: "
		    "invalid SHA256 length, have %d",
		    p->fn, hash->value.bit_string->length);
		goto out;
	}

	/* Insert the filename and hash value. */
	fent = &p->res->files[p->res->filesz++];

	fent->file = fn;
	fn = NULL;
	memcpy(fent->hash, hash->value.bit_string->data, SHA256_DIGEST_LENGTH);

	rc = 1;
out:
	free(fn);
	sk_ASN1_TYPE_pop_free(seq, ASN1_TYPE_free);
	return rc;
}

/*
 * Parse the "FileAndHash" sequence, RFC 6486, sec. 4.2.
 * Return zero on failure, non-zero on success.
 */
static int
mft_parse_flist(struct parse *p, const ASN1_OCTET_STRING *os)
{
	ASN1_SEQUENCE_ANY	*seq;
	const ASN1_TYPE		*t;
	const unsigned char	*d = os->data;
	size_t			 dsz = os->length;
	int			 i, rc = 0;

	if ((seq = d2i_ASN1_SEQUENCE_ANY(NULL, &d, dsz)) == NULL) {
		cryptowarnx("%s: RFC 6486 section 4.2: fileList: "
		    "failed ASN.1 sequence parse", p->fn);
		goto out;
	}

	p->res->files = calloc(sk_ASN1_TYPE_num(seq), sizeof(struct mftfile));
	if (p->res->files == NULL)
		err(1, NULL);

	for (i = 0; i < sk_ASN1_TYPE_num(seq); i++) {
		t = sk_ASN1_TYPE_value(seq, i);
		if (t->type != V_ASN1_SEQUENCE) {
			warnx("%s: RFC 6486 section 4.2: fileList: "
			    "want ASN.1 sequence, have %s (NID %d)",
			    p->fn, ASN1_tag2str(t->type), t->type);
			goto out;
		} else if (!mft_parse_filehash(p, t->value.octet_string))
			goto out;
	}

	rc = 1;
out:
	sk_ASN1_TYPE_pop_free(seq, ASN1_TYPE_free);
	return rc;
}

/*
 * Handle the eContent of the manifest object, RFC 6486 sec. 4.2.
 * Returns <0 on failure, 0 on stale, >0 on success.
 */
static int
mft_parse_econtent(const unsigned char *d, size_t dsz, struct parse *p)
{
	ASN1_SEQUENCE_ANY	*seq;
	const ASN1_TYPE		*t;
	const ASN1_GENERALIZEDTIME *from, *until;
	long			 mft_version;
	BIGNUM			*mft_seqnum = NULL;
	int			 i = 0, rc = -1;

	if ((seq = d2i_ASN1_SEQUENCE_ANY(NULL, &d, dsz)) == NULL) {
		cryptowarnx("%s: RFC 6486 section 4.2: Manifest: "
		    "failed ASN.1 sequence parse", p->fn);
		goto out;
	}

	/* Test if the optional profile version field is present. */
	if (sk_ASN1_TYPE_num(seq) != 5 &&
	    sk_ASN1_TYPE_num(seq) != 6) {
		warnx("%s: RFC 6486 section 4.2: Manifest: "
		    "want 5 or 6 elements, have %d", p->fn,
		    sk_ASN1_TYPE_num(seq));
		goto out;
	}

	/* Parse the optional version field */
	if (sk_ASN1_TYPE_num(seq) == 6) {
		t = sk_ASN1_TYPE_value(seq, i++);
		d = t->value.asn1_string->data;
		dsz = t->value.asn1_string->length;

		if (cms_econtent_version(p->fn, &d, dsz, &mft_version) == -1)
			goto out;

		switch (mft_version) {
		case 0:
			warnx("%s: incorrect encoding for version 0", p->fn);
			goto out;
		default:
			warnx("%s: version %ld not supported (yet)", p->fn,
			    mft_version);
			goto out;
		}
	}

	/* Now the manifest sequence number. */

	t = sk_ASN1_TYPE_value(seq, i++);
	if (t->type != V_ASN1_INTEGER) {
		warnx("%s: RFC 6486 section 4.2.1: manifestNumber: "
		    "want ASN.1 integer, have %s (NID %d)",
		    p->fn, ASN1_tag2str(t->type), t->type);
		goto out;
	}

	mft_seqnum = ASN1_INTEGER_to_BN(t->value.integer, NULL);
	if (mft_seqnum == NULL) {
		warnx("%s: ASN1_INTEGER_to_BN error", p->fn);
		goto out;
	}

	if (BN_is_negative(mft_seqnum)) {
		warnx("%s: RFC 6486 section 4.2.1: manifestNumber: "
		    "want positive integer, have negative.", p->fn);
		goto out;
	}

	if (BN_num_bytes(mft_seqnum) > 20) {
		warnx("%s: RFC 6486 section 4.2.1: manifestNumber: "
		    "want 20 or less than octets, have more.", p->fn);
		goto out;
	}

	p->res->seqnum = BN_bn2hex(mft_seqnum);
	if (p->res->seqnum == NULL) {
		warnx("%s: BN_bn2hex error", p->fn);
		goto out;
	}

	/*
	 * Timestamps: this and next update time.
	 * Validate that the current date falls into this interval.
	 * This is required by section 4.4, (3).
	 * If we're after the given date, then the MFT is stale.
	 * This is made super complicated because it uses OpenSSL's
	 * ASN1_GENERALIZEDTIME instead of ASN1_TIME, which we could
	 * compare against the current time trivially.
	 */

	t = sk_ASN1_TYPE_value(seq, i++);
	if (t->type != V_ASN1_GENERALIZEDTIME) {
		warnx("%s: RFC 6486 section 4.2.1: thisUpdate: "
		    "want ASN.1 generalised time, have %s (NID %d)",
		    p->fn, ASN1_tag2str(t->type), t->type);
		goto out;
	}
	from = t->value.generalizedtime;

	t = sk_ASN1_TYPE_value(seq, i++);
	if (t->type != V_ASN1_GENERALIZEDTIME) {
		warnx("%s: RFC 6486 section 4.2.1: nextUpdate: "
		    "want ASN.1 generalised time, have %s (NID %d)",
		    p->fn, ASN1_tag2str(t->type), t->type);
		goto out;
	}
	until = t->value.generalizedtime;

	rc = check_validity(from, until, p->fn);
	if (rc != 1)
		goto out;

	/* The mft is valid. Reset rc so later 'goto out' return failure. */
	rc = -1;

	/* File list algorithm. */

	t = sk_ASN1_TYPE_value(seq, i++);
	if (t->type != V_ASN1_OBJECT) {
		warnx("%s: RFC 6486 section 4.2.1: fileHashAlg: "
		    "want ASN.1 object time, have %s (NID %d)",
		    p->fn, ASN1_tag2str(t->type), t->type);
		goto out;
	} else if (OBJ_obj2nid(t->value.object) != NID_sha256) {
		warnx("%s: RFC 6486 section 4.2.1: fileHashAlg: "
		    "want SHA256 object, have %s (NID %d)", p->fn,
		    ASN1_tag2str(OBJ_obj2nid(t->value.object)),
		    OBJ_obj2nid(t->value.object));
		goto out;
	}

	/* Now the sequence. */

	t = sk_ASN1_TYPE_value(seq, i++);
	if (t->type != V_ASN1_SEQUENCE) {
		warnx("%s: RFC 6486 section 4.2.1: fileList: "
		    "want ASN.1 sequence, have %s (NID %d)",
		    p->fn, ASN1_tag2str(t->type), t->type);
		goto out;
	} else if (!mft_parse_flist(p, t->value.octet_string))
		goto out;

	rc = 1;
out:
	sk_ASN1_TYPE_pop_free(seq, ASN1_TYPE_free);
	BN_free(mft_seqnum);
	return rc;
}

/*
 * Parse the objects that have been published in the manifest.
 * This conforms to RFC 6486.
 * Note that if the MFT is stale, all referenced objects are stripped
 * from the parsed content.
 * The MFT content is otherwise returned.
 */
struct mft *
mft_parse(X509 **x509, const char *fn)
{
	struct parse	 p;
	int		 c, rc = 0;
	size_t		 i, cmsz;
	unsigned char	*cms;

	memset(&p, 0, sizeof(struct parse));
	p.fn = fn;

	if (mft_oid == NULL) {
		mft_oid = OBJ_txt2obj("1.2.840.113549.1.9.16.1.26", 1);
		if (mft_oid == NULL)
			errx(1, "OBJ_txt2obj for %s failed",
			    "1.2.840.113549.1.9.16.1.26");
	}

	cms = cms_parse_validate(x509, fn, mft_oid, &cmsz);
	if (cms == NULL)
		return NULL;
	assert(*x509 != NULL);

	if ((p.res = calloc(1, sizeof(struct mft))) == NULL)
		err(1, NULL);
	if ((p.res->file = strdup(fn)) == NULL)
		err(1, NULL);

	p.res->aia = x509_get_aia(*x509, fn);
	p.res->aki = x509_get_aki(*x509, 0, fn);
	p.res->ski = x509_get_ski(*x509, fn);
	if (p.res->aia == NULL || p.res->aki == NULL || p.res->ski == NULL) {
		warnx("%s: RFC 6487 section 4.8: "
		    "missing AIA, AKI or SKI X509 extension", fn);
		goto out;
	}

	/*
	 * If we're stale, then remove all of the files that the MFT
	 * references as well as marking it as stale.
	 */

	if ((c = mft_parse_econtent(cms, cmsz, &p)) == 0) {
		/*
		 * FIXME: it should suffice to just mark this as stale
		 * and have the logic around mft_read() simply ignore
		 * the contents of stale entries, just like it does for
		 * invalid ROAs or certificates.
		 */

		p.res->stale = 1;
		if (p.res->files != NULL)
			for (i = 0; i < p.res->filesz; i++)
				free(p.res->files[i].file);
		free(p.res->files);
		p.res->filesz = 0;
		p.res->files = NULL;
	} else if (c == -1)
		goto out;

	rc = 1;
out:
	if (rc == 0) {
		mft_free(p.res);
		p.res = NULL;
		X509_free(*x509);
		*x509 = NULL;
	}
	free(cms);
	return p.res;
}

/*
 * Check all files and their hashes in a MFT structure.
 * Return zero on failure, non-zero on success.
 */
int
mft_check(const char *fn, struct mft *p)
{
	size_t	i;
	int	rc = 1;
	char	*cp, *path = NULL;

	/* Check hash of file now, but first build path for it */
	cp = strrchr(fn, '/');
	assert(cp != NULL);
	assert(cp - fn < INT_MAX);

	for (i = 0; i < p->filesz; i++) {
		const struct mftfile *m = &p->files[i];
		if (asprintf(&path, "%.*s/%s", (int)(cp - fn), fn,
		    m->file) == -1)
			err(1, NULL);
		if (!valid_filehash(path, m->hash, sizeof(m->hash))) {
			warnx("%s: bad message digest for %s", fn, m->file);
			rc = 0;
		}
		free(path);
	}

	return rc;
}

/*
 * Free an MFT pointer.
 * Safe to call with NULL.
 */
void
mft_free(struct mft *p)
{
	size_t	 i;

	if (p == NULL)
		return;

	if (p->files != NULL)
		for (i = 0; i < p->filesz; i++)
			free(p->files[i].file);

	free(p->aia);
	free(p->aki);
	free(p->ski);
	free(p->file);
	free(p->files);
	free(p->seqnum);
	free(p);
}

/*
 * Serialise MFT parsed content into the given buffer.
 * See mft_read() for the other side of the pipe.
 */
void
mft_buffer(struct ibuf *b, const struct mft *p)
{
	size_t		 i;

	io_simple_buffer(b, &p->stale, sizeof(int));
	io_str_buffer(b, p->file);
	io_simple_buffer(b, &p->filesz, sizeof(size_t));

	for (i = 0; i < p->filesz; i++) {
		io_str_buffer(b, p->files[i].file);
		io_simple_buffer(b, p->files[i].hash, SHA256_DIGEST_LENGTH);
	}

	io_str_buffer(b, p->aia);
	io_str_buffer(b, p->aki);
	io_str_buffer(b, p->ski);
}

/*
 * Read an MFT structure from the file descriptor.
 * Result must be passed to mft_free().
 */
struct mft *
mft_read(int fd)
{
	struct mft	*p = NULL;
	size_t		 i;

	if ((p = calloc(1, sizeof(struct mft))) == NULL)
		err(1, NULL);

	io_simple_read(fd, &p->stale, sizeof(int));
	io_str_read(fd, &p->file);
	assert(p->file);
	io_simple_read(fd, &p->filesz, sizeof(size_t));

	if ((p->files = calloc(p->filesz, sizeof(struct mftfile))) == NULL)
		err(1, NULL);

	for (i = 0; i < p->filesz; i++) {
		io_str_read(fd, &p->files[i].file);
		io_simple_read(fd, p->files[i].hash, SHA256_DIGEST_LENGTH);
	}

	io_str_read(fd, &p->aia);
	io_str_read(fd, &p->aki);
	io_str_read(fd, &p->ski);
	assert(p->aia && p->aki && p->ski);

	return p;
}
